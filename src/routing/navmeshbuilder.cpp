/*
    SPDX-FileCopyrightText: 2024 Volker Krause <vkrause@kde.org>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "navmeshbuilder.h"

#include "navmesh.h"
#include "navmesh_p.h"
#include "navmeshtransform.h"
#include "recastnav_p.h"
#include "recastnavsettings_p.h"
#include <logging.h>

#include <KOSMIndoorMap/MapData>
#include <KOSMIndoorMap/MapCSSParser>
#include <KOSMIndoorMap/MapCSSResult>
#include <KOSMIndoorMap/MapCSSStyle>
#include <KOSMIndoorMap/OverlaySource>

#include <loader/levelparser_p.h>
#include <scene/penwidthutil_p.h>
#include <scene/scenegraphitem.h>
#include <style/mapcssdeclaration_p.h>
#include <style/mapcssstate_p.h>

#include <QFile>
#include <QPolygonF>
#include <QPainterPath>
#include <QThreadPool>

#include <private/qtriangulator_p.h>
#include <private/qtriangulatingstroker_p.h>

#if HAVE_RECAST
#include <DetourNavMeshBuilder.h>
#endif

#include <cmath>
#include <unordered_map>

namespace KOSMIndoorRouting {

enum class LinkDirection { Forward, Backward, Bidirectional };

class NavMeshBuilderPrivate
{
public:
    /** Look up level for a given node id. */
    [[nodiscard]] int levelForNode(OSM::Id nodeId) const;
    void addNodeToLevelIndex(OSM::Id nodeId, int level);
    void indexNodeLevels();

    void processElement(OSM::Element elem, int floorLevel);
    void processGeometry(OSM::Element elem, int floorLevel, const KOSMIndoorMap::MapCSSResultLayer &res);
    void processLink(OSM::Element elem, int floorLevel, LinkDirection linkDir, const KOSMIndoorMap::MapCSSResultLayer &res);

    void addVertex(float x, float y, float z);
    void addFace(std::size_t i, std::size_t j, std::size_t k, AreaType areaType);
    void addOffMeshConnection(float x1, float y1, float z1, float x2, float y2, float z2, LinkDirection linkDir, AreaType areaType);

    void buildNavMesh();

    void writeGsetFile();
    void writeObjFile();

    KOSMIndoorMap::MapData m_data;
    KOSMIndoorMap::MapCSSStyle m_style;
    KOSMIndoorMap::MapCSSResult m_filterResult;

    NavMeshTransform m_transform;

    std::unordered_map<OSM::Id, int> m_nodeLevelMap;
    KOSMIndoorMap::AbstractOverlaySource *m_equipmentModel = nullptr;

    // triangle data
    std::vector<float> m_verts;
    inline int numVerts() const { return (int)m_verts.size() / 3; }
    std::vector<int> m_tris;
    inline int numTris() const { return (int)m_tris.size() / 3; }
    std::vector<uint8_t> m_triAreaIds;

    // off mesh connection data
    struct {
        std::vector<float> verts;
        std::vector<float> rads;
        std::vector<uint16_t> flags;
        std::vector<uint8_t> areas;
        std::vector<uint8_t> dir;
        std::vector<uint32_t> userId;
    } m_offMeshCon;
    inline int offMeshCount() const { return (int) m_offMeshCon.rads.size(); }

    NavMesh m_navMesh;

    // diganostic obj output
    QString m_gsetFileName;
    QString m_objFileName;
    qsizetype m_vertexOffset = 0;
};
}

using namespace KOSMIndoorRouting;

//BEGIN TODO largely copied from SceneController, refactor/unify?
static QPolygonF createPolygon(const OSM::DataSet &dataSet, OSM::Element e)
{
    const auto path = e.outerPath(dataSet);
    if (path.empty()) {
        return {};
    }

    QPolygonF poly;
    // Element::outerPath takes care of re-assembling broken up line segments
    // the below takes care of properly merging broken up polygons
    for (auto it = path.begin(); it != path.end();) {
        QPolygonF subPoly;
        subPoly.reserve(path.size());
        OSM::Id pathBegin = (*it)->id;

        auto subIt = it;
        for (; subIt != path.end(); ++subIt) {
            subPoly.push_back(QPointF((*subIt)->coordinate.lonF(), (*subIt)->coordinate.latF()));
            if ((*subIt)->id == pathBegin && subIt != it && subIt != std::prev(path.end())) {
                ++subIt;
                break;
            }
        }
        it = subIt;
        poly = poly.isEmpty() ? std::move(subPoly) : poly.united(subPoly);
    }
    return poly;
}

