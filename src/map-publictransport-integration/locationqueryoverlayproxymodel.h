/*
    SPDX-FileCopyrightText: 2020 Volker Krause <vkrause@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#ifndef KOSMINDOORMAP_LOCATIONQUERYOVERLAYPROXYMODEL_H
#define KOSMINDOORMAP_LOCATIONQUERYOVERLAYPROXYMODEL_H

#include <QAbstractItemModel>

#include <KOSMIndoorMap/MapData>

namespace OSM {
class Element;
class Node;
}

namespace KOSMIndoorMap {

/** Adapts a KPublicTransport::LocationQueryModel to be compatible with a KOSMIndoorMap::OverlaySource.
 */
class LocationQueryOverlayProxyModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(KOSMIndoorMap::MapData mapData READ mapData WRITE setMapData NOTIFY mapDataChanged)
    Q_PROPERTY(QObject* sourceModel READ sourceModel WRITE setSourceModel NOTIFY sourceModelChanged)

public:
    explicit LocationQueryOverlayProxyModel(QObject *parent = nullptr);
    ~LocationQueryOverlayProxyModel();

    MapData mapData() const;
    void setMapData(const MapData &data);

    QObject *sourceModel() const;
    void setSourceModel(QObject *sourceModel);

    enum Role {
        ElementRole = Qt::UserRole,
        LevelRole,
        HiddenElementRole,
    };
    Q_ENUM(Role)

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex & index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void mapDataChanged();
    void sourceModelChanged();

private:
    struct Info {
        OSM::Node overlayNode;
        OSM::Element sourceElement;
    };

    void initialize();
    Info nodeForRow(int row) const;

    struct {
        OSM::TagKey name;
        OSM::TagKey amenity;
        OSM::TagKey capacity;
        OSM::TagKey realtimeAvailable;
        OSM::TagKey network;
        OSM::TagKey mxoid;
        OSM::TagKey remainingRange;
        OSM::TagKey vehicle;
        OSM::TagKey addr_street;
        OSM::TagKey addr_city;
        OSM::TagKey addr_postcode;
    } m_tagKeys;
    OSM::TagKey m_realtimeAvailableTagKeys[5];

    std::vector<Info> m_nodes;
    MapData m_data;
    QAbstractItemModel *m_sourceModel = nullptr;
};

}

#endif // KOSMINDOORMAP_LOCATIONQUERYOVERLAYPROXYMODEL_H
