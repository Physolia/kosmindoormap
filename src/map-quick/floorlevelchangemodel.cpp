/*
    SPDX-FileCopyrightText: 2020 Volker Krause <vkrause@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "floorlevelchangemodel.h"

#include "loader/levelparser_p.h"
#include <KOSMIndoorMap/MapData>

#include <KLocalizedString>

#include <QDebug>

using namespace KOSMIndoorMap;

FloorLevelChangeModel::FloorLevelChangeModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

FloorLevelChangeModel::~FloorLevelChangeModel() = default;

int FloorLevelChangeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_levels.size();
}

QVariant FloorLevelChangeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    switch (role) {
        case Qt::DisplayRole:
            return m_levels[index.row()].name();
        case FloorLevelRole:
            return m_levels[index.row()].numericLevel();
        case CurrentFloorRole:
            return m_levels[index.row()].numericLevel() == m_currentFloorLevel;
    }
    return {};
}

QHash<int, QByteArray> FloorLevelChangeModel::roleNames() const
{
    auto n = QAbstractListModel::roleNames();
    n.insert(NameRole, "name");
    n.insert(FloorLevelRole, "floorLevel");
    n.insert(CurrentFloorRole, "isCurrentFloor");
    return n;
}

int FloorLevelChangeModel::currentFloorLevel() const
{
    return m_currentFloorLevel;
}

int FloorLevelChangeModel::currentFloorLevelRow() const
{
    const auto it = std::find_if(m_levels.begin(), m_levels.end(), [this](const auto &level) { return level.numericLevel() == m_currentFloorLevel; });
    return it != m_levels.end() ? (int)std::distance(m_levels.begin(), it) : -1;
}

void FloorLevelChangeModel::setCurrentFloorLevel(int level)
{
    if (m_currentFloorLevel == level) {
        return;
    }
    m_currentFloorLevel = level;
    if (!m_levels.empty()) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, 0));
    }
    Q_EMIT contentChanged();
}

FloorLevelModel* FloorLevelChangeModel::floorLevelModel() const
{
    return m_floorLevelModel;
}

void FloorLevelChangeModel::setFloorLevelModel(FloorLevelModel *floorLevelModel)
{
    if (m_floorLevelModel == floorLevelModel) {
        return;
    }

    if (m_floorLevelModel) {
        disconnect(m_floorLevelModel, &FloorLevelModel::modelAboutToBeReset, this, nullptr);
    }

    m_floorLevelModel = floorLevelModel;
    connect(m_floorLevelModel, &FloorLevelModel::modelAboutToBeReset, this, [this]() {
        beginResetModel();
        m_element = {};
        m_levels.clear();
        endResetModel();
    });
    Q_EMIT contentChanged();
}

OSMElement FloorLevelChangeModel::element() const
{
    return OSMElement(m_element);
}

void FloorLevelChangeModel::setElement(const OSMElement &element)
{
    if (m_element == element.element()) {
        return;
    }

    beginResetModel();
    m_element = element.element();
    m_levels.clear();

    if (isLevelChangeElement(m_element)) {

        // elevators are sometimes also tagged with building:level tags instead of level/repeat_on, so handle that as well
        const auto buildingLevels = m_element.tagValue("building:levels").toUInt();
        if (buildingLevels > 0) {
            const auto buildingMinLevel = m_element.tagValue("building:min_level", "level").toUInt();
            for (auto i = buildingMinLevel; i < buildingLevels; ++i) {
                appendFullFloorLevel(i * 10);
            }
        }
        const auto buildingUndergroundLevel = m_element.tagValue("building:levels:underground").toUInt();
        for (auto i = buildingUndergroundLevel; i > 0; --i) {
            appendFullFloorLevel(-i * 10);
        }

        LevelParser::parse(m_element.tagValue("level", "repeat_on"), m_element, [this](int level, OSM::Element e) {
            Q_UNUSED(e);
            appendFloorLevel(level);

        });
        std::sort(m_levels.begin(), m_levels.end());
        m_levels.erase(std::unique(m_levels.begin(), m_levels.end()), m_levels.end());
    }

    endResetModel();
    Q_EMIT contentChanged();
}

bool FloorLevelChangeModel::isLevelChangeElement(OSM::Element element) const
{
    return !element.tagValue("highway").isEmpty()
        || !element.tagValue("elevator").isEmpty()
        || !element.tagValue("stairwell").isEmpty()
        || element.tagValue("building:part") == "elevator"
        || element.tagValue("building") == "elevator"
        || element.tagValue("room") == "elevator"
        || element.tagValue("levelpart") == "elevator_platform"
        || (!element.tagValue("indoor").isEmpty() && element.tagValue("stairs") == "yes")
        || element.tagValue("room") == "stairs";
}

void FloorLevelChangeModel::appendFloorLevel(int level)
{
    MapLevel ml(level);
    if (ml.isFullLevel()) {
        appendFullFloorLevel(level);
    } else {
        appendFullFloorLevel(ml.fullLevelBelow());
        appendFullFloorLevel(ml.fullLevelAbove());
    }
}

void FloorLevelChangeModel::appendFullFloorLevel(int level)
{
    if (!m_floorLevelModel) {
        m_levels.push_back(MapLevel(level));
    } else {
        const auto row = m_floorLevelModel->rowForLevel(level);
        if (row >= 0) {
            const auto idx = m_floorLevelModel->index(row, 0);
            m_levels.push_back(m_floorLevelModel->data(idx, FloorLevelModel::MapLevelRole).value<MapLevel>());
        }
    }
}

bool FloorLevelChangeModel::hasSingleLevelChange() const
{
    if (m_levels.size() != 2) {
        return false;
    }
    return m_levels[0].numericLevel() == m_currentFloorLevel || m_levels[1].numericLevel() == m_currentFloorLevel;
}

int FloorLevelChangeModel::destinationLevel() const
{
    if (m_levels.size() != 2) {
        return 0;
    }
    return m_levels[0].numericLevel() == m_currentFloorLevel ? m_levels[1].numericLevel() : m_levels[0].numericLevel();
}

QString FloorLevelChangeModel::destinationLevelName() const
{
    if (m_levels.size() != 2) {
        return {};
    }
    return m_levels[0].numericLevel() == m_currentFloorLevel ? m_levels[1].name() : m_levels[0].name();
}

bool FloorLevelChangeModel::hasMultipleLevelChanges() const
{
    return m_levels.size() > 1;
}

QString FloorLevelChangeModel::title() const
{
    if (m_element.tagValue("highway") == "elevator"
        || !m_element.tagValue("elevator").isEmpty()
        || m_element.tagValue("building:part") == "elevator"
        || m_element.tagValue("building") == "elevator"
        || m_element.tagValue("room") == "elevator"
        || m_element.tagValue("levelpart") == "elevator_platform")
    {
        return i18n("Elevator");
    }

    if (!m_element.tagValue("stairwell").isEmpty()
     || m_element.tagValue("stairs") == "yes"
     || m_element.tagValue("room") == "stairs")
    {
        return i18n("Staircase");
    }

    if (m_levels.size() > 2) {
        qWarning() << "Unknown floor level change element type:" << m_element.url();
    }
    return {};
}

#include "moc_floorlevelchangemodel.cpp"
