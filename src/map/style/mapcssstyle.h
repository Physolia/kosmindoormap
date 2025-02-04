/*
    SPDX-FileCopyrightText: 2020 Volker Krause <vkrause@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#ifndef KOSMINDOORMAP_MAPCSSSTYLE_H
#define KOSMINDOORMAP_MAPCSSSTYLE_H

#include "kosmindoormap_export.h"

#include <memory>

class QIODevice;

namespace OSM {
class DataSet;
}

namespace KOSMIndoorMap {

class MapCSSResult;
class MapCSSState;
class MapCSSStylePrivate;

/** A parsed MapCSS style sheet.
 *  @see MapCSSParser::parse for how to obtain a valid instance
 */
class KOSMINDOORMAP_EXPORT MapCSSStyle
{
public:
    /** Creates an invalid/empty style. */
    explicit MapCSSStyle();
    MapCSSStyle(const MapCSSStyle&) = delete;
    MapCSSStyle(MapCSSStyle&&) noexcept;
    ~MapCSSStyle();

    MapCSSStyle& operator=(const MapCSSStyle&) = delete;
    MapCSSStyle& operator=(MapCSSStyle&&) noexcept;

    /** Returns @c true if this is a default-constructed or otherwise empty/invalud style. */
    [[nodiscard]] bool isEmpty() const;

    /** Optimizes style sheet rules for application against @p dataSet.
     *  This does resolve tag keys and is therefore mandatory when changing the data set.
     */
    void compile(const OSM::DataSet &dataSet);

    /** Evaluates the style sheet for a given state @p state (OSM element, view state, element state, etc).
     *  The result is not returned but added to @p result for reusing allocated memory
     *  between evaluations.
     */
    void evaluate(MapCSSState &&state, MapCSSResult &result) const;

    /** Evaluate canvas style rules. */
    void evaluateCanvas(const MapCSSState &state, MapCSSResult &result) const;

    /** Write this style as MapCSS to @p out.
     *  Mainly used for testing.
     */
    void write(QIODevice *out) const;

private:
    friend class MapCSSStylePrivate;
    std::unique_ptr<MapCSSStylePrivate> d;
};

}

#endif // KOSMINDOORMAP_MAPCSSSTYLE_H