// @see https://wiki.openstreetmap.org/wiki/Relation:multipolygon
static QPainterPath createPath(const OSM::DataSet &dataSet, const OSM::Element e)
{
    assert(e.type() == OSM::Type::Relation);
    QPolygonF outerPath = createPolygon(dataSet, e); // TODO this is actually not correct for the multiple outer polygon case
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);

    for (const auto &mem : e.relation()->members) {
        const bool isInner = std::strcmp(mem.role().name(), "inner") == 0;
        const bool isOuter = std::strcmp(mem.role().name(), "outer") == 0;
        if (mem.type() != OSM::Type::Way || (!isInner && !isOuter)) {
            continue;
        }
        if (auto way = dataSet.way(mem.id)) {
            const auto subPoly = createPolygon(dataSet, OSM::Element(way));
            if (subPoly.isEmpty()) {
                continue;
            }
            path.addPolygon(subPoly);
            path.closeSubpath();
        }
    }

    return path;
}
//END

NavMeshBuilder::NavMeshBuilder(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<NavMeshBuilderPrivate>())
{
}

NavMeshBuilder::~NavMeshBuilder() = default;

void NavMeshBuilder::setMapData(const KOSMIndoorMap::MapData &mapData)
{
    d->m_data = mapData;

    if (d->m_style.isEmpty()) {
        KOSMIndoorMap::MapCSSParser p;
        d->m_style = p.parse(QStringLiteral(":/org.kde.kosmindoorrouting/navmesh-filter.mapcss"));
        if (p.hasError()) {
            qWarning() << p.errorMessage();
            return;
        }
    }

    if (!d->m_data.isEmpty()) {
        d->m_style.compile(d->m_data.dataSet());
    }
}

void NavMeshBuilder::setEquipmentModel(KOSMIndoorMap::AbstractOverlaySource *equipmentModel)
{
    d->m_equipmentModel = equipmentModel;
    // TODO can we do incremental updates when a realtime elevator status changes?
}

void NavMeshBuilder::writeDebugNavMesh(const QString &gsetFile, const QString &objFile)
{
    d->m_gsetFileName = gsetFile;
    d->m_objFileName = objFile;
}

static bool isDoor(const OSM::Node *node)
{
    return !OSM::tagValue(*node, "door").isEmpty();
}

int NavMeshBuilderPrivate::levelForNode(OSM::Id nodeId) const
{
    const auto it = m_nodeLevelMap.find(nodeId);
    return it != m_nodeLevelMap.end() ? (*it).second : 0;
}

void NavMeshBuilderPrivate::addNodeToLevelIndex(OSM::Id nodeId, int level)
{
    auto it = m_nodeLevelMap.find(nodeId);
    if (it == m_nodeLevelMap.end()) {
        m_nodeLevelMap[nodeId] = level;
        return;
    }
    if ((*it).second != level) {
        (*it).second = std::numeric_limits<int>::min();
    }
}

void NavMeshBuilderPrivate::indexNodeLevels()
{
    for (const auto &level : m_data.levelMap()) {
        if (level.first.numericLevel() == 0) {
            continue;
        }
        for (const auto elem : level.second) {
            switch (elem.type()) {
                case OSM::Type::Null:
                    Q_UNREACHABLE();
                case OSM::Type::Node:
                    continue;
                case OSM::Type::Way:
                {
                    // TODO ignore multi-level ways
                    const auto lvl = elem.tagValue("level");
                    if (lvl.isEmpty() || lvl.contains(';')) {
                        break;
                    }
                    for (OSM::Id nodeId : elem.way()->nodes) {
                        addNodeToLevelIndex(nodeId, level.first.numericLevel());
                    }
                    break;
                }
                case OSM::Type::Relation:
                    // TODO
                    break;
            }
        }
    }
}

