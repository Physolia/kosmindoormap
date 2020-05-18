/*
    Copyright (C) 2020 Volker Krause <vkrause@kde.org>

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU Library General Public License as published by
    the Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
    License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "mapcsscondition.h"
#include "mapcssstate.h"

#include <QDebug>
#include <QIODevice>

using namespace KOSMIndoorMap;

MapCSSCondition::MapCSSCondition() = default;
MapCSSCondition::MapCSSCondition(MapCSSCondition &&) = default;
MapCSSCondition::~MapCSSCondition() = default;

bool MapCSSCondition::matches(const MapCSSState &state) const
{
    const auto v = state.element.tagValue(m_key.constData());
    switch (op) {
        case None: return !v.isEmpty();
        case Equal: return v == m_value;
        case NotEqual: return v != m_value;
    }
    return false;
}

void MapCSSCondition::setKey(const char *key, int len)
{
    m_key = QByteArray(key, len);
    qDebug() << m_key;
}

void MapCSSCondition::setValue(const char *value, int len)
{
    m_value = QString::fromUtf8(value, len);
    qDebug() << m_value;
}

void MapCSSCondition::write(QIODevice *out) const
{
    out->write("[");
    out->write(m_key);

    switch (op) {
        case None: out->write("]"); return;
        case Equal: out->write("="); break;
        case NotEqual: out->write("!="); break;
    }
    out->write(m_value.toUtf8());

    out->write("]");
}


void MapCSSConditionHolder::addCondition(MapCSSCondition *condition)
{
    conditions.push_back(std::unique_ptr<MapCSSCondition>(condition));
}