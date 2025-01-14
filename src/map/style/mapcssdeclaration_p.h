/*
    SPDX-FileCopyrightText: 2020 Volker Krause <vkrause@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#ifndef KOSMINDOORMAP_MAPCSSDECLARATION_P_H
#define KOSMINDOORMAP_MAPCSSDECLARATION_P_H

#include "kosmindoormap_export.h"
#include "mapcsstypes.h"

#include <osm/datatypes.h>

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QPen>

#include <cmath>

namespace OSM {
class DataSet;
}

class QIODevice;
class PenWidthUtilTest;

namespace KOSMIndoorMap { class MapCSSParser; }
int yyparse(KOSMIndoorMap::MapCSSParser*, void*);

namespace KOSMIndoorMap {

/** Property/value declaration of a MapCSS rule.
 *  @see https://wiki.openstreetmap.org/wiki/MapCSS/0.2#Vocabulary
 *  @internal only exported for unit tests
 */
class KOSMINDOORMAP_EXPORT MapCSSDeclaration
{
public:
    /** Type of declaration. */
    enum Type {
        PropertyDeclaration, ///< sets a style propery
        TagDeclaration, ///< sets a tag value
        ClassDeclaration, ///< sets a class type
    };

    explicit MapCSSDeclaration(Type type);
    ~MapCSSDeclaration();

    /** The property set by this declaration. */
    enum Property {
        Unknown,

        // general properties
        ZIndex, /// z-order

        // line properties
        Width, /// line width
        Color, /// line color
        Opacity, /// line opacity
        Dashes, /// line dash pattern
        Image, /// fill image for the line
        LineCap, /// line end cap style: none (default), round, square
        LineJoin, /// line join style: round (default), miter, bevel

        // line casing properties
        CasingWidth, /// line casing width
        CasingColor, /// line casing color
        CasingOpacity, /// line casing opacity
        CasingDashes, /// line casing dash pattern
        CasingLineCap, /// line casing end cap
        CasingLineJoin, /// line casing join style

        // missing here: extrude properties

        // polygon (and canvas) properties
        FillColor, /// area fill color
        FillOpacity, /// area fill opacity
        FillImage, /// image to fill the area with

        // icon properties
        IconImage, /// URL to the icon image
        IconWidth, /// icon width
        IconHeight, /// icon height
        IconOpacity, /// icon opacity
        IconColor, /// for colorized SVGs, non-standard extension
        IconAllowTextOverlap, /// the equivalent to CartoCSS's allow-overlap, non-standard extension
        IconAllowIconOverlap, /// the equivalent to CartoCSS's ignore-placement, non-standard extension

        // label properties
        FontFamily, /// font name
        FontSize, /// font size
        FontWeight, /// font weight: bold or normal (default)
        FontStyle, /// italic or normal (default)
        FontVariant, /// small-caps or normal (default)
        TextDecoration, /// none (default) or underline
        TextTransform, /// none (default), uppercase, lowercase or capitalize
        TextColor, /// text color used for the label
        TextOpacity, /// text opacity
        TextPosition, /// @p line or @p center
        TextOffset, /// vertical offset from the center of the way or point
        MaxWidth, /// maximum width before wrapping
        Text, /// label content
        TextHaloColor, /// text halo color
        TextHaloRadius, /// text halo radius

        // shield properties (casing > frame > shield > text)
        ShieldColor, /// shield color
        ShieldOpacity, /// shield opacity
        ShieldFrameColor, /// shield frame color
        ShieldFrameWidth, /// shield frame width (0 to disable)
        ShieldCasingColor, /// shield casing color
        ShieldCasingWidth, /// shield  casing width
        ShieldText, /// text to render on the shield
        ShieldImage, /// background image of the shield
        ShieldShape, /// @p rounded or @p rectangular
    };

    /** Checks if this is a meaningful declaration. */
    bool isValid() const;

    Type type() const;
    Property property() const;

    /** The type of property. Helps to determine which kind of geometry we need to emit for a rule. */
    enum PropertyFlag {
        NoFlag = 0,
        AreaProperty = 1,
        LineProperty = 2,
        LabelProperty = 4,
        CanvasProperty = 8,
    };
    int propertyFlags() const;

    /** Numeric value for this property. */
    int intValue() const;
    double doubleValue() const;
    bool boolValue() const;
    /** Quoted string value. */
    QString stringValue() const;
    /** Color value for this property. */
    QColor colorValue() const;
    /** Tag key name value. */
    QByteArray keyValue() const;
    /** Line dashes. */
    QVector<double> dashesValue() const;

    /** Tag key of the tag to change in a tag setting declaration. */
    OSM::TagKey tagKey() const;

    Qt::PenCapStyle capStyle() const;
    Qt::PenJoinStyle joinStyle() const;
    QFont::Capitalization capitalizationStyle() const;
    bool isBoldStyle() const;
    bool isItalicStyle() const;
    bool isUnderlineStyle() const;

    /** Text position property. */
    enum class Position {
        NoPostion,
        Line,
        Center,
    };
    Position textPosition() const;

    /** Unit type for numeric value. */
    enum Unit {
        NoUnit,
        Pixels,
        Point,
        Meters,
    };
    Unit unit() const;

    ClassSelectorKey classSelectorKey() const;

    void compile(const OSM::DataSet &dataSet);
    void write(QIODevice *out) const;

private:
    friend int ::yyparse(KOSMIndoorMap::MapCSSParser*, void*);
    friend class ::PenWidthUtilTest;

    /** @internal, for use by the parser. */
    void setPropertyName(const char *name, std::size_t len);
    void setIdentifierValue(const char *val, int len);
    void setDoubleValue(double val);
    void setBoolValue(bool val);
    void setStringValue(char *str);
    void setColorRgba(uint32_t argb);
    void setDashesValue(const QVector<double> &dashes);
    void setUnit(const char *val, int len);
    void setClassSelectorKey(ClassSelectorKey key);

    Property m_property = Unknown;
    int m_flags = NoFlag;
    // ### merge all of this into a QVariant?
    QByteArray m_identValue;
    QColor m_colorValue;
    double m_doubleValue = NAN;
    QVector<double> m_dashValue;
    QString m_stringValue;
    OSM::TagKey m_tagKey;
    ClassSelectorKey m_class;
    Unit m_unit = NoUnit;
    Type m_type;
    bool m_boolValue = false;
};

}

#endif // KOSMINDOORMAP_MAPCSSDECLARATION_P_H