void NavMeshBuilder::start()
{
    // the first half of this where we access m_data runs in the main thread (as MapData isn't prepared for multi-threaded access)
    qCDebug(Log) << QThread::currentThread();

    d->m_transform.initialize(d->m_data.boundingBox());
    d->indexNodeLevels();

    std::vector<OSM::Element> hiddenElements;
    d->m_equipmentModel->hiddenElements(hiddenElements);
    std::sort(hiddenElements.begin(), hiddenElements.end());

    for (const auto &level : d->m_data.levelMap()) {
        for (const auto &elem : level.second) {
            if (std::binary_search(hiddenElements.begin(), hiddenElements.end(), elem)) {
                continue;
            }
            d->processElement(elem, level.first.numericLevel());
        }

        if (level.first.numericLevel() % 10) {
            continue;
        }
        d->m_equipmentModel->forEach(level.first.numericLevel(), [this](OSM::Element elem, int floorLevel) {
            d->processElement(elem, floorLevel);
        });
    }

    [[unlikely]] if (!d->m_gsetFileName.isEmpty()) {
        d->writeGsetFile();
        d->writeObjFile();
    }

    qCDebug(Log) << "Vertex data size:" << d->m_verts.size() * sizeof(float);
    qCDebug(Log) << "Triangle index size:" << d->m_tris.size() * sizeof(int);
    qCDebug(Log) << "Triangle area size:" << d->m_triAreaIds.size();
    qCDebug(Log) << "Off-mesh data size:" << d->offMeshCount() * 16;

    // the second half of this (which takes the majority of the time) runs in a secondary thread
    QThreadPool::globalInstance()->start([this]() {
        d->buildNavMesh();
        QMetaObject::invokeMethod(this, &NavMeshBuilder::finished, Qt::QueuedConnection);
    });
}

void NavMeshBuilderPrivate::processElement(OSM::Element elem, int floorLevel)
{
    KOSMIndoorMap::MapCSSState filterState;
    filterState.element = elem;
    m_style.initializeState(filterState);
    m_style.evaluate(filterState, m_filterResult);

    for (const auto &res : m_filterResult.results()) {
        if (res.layerSelector().isNull()) {
            processGeometry(elem, floorLevel, res);
        } else {
            LinkDirection linkDir = LinkDirection::Bidirectional; // TODO use precompiled keys
            if (std::strcmp(res.layerSelector().name(), "link_forward") == 0) {
                linkDir = LinkDirection::Forward;
            } else if (std::strcmp(res.layerSelector().name(), "link_backward") == 0) {
                linkDir = LinkDirection::Backward;
            }
            processLink(elem, floorLevel, linkDir, res);
        }
    }
}

