/*
    SPDX-FileCopyrightText: 2020 Volker Krause <vkrause@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "mapitem.h"

#include <KOSMIndoorMap/HitDetector>
#include <KOSMIndoorMap/MapCSSParser>
#include <KOSMIndoorMap/OverlaySource>

#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimeZone>

using namespace KOSMIndoorMap;

MapItem::MapItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
    , m_loader(new MapLoader(this))
    , m_view(new View(this))
    , m_floorLevelModel(new FloorLevelModel(this))
{
    connect(m_loader, &MapLoader::isLoadingChanged, this, &MapItem::clear);
    connect(m_loader, &MapLoader::done, this, &MapItem::loaderDone);

    m_view->setScreenSize({100, 100}); // FIXME this breaks view when done too late!
    m_controller.setView(m_view);
    connect(m_view, &View::floorLevelChanged, this, [this]() { update(); });
    connect(m_view, &View::transformationChanged, this, [this]() { update(); });

    setStylesheetName({}); // set default stylesheet
}

MapItem::~MapItem() = default;

void MapItem::paint(QPainter *painter)
{
    m_controller.updateScene(m_sg);
    m_renderer.setPainter(painter);
    m_renderer.render(m_sg, m_view);
}

MapLoader* MapItem::loader() const
{
    return m_loader;
}

View* MapItem::view() const
{
    return m_view;
}

QString MapItem::styleSheetName() const
{
    return m_styleSheetName;
}

void MapItem::setStylesheetName(const QString &styleSheet)
{
    QString styleFile;

    if (styleSheet.isEmpty() || styleSheet == QLatin1String("default")) {
        if (QGuiApplication::palette().base().color().value() > 128) {
            setStylesheetName(QStringLiteral("breeze-light"));
        } else {
            setStylesheetName(QStringLiteral("breeze-dark"));
        }
        return;
    } else {
        styleFile = styleSheet.contains(QLatin1Char(':')) ? QUrl::fromUserInput(styleSheet).toLocalFile() : styleSheet;
        if (!QFile::exists(styleFile)) {
#ifndef Q_OS_ANDROID
            auto searchPaths = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
#else
            auto searchPaths = QStandardPaths::standardLocations(QStandardPaths::AppLocalDataLocation);
#endif
            searchPaths.push_back(QStringLiteral(":"));
            for (const auto &searchPath : std::as_const(searchPaths)) {
                const QString f = searchPath + QLatin1String("/org.kde.kosmindoormap/assets/css/") + styleSheet + QLatin1String(".mapcss");
                if (QFile::exists(f)) {
                    qDebug() << "resolved stylesheet name to" << f;
                    styleFile = f;
                    break;
                }
            }
        }
    }

    if (m_styleSheetName == styleFile) {
        return;
    }
    m_styleSheetName = styleFile;
    m_style = MapCSSStyle();

    if (!m_styleSheetName.isEmpty()) {
        MapCSSParser cssParser;
        m_style = cssParser.parse(m_styleSheetName);

        if (cssParser.hasError()) {
            m_errorMessage = cssParser.errorMessage();
            Q_EMIT errorChanged();
            return;
        }
        m_errorMessage.clear();
        Q_EMIT errorChanged();
    }

    m_style.compile(m_data.dataSet());
    m_controller.setStyleSheet(&m_style);

    Q_EMIT styleSheetChanged();
    update();
}

FloorLevelModel* MapItem::floorLevelModel() const
{
    return m_floorLevelModel;
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
void MapItem::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChanged(newGeometry, oldGeometry);
#else
void MapItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
#endif
    m_view->setScreenSize(newGeometry.size().toSize());
    // the scale factor isn't automatically applied to the paint device, only to the input coordinates
    // so we need to handle this manually here
    m_view->setDeviceTransform(QTransform::fromScale(window()->devicePixelRatio(), window()->devicePixelRatio()));
}

void MapItem::loaderDone()
{
    m_floorLevelModel->setMapData(nullptr);
    m_sg.clear();

    if (!m_loader->hasError()) {
        auto data = m_loader->takeData();
        if (data.regionCode().isEmpty()) {
            data.setRegionCode(m_data.regionCode());
        }
        data.setTimeZone(m_data.timeZone());
        m_data = std::move(data);
        m_view->setSceneBoundingBox(m_data.boundingBox());
        m_controller.setMapData(m_data);
        m_style.compile(m_data.dataSet());
        m_controller.setStyleSheet(&m_style);
        m_view->setLevel(0);
        m_floorLevelModel->setMapData(&m_data);
        m_view->floorLevelChanged();
        Q_EMIT mapDataChanged();
    }

    Q_EMIT errorChanged();
    update();
}

OSMElement MapItem::elementAt(double x, double y) const
{
    HitDetector detector;
    const auto item = detector.itemAt(QPointF(x, y), m_sg, m_view);
    if (item) {
        qDebug() << item->element.url();
        for (auto it = item->element.tagsBegin(); it != item->element.tagsEnd(); ++it) {
            qDebug() << "    " << (*it).key.name() << (*it).value;
        }
        return OSMElement(item->element);
    }
    return {};
}

void MapItem::clear()
{
    if (!m_loader->isLoading() || m_sg.items().empty()) {
        return;
    }

    m_sg.clear();
    m_data = MapData();
    m_controller.setMapData(m_data);
    Q_EMIT mapDataChanged();
    Q_EMIT errorChanged();
    update();
}

bool MapItem::hasError() const
{
    return !m_errorMessage.isEmpty() || m_loader->hasError();
}

QString MapItem::errorMessage() const
{
    return m_errorMessage.isEmpty() ? m_loader->errorMessage() : m_errorMessage;
}

MapData MapItem::mapData() const
{
    return m_data;
}

QVariant MapItem::overlaySources() const
{
    return m_overlaySources;
}

void MapItem::setOverlaySources(const QVariant &overlays)
{
    const auto oldOwnedOverlays = std::move(m_ownedOverlaySources);

    std::vector<QPointer<AbstractOverlaySource>> sources;
    if (overlays.canConvert<QVariantList>()) {
        const auto l = overlays.value<QVariantList>();
        for (const auto &v : l) {
            addOverlaySource(sources, v);
        }
    } else {
        addOverlaySource(sources, overlays);
    }

    for (const auto &overlay : sources) {
        connect(overlay.data(), &AbstractOverlaySource::update, this, &MapItem::overlayUpdate, Qt::UniqueConnection);
        connect(overlay.data(), &AbstractOverlaySource::reset, this, &MapItem::overlayReset, Qt::UniqueConnection);
    }

    m_controller.setOverlaySources(std::move(sources));
    Q_EMIT overlaySourcesChanged();
    update();
}

void MapItem::addOverlaySource(std::vector<QPointer<AbstractOverlaySource>> &overlaySources, const QVariant &source)
{
    const auto obj = source.value<QObject*>();
    if (auto model = qobject_cast<QAbstractItemModel*>(obj)) {
        auto overlay = std::make_unique<ModelOverlaySource>(model);
        overlaySources.push_back(overlay.get());
        m_ownedOverlaySources.push_back(std::move(overlay));
    } else if (auto source = qobject_cast<AbstractOverlaySource*>(obj)) {
        overlaySources.push_back(source);
    } else {
        qWarning() << "unsupported overlay source:" << source << obj;
    }
}

void MapItem::overlayUpdate()
{
    m_controller.overlaySourceUpdated();
    update();
}

void MapItem::overlayReset()
{
    m_style.compile(m_data.dataSet());
}

QString MapItem::region() const
{
    return m_data.regionCode();
}

void MapItem::setRegion(const QString &region)
{
    if (m_data.regionCode() == region) {
        return;
    }

    m_data.setRegionCode(region);
    Q_EMIT regionChanged();
}

QString MapItem::timeZoneId() const
{
    return QString::fromUtf8(m_data.timeZone().id());
}

void MapItem::setTimeZoneId(const QString &tz)
{
    const auto tzId = tz.toUtf8();
    if (m_data.timeZone().id() == tzId) {
        return;
    }

    m_data.setTimeZone(QTimeZone(tzId));
    Q_EMIT timeZoneChanged();
}
