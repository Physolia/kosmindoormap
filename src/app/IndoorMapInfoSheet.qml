/*
    SPDX-FileCopyrightText: 2020 Volker Krause <vkrause@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

import QtQuick 2.12
import QtQuick.Layouts 1.1
import QtQuick.Controls 2.1 as QQC2
import org.kde.kirigami 2.7 as Kirigami
import org.kde.kosmindoormap 1.0
import org.kde.osm.editorcontroller 1.0

Kirigami.OverlaySheet {
    id: elementDetailsSheet
    property var model
    property var mapData

    header: Column {
        Kirigami.Heading {
            text: elementDetailsSheet.model.name
        }
        Kirigami.Heading {
            text: elementDetailsSheet.model.category
            level: 4
            visible: text != ""
        }
    }

    ListView {
        id: contentView
        model: elementDetailsSheet.model

        Component {
            id: infoStringDelegate
            RowLayout {
                QQC2.Label {
                    visible: row && row.keyLabel != ""
                    text: row ? row.keyLabel + ":" : ""
                    color: (row && row.category == OSMElementInformationModel.DebugCategory) ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.textColor
                    Layout.alignment: Qt.AlignTop
                }
                QQC2.Label {
                    text: row ? row.value : ""
                    color: (row && row.category == OSMElementInformationModel.DebugCategory) ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.textColor
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        Component {
            id: infoLinkDelegate
            RowLayout {
                QQC2.Label {
                    visible: row && row.keyLabel != ""
                    text: row ? row.keyLabel + ":" : ""
                    color: (row && row.category == OSMElementInformationModel.DebugCategory) ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.textColor
                    Layout.alignment: Qt.AlignTop
                }
                QQC2.Label {
                    text: row ? "<a href=\"" + row.url + "\">" + row.value + "</a>" : ""
                    color: (row && row.category == OSMElementInformationModel.DebugCategory) ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.textColor
                    onLinkActivated: Qt.openUrlExternally(link)
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        Component {
            id: infoAddressDelegate
            QQC2.Label {
                text: (row.value.street + " " + row.value.houseNumber + "\n" + row.value.postalCode + " " + row.value.city + "\n" + row.value.country).trim()
            }
        }

        Component {
            id: infoOpeningHoursDelegate
            IndoorMapInfoSheetOpeningHoursDelegate {
                mapData: elementDetailsSheet.mapData
                model: row
            }
        }

        section.property: "categoryLabel"
        section.delegate: Kirigami.Heading {
            x: Kirigami.Units.largeSpacing
            level: 4
            text: section
            color: section == "Debug" ? Kirigami.Theme.disabledTextColor : Kirigami.Theme.textColor
            height: implicitHeight + Kirigami.Units.largeSpacing
            verticalAlignment: Qt.AlignBottom
        }
        section.criteria: ViewSection.FullString
        section.labelPositioning: ViewSection.InlineLabels

        delegate: Loader {
            property var row: model
            x: Kirigami.Units.largeSpacing
            width: ListView.view.width - 2 * x
            sourceComponent: {
                switch (row.type) {
                    case OSMElementInformationModel.Link:
                        return infoLinkDelegate;
                    case OSMElementInformationModel.PostalAddress:
                        return infoAddressDelegate;
                    case OSMElementInformationModel.OpeningHoursType:
                        return infoOpeningHoursDelegate;
                    case OSMElementInformationModel.String:
                    default:
                        return infoStringDelegate;
                }
            }
        }
    }

    footer: RowLayout {
        Item { Layout.fillWidth: true }
        QQC2.Button {
            icon.name: "document-edit"
            text: "Edit with iD"
            onClicked: EditorController.editElement(elementDetailsSheet.model.element.element, Editor.ID)
        }
        QQC2.Button {
            icon.name: "org.openstreetmap.josm"
            text: "Edit with JOSM"
            visible: EditorController.hasEditor(Editor.JOSM)
            onClicked: EditorController.editElement(elementDetailsSheet.model.element.element, Editor.JOSM)
        }
        QQC2.Button {
            icon.name: "document-edit"
            text: "Edit with Vespucci"
            visible: EditorController.hasEditor(Editor.Vespucci)
            onClicked: EditorController.editElement(elementDetailsSheet.model.element.element, Editor.Vespucci)
        }
    }

    onSheetOpenChanged: {
        if (sheetOpen == false) {
            elementDetailsSheet.model.clear()
        }
    }
}