void NavMeshBuilderPrivate::processGeometry(OSM::Element elem, int floorLevel, const KOSMIndoorMap::MapCSSResultLayer &res)
{
    if (res.hasAreaProperties()) {
        const auto prop = res.declaration(KOSMIndoorMap::MapCSSProperty::FillOpacity);
        if (prop && prop->doubleValue() > 0.0) {
            QPainterPath path;
            if (elem.type() == OSM::Type::Relation) {
                path = createPath(m_data.dataSet(), elem);
            } else {
                path.addPolygon(createPolygon(m_data.dataSet(), elem));
            }

            QPainterPath p;
            const auto triSet = qTriangulate(m_transform.mapGeoToNav(path));
            qCDebug(Log) << "A" << elem.url() << m_transform.mapGeoToNav(path).boundingRect() << path.elementCount() << triSet.indices.size() << triSet.vertices.size() << m_vertexOffset << floorLevel;

            for (qsizetype i = 0; i < triSet.vertices.size(); i += 2) {
                addVertex(triSet.vertices[i], m_transform.mapHeightToNav(floorLevel), triSet.vertices[i + 1]);
            }
            if (triSet.indices.type() == QVertexIndexVector::UnsignedShort) {
                for (qsizetype i = 0; i <triSet.indices.size(); i += 3) {
                    addFace(*(reinterpret_cast<const uint16_t*>(triSet.indices.data()) + i) + m_vertexOffset,
                            *(reinterpret_cast<const uint16_t*>(triSet.indices.data()) + i + 1) + m_vertexOffset,
                            *(reinterpret_cast<const uint16_t*>(triSet.indices.data()) + i + 2) + m_vertexOffset,
                            AreaType::Walkable); // TODO area type from CSS
                }
            } else if (triSet.indices.type() == QVertexIndexVector::UnsignedInt) {
                for (qsizetype i = 0; i <triSet.indices.size(); i += 3) {
                    addFace(*(reinterpret_cast<const uint32_t*>(triSet.indices.data()) + i) + m_vertexOffset,
                            *(reinterpret_cast<const uint32_t*>(triSet.indices.data()) + i + 1) + m_vertexOffset,
                            *(reinterpret_cast<const uint32_t*>(triSet.indices.data()) + i + 2) + m_vertexOffset,
                            AreaType::Walkable); // TODO area type from CSS
                }
            }
            m_vertexOffset += triSet.vertices.size() / 2;
        }
    }

    if (res.hasLineProperties()) {
        const auto prop = res.declaration(KOSMIndoorMap::MapCSSProperty::Width);
        KOSMIndoorMap::Unit dummyUnit;
        if (const auto penWidth = prop ? KOSMIndoorMap::PenWidthUtil::penWidth(elem, prop, dummyUnit) : 0.0; penWidth > 0.0) {
            QPolygonF poly = m_transform.mapGeoToNav(createPolygon(m_data.dataSet(), elem));
            QPainterPath path;
            path.addPolygon(poly);
            QPen pen;
            // TODO join/cap styles
            pen.setCapStyle(Qt::FlatCap);
            pen.setWidthF(penWidth);

            QTriangulatingStroker stroker;
            stroker.process(qtVectorPathForPath(path), pen, {}, {});
            qCDebug(Log) << "L" << elem.url() << stroker.vertexCount() << pen.widthF();

            for (int i = 0; i < stroker.vertexCount(); i += 2) {
                auto l = floorLevel;
                if (poly.size() == 2 && elem.type() == OSM::Type::Way) { // TODO can we generalize this?
                    const auto way = elem.way();
                    const auto l1 = levelForNode(way->nodes.at(0));
                    const auto l2 = levelForNode(way->nodes.at(1));
                    qCDebug(Log) << "  S" << elem.url() << floorLevel << l1 << l2;
                    if (l1 != l2 && l1 != std::numeric_limits<int>::min() && l2 != std::numeric_limits<int>::min()) {
                        QPointF p(*(stroker.vertices() + i), *(stroker.vertices() + i + 1));
                        l = QLineF(poly.at(0), p).length() < QLineF(poly.at(1), p).length() ? l1 : l2;
                    }
                }
                addVertex(*(stroker.vertices() + i), m_transform.mapHeightToNav(l), *(stroker.vertices() + i + 1));
            }
            for (int i = 0; i < stroker.vertexCount() / 2 - 2; ++i) {
                // GL_TRIANGLE_STRIP winding order
                if (i % 2) {
                    addFace(m_vertexOffset + i, m_vertexOffset + i + 1, m_vertexOffset + i + 2, AreaType::Walkable);  // TODO area type from CSS
                } else {
                    addFace(m_vertexOffset + i + 1, m_vertexOffset + i, m_vertexOffset + i + 2, AreaType::Walkable);  // TODO area type from CSS
                }
            }
            m_vertexOffset += stroker.vertexCount() / 2;
        }
    }

    if (res.hasExtrudeProperties()) {
        const auto prop = res.declaration(KOSMIndoorMap::MapCSSProperty::Extrude);
        if (prop && prop->doubleValue() > 0.0) {
            const auto way = elem.outerPath(m_data.dataSet());
            for (std::size_t i = 0; i < way.size() - 1; ++i) {
                if (isDoor(way[i]) || isDoor(way[i + 1])) {
                    continue;
                }
                const auto p1 = m_transform.mapGeoToNav(way[i]->coordinate);
                const auto p2 = m_transform.mapGeoToNav(way[i + 1]->coordinate);
                addVertex(p1.x(), m_transform.mapHeightToNav(floorLevel), p1.y());
                addVertex(p2.x(), m_transform.mapHeightToNav(floorLevel), p2.y());
                addVertex(p1.x(), m_transform.mapHeightToNav(floorLevel + 10), p1.y());
                addVertex(p2.x(), m_transform.mapHeightToNav(floorLevel + 10), p2.y());
                addFace(m_vertexOffset, m_vertexOffset + 1, m_vertexOffset + 2, AreaType::Unwalkable);
                addFace(m_vertexOffset + 1, m_vertexOffset + 3, m_vertexOffset + 2, AreaType::Unwalkable);
                m_vertexOffset += 4;
            }
        }
    }
}

void NavMeshBuilderPrivate::processLink(OSM::Element elem, int floorLevel, LinkDirection linkDir, const KOSMIndoorMap::MapCSSResultLayer &res)
{
    if (res.hasAreaProperties()) {
        std::vector<int> levels;
        KOSMIndoorMap::LevelParser::parse(elem.tagValue("level"), elem, [&levels](int level, auto) { levels.push_back(level); });
        if (levels.size() > 1) {
            qDebug() << "E" << elem.url() << levels;
            // TODO doesn't work for concave polygons!
            const QPointF p = m_transform.mapGeoToNav(elem.center());
            for (std::size_t i = 0; i < levels.size() - 1; ++i) {
                addOffMeshConnection(p.x(), m_transform.mapHeightToNav(levels[i]), p.y(), p.x(), m_transform.mapHeightToNav(levels[i + 1]), p.y(), LinkDirection::Bidirectional, AreaType::Elevator); // TODO area type from MapCSS
            }
        }
    }
    if (res.hasLineProperties() && elem.type() == OSM::Type::Way) {
        const auto way = elem.way();
        if (way->nodes.size() == 2) {
            const auto l1 = levelForNode(way->nodes.at(0));
            const auto l2 = levelForNode(way->nodes.at(1));
            qCDebug(Log) << "  LINK" << elem.url() << floorLevel << l1 << l2;
            if (l1 != l2 && l1 != std::numeric_limits<int>::min() && l2 != std::numeric_limits<int>::min()) {
                const auto poly = createPolygon(m_data.dataSet(), elem);
                const auto p1 = m_transform.mapGeoToNav(poly.at(0));
                const auto p2 = m_transform.mapGeoToNav(poly.at(1));
                addOffMeshConnection(p1.x(), m_transform.mapHeightToNav(l1), p1.y(), p2.x(), m_transform.mapHeightToNav(l2), p2.y(), linkDir, AreaType::Escalator); // TODO area type from MapCSS
            }
        }
    }
}

void NavMeshBuilderPrivate::addVertex(float x, float y, float z)
{
    for (const auto v : {x, y, z}) {
        m_verts.push_back(v);
    }
}

void NavMeshBuilderPrivate::addFace(std::size_t i, std::size_t j, std::size_t k, AreaType areaType)
{
    for (const auto v : {i, j, k}) {
        m_tris.push_back((int)v);
    }
    m_triAreaIds.push_back(qToUnderlying(areaType));
}

void NavMeshBuilderPrivate::addOffMeshConnection(float x1, float y1, float z1, float x2, float y2, float z2, LinkDirection linkDir, AreaType areaType)
{
    if (linkDir == LinkDirection::Backward) {
        std::swap(x1, x2);
        std::swap(y1, y2);
        std::swap(z1, z2);
        linkDir = LinkDirection::Forward;
    }

    for (const auto v : { x1, y1, z1, x2, y2, z2 }) {
        m_offMeshCon.verts.push_back(v);
    }
    m_offMeshCon.rads.push_back(0.6); // ???
    m_offMeshCon.flags.push_back(8); // ???
    m_offMeshCon.areas.push_back(qToUnderlying(areaType));
    m_offMeshCon.dir.push_back(linkDir == LinkDirection::Bidirectional ? 1 : 0);
    m_offMeshCon.userId.push_back(0); // ???
}

void NavMeshBuilderPrivate::writeGsetFile()
{
    QFile f(m_gsetFileName);
    f.open(QFile::WriteOnly);
    f.write("f ");
    f.write(m_objFileName.toUtf8());
    f.write("\n");

    f.write("s ");
    f.write(QByteArray::number(RECAST_CELL_SIZE));
    f.write(" ");
    f.write(QByteArray::number(RECAST_CELL_HEIGHT));
    f.write(" ");

    f.write(QByteArray::number(RECAST_AGENT_HEIGHT));
    f.write(" ");
    f.write(QByteArray::number(RECAST_AGENT_RADIUS));
    f.write(" ");
    f.write(QByteArray::number(RECAST_AGENT_MAX_CLIMB));
    f.write(" ");
    f.write(QByteArray::number(RECAST_AGENT_MAX_SLOPE));
    f.write(" ");

    f.write(QByteArray::number(RECAST_REGION_MIN_AREA));
    f.write(" ");
    f.write(QByteArray::number(RECAST_REGION_MERGE_AREA));
    f.write(" ");
    f.write(QByteArray::number(RECAST_MAX_EDGE_LEN));
    f.write(" ");
    f.write(QByteArray::number(RECAST_MAX_SIMPLIFICATION_ERROR));
    f.write(" 6 "); // vertex per polygon
    f.write(QByteArray::number(RECAST_DETAIL_SAMPLE_DIST));
    f.write(" ");
    f.write(QByteArray::number(RECAST_DETAIL_SAMPLE_MAX_ERROR));
    f.write(" ");
    f.write(QByteArray::number(qToUnderlying(RECAST_PARTITION_TYPE))); // partition type
    f.write(" ");

    // bbox min
    auto p = m_transform.mapGeoToNav(m_data.boundingBox().min);
    f.write(QByteArray::number(p.x()));
    f.write(" ");
    f.write(QByteArray::number(std::prev(m_data.levelMap().end())->first.numericLevel()));
    f.write(" ");
    f.write(QByteArray::number(p.y()));
    f.write(" ");

    // bbox max
    p = m_transform.mapGeoToNav(m_data.boundingBox().max);
    f.write(QByteArray::number(p.x()));
    f.write(" ");
    f.write(QByteArray::number(m_data.levelMap().begin()->first.numericLevel()));
    f.write(" ");
    f.write(QByteArray::number(p.y()));
    f.write(" ");

    f.write("0\n"); // tile size?

    for (int i = 0; i < offMeshCount(); ++i) {
        f.write("c ");
        for (int j = 0; j < 6; ++j) {
            f.write(QByteArray::number(m_offMeshCon.verts[i * 6 + j]));
            f.write(" ");
        }
        f.write(QByteArray::number(m_offMeshCon.rads[i]));
        f.write(" ");
        f.write(QByteArray::number(m_offMeshCon.dir[i]));
        f.write(" ");
        f.write(QByteArray::number(m_offMeshCon.areas[i]));
        f.write(" ");
        f.write(QByteArray::number(m_offMeshCon.flags[i]));
        f.write("\n");
    }
}

void NavMeshBuilderPrivate::writeObjFile()
{
    QFile f(m_objFileName);
    f.open(QFile::WriteOnly);

    for (std::size_t i = 0; i < m_verts.size(); i += 3) {
        f.write("v ");
        f.write(QByteArray::number(m_verts[i]));
        f.write(" ");
        f.write(QByteArray::number(m_verts[i+1]));
        f.write(" ");
        f.write(QByteArray::number(m_verts[i+2]));
        f.write("\n");
    }

    for (std::size_t i = 0; i < m_tris.size(); i += 3) {
        f.write("f ");
        f.write(QByteArray::number(m_tris[i] + 1));
        f.write(" ");
        f.write(QByteArray::number(m_tris[i+1] + 1));
        f.write(" ");
        f.write(QByteArray::number(m_tris[i+2] + 1));
        f.write("\n");
    }
}

void NavMeshBuilderPrivate::buildNavMesh()
{
    qCDebug(Log) << QThread::currentThread();

    const auto bmin = m_transform.mapGeoHeightToNav(m_data.boundingBox().min, std::prev(m_data.levelMap().end())->first.numericLevel());
    const auto bmax = m_transform.mapGeoHeightToNav(m_data.boundingBox().max, m_data.levelMap().begin()->first.numericLevel());

    NavMesh resultData;
    const auto result = NavMeshPrivate::create(resultData);
    result->m_transform = m_transform;

    // steps as defined in the Recast demo app
#if HAVE_RECAST
    // step 1: setup
    rcContext ctx;
    int width = 0;
    int height = 0;
    rcCalcGridSize(bmin, bmax, RECAST_CELL_SIZE, &width, &height);
    qCDebug(Log) << width << "x" << height << "cells";

    const auto walkableHeight = (int)std::ceil(RECAST_AGENT_HEIGHT / RECAST_CELL_HEIGHT);
    const auto walkableClimb = (int)std::floor(RECAST_AGENT_MAX_CLIMB/ RECAST_CELL_HEIGHT);
    const auto walkableRadius = (int)std::ceil(RECAST_AGENT_RADIUS / RECAST_CELL_SIZE);

    // step 2: build input polygons
    rcHeightfieldPtr solid(rcAllocHeightfield());
    if (!rcCreateHeightfield(&ctx, *solid, width, height, bmin, bmax, RECAST_CELL_SIZE, RECAST_CELL_HEIGHT)) {
        qCWarning(Log) << "Failed to create solid heightfield.";
        return;
    }

    if (!rcRasterizeTriangles(&ctx, m_verts.data(), numVerts(), m_tris.data(), m_triAreaIds.data(), numTris(), *solid, walkableClimb)) {
        qCWarning(Log) << "Failed to rasterize triangles";
        return;
    }

    // step 3: filter walkable sufaces

    rcFilterLowHangingWalkableObstacles(&ctx, walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, walkableHeight, walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, walkableHeight, *solid);

    // step 4: partition surface into regions
    rcCompactHeightfieldPtr chf(rcAllocCompactHeightfield());
    if (!rcBuildCompactHeightfield(&ctx, walkableHeight, walkableClimb, *solid, *chf)) {
        qCWarning(Log) << "Failed to build compact height field.";
        return;
    }
    solid.reset();

    if (!rcErodeWalkableArea(&ctx, walkableRadius, *chf)) {
        qCWarning(Log) << "Failed to erode walkable area";
        return;
    }

    if (!rcBuildRegionsMonotone(&ctx, *chf, 0, RECAST_REGION_MIN_AREA, RECAST_REGION_MERGE_AREA)) {
        qCWarning(Log) << "Failed to build monotone regions";
        return;
    }

    // step 5: create contours
    rcContourSetPtr cset(rcAllocContourSet());
    if (!rcBuildContours(&ctx, *chf, RECAST_MAX_SIMPLIFICATION_ERROR, RECAST_MAX_EDGE_LEN, *cset)) {
        qCWarning(Log) << "Failed to create contours.";
        return;
    }

    // step 6: create polygon mesh from countours
    rcPolyMeshPtr pmesh(rcAllocPolyMesh());
    if (!rcBuildPolyMesh(&ctx, *cset, DT_VERTS_PER_POLYGON, *pmesh)) {
        qCWarning(Log) << "Failed to triangulate contours";
        return;
    }

    // step 7: create detail mesh
    rcPolyMeshDetailPtr dmesh(rcAllocPolyMeshDetail());
    if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, RECAST_DETAIL_SAMPLE_DIST, RECAST_DETAIL_SAMPLE_MAX_ERROR, *dmesh)) {
        qCWarning(Log) << "Failed to build detail mesh";
        return;
    }
    chf.reset();
    cset.reset();

    // step 8 create detour data
    uint8_t *navData = nullptr;
    int navDataSize = 0;

    // TODO proper polygon flag update
    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == RC_WALKABLE_AREA) {
            pmesh->flags[i] = 0x01;
        }
    }

    dtNavMeshCreateParams params;
    std::memset(&params, 0, sizeof(params));
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.offMeshConVerts = m_offMeshCon.verts.data();
    params.offMeshConRad = m_offMeshCon.rads.data();
    params.offMeshConDir = m_offMeshCon.dir.data();
    params.offMeshConAreas = m_offMeshCon.areas.data();
    params.offMeshConFlags = m_offMeshCon.flags.data();
    params.offMeshConUserID = m_offMeshCon.userId.data();
    params.offMeshConCount = offMeshCount();
    params.walkableHeight = RECAST_AGENT_HEIGHT;
    params.walkableRadius = RECAST_AGENT_RADIUS;
    params.walkableClimb = RECAST_AGENT_MAX_CLIMB;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = RECAST_CELL_SIZE;
    params.ch = RECAST_CELL_HEIGHT;
    params.buildBvTree = true;

    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) {
        qCWarning(Log) << "dtCreateNavMeshData failed";
        return;
    }
    std::unique_ptr<uint8_t> navDataPtr(navData);

    result->m_navMesh.reset(dtAllocNavMesh());
    auto status = result->m_navMesh->init(navData, navDataSize, DT_TILE_FREE_DATA);
    if (dtStatusFailed(status)) {
        qCWarning(Log) << "Fail to init dtNavMesh";
        return;
    }

    result->m_navMeshQuery.reset(dtAllocNavMeshQuery());
    status = result->m_navMeshQuery->init(result->m_navMesh.get(), 2048); // TODO what is the 2048?
    if (dtStatusFailed(status)) {
        qCWarning(Log) << "Failed to init dtNavMeshQuery";
        return;
    }
    (void)navDataPtr.release(); // managed by navMeshQuery now

    // TODO store result do pmesh, dmesh need to be preserved?
    m_navMesh = std::move(resultData);
    qCDebug(Log) << "done";
#endif
}

NavMesh NavMeshBuilder::navMesh() const
{
    return d->m_navMesh;
}
