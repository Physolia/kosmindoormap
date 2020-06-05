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

#include "element.h"

using namespace OSM;

Id Element::id() const
{
    switch (type()) {
        case Type::Null:
            return {};
        case Type::Node:
            return node()->id;
        case Type::Way:
            return way()->id;
        case Type::Relation:
            return relation()->id;
    }
    return {};
}

Coordinate Element::center() const
{
    switch (type()) {
        case Type::Null:
            return {};
        case Type::Node:
            return node()->coordinate;
        case Type::Way:
            return way()->bbox.center();
        case Type::Relation:
            return relation()->bbox.center();
    }

    return {};
}

BoundingBox Element::boundingBox() const
{
    switch (type()) {
        case Type::Null:
            return {};
        case Type::Node:
            return BoundingBox(node()->coordinate, node()->coordinate);
        case Type::Way:
            return way()->bbox;
        case Type::Relation:
            return relation()->bbox;
    }

    return {};
}

QString Element::tagValue(TagKey key) const
{
    switch (type()) {
        case Type::Null:
            return {};
        case Type::Node:
            return OSM::tagValue(*node(), key);
        case Type::Way:
            return OSM::tagValue(*way(), key);
        case Type::Relation:
            return OSM::tagValue(*relation(), key);
    }

    return {};
}

QString Element::tagValue(const char *keyName) const
{
    switch (type()) {
        case Type::Null:
            return {};
        case Type::Node:
            return OSM::tagValue(*node(), keyName);
        case Type::Way:
            return OSM::tagValue(*way(), keyName);
        case Type::Relation:
            return OSM::tagValue(*relation(), keyName);
    }

    return {};
}

QString Element::tagValue(const char *keyName, const QLocale &locale) const
{
    switch (type()) {
        case Type::Null:
            return {};
        case Type::Node:
            return OSM::tagValue(*node(), keyName, locale);
        case Type::Way:
            return OSM::tagValue(*way(), keyName, locale);
        case Type::Relation:
            return OSM::tagValue(*relation(), keyName, locale);
    }

    return {};
}

std::vector<Tag>::const_iterator OSM::Element::tagsBegin() const
{
    switch (type()) {
        case Type::Null:
            Q_UNREACHABLE();
        case Type::Node:
            return node()->tags.begin();
        case Type::Way:
            return way()->tags.begin();
        case Type::Relation:
            return relation()->tags.begin();
    }
    Q_UNREACHABLE();
}

std::vector<Tag>::const_iterator OSM::Element::tagsEnd() const
{
    switch (type()) {
        case Type::Null:
            Q_UNREACHABLE();
        case Type::Node:
            return node()->tags.end();
        case Type::Way:
            return way()->tags.end();
        case Type::Relation:
            return relation()->tags.end();
    }
    Q_UNREACHABLE();
}

QString Element::url() const
{
    switch (type()) {
        case Type::Null:
            return {};
        case Type::Node:
            return node()->url();
        case Type::Way:
            return way()->url();
        case Type::Relation:
            return relation()->url();
    }

    return {};
}

template <typename Iter>
static void appendNodesFromWay(const DataSet &dataSet, std::vector<const Node*> &nodes, const Iter& nodeBegin, const Iter &nodeEnd)
{
    nodes.reserve(nodes.size() + std::distance(nodeBegin, nodeEnd));
    for (auto it = nodeBegin; it != nodeEnd; ++it) {
        const auto nodeIt = std::lower_bound(dataSet.nodes.begin(), dataSet.nodes.end(), (*it));
        if (nodeIt == dataSet.nodes.end() || (*nodeIt).id != (*it)) {
            continue;
        }
        nodes.push_back(&(*nodeIt));
    }
}

static OSM::Id appendNextPath(const DataSet &dataSet, std::vector<const Node*> &nodes, OSM::Id startNode, std::vector<const Way*> &ways)
{
    if (ways.empty()) {
        return {};
    }

    for (auto it = std::next(ways.begin()); it != ways.end(); ++it) {
        assert(!(*it)->nodes.empty()); // ensured in the caller
        if ((*it)->nodes.front() == startNode) {
            appendNodesFromWay(dataSet, nodes, (*it)->nodes.begin(), (*it)->nodes.end());
            const auto lastNodeId = (*it)->nodes.back();
            ways.erase(it);
            return lastNodeId;
        }
        // path segments can also be backwards
        if ((*it)->nodes.back() == startNode) {
            appendNodesFromWay(dataSet, nodes, (*it)->nodes.rbegin(), (*it)->nodes.rend());
            const auto lastNodeId = (*it)->nodes.front();
            ways.erase(it);
            return lastNodeId;
        }
    }

    return {};
}

std::vector<const Node*> Element::outerPath(const DataSet &dataSet) const
{
    switch (type()) {
        case Type::Null:
            return {};
        case Type::Node:
            return {node()};
        case Type::Way:
        {
            std::vector<const Node*> nodes;
            appendNodesFromWay(dataSet, nodes, way()->nodes.begin(), way()->nodes.end());
            return nodes;
        }
        case Type::Relation:
        {
            if (tagValue("type") != QLatin1String("multipolygon")) {
                return {};
            }

            // collect the relevant ways
            std::vector<const Way*> ways;
            for (const auto &member : relation()->members) {
                if (member.role != QLatin1String("outer")) {
                    continue;
                }
                const auto it = std::lower_bound(dataSet.ways.begin(), dataSet.ways.end(), member.id);
                if (it != dataSet.ways.end() && (*it).id == member.id && !(*it).nodes.empty()) {
                    ways.push_back(&(*it));
                }
            }

            // stitch them together (there is no well-defined order)
            std::vector<const Node*> nodes;
            for (auto it = ways.begin(); it != ways.end();) {
                assert(!(*it)->nodes.empty()); // ensured above

                appendNodesFromWay(dataSet, nodes, (*it)->nodes.begin(), (*it)->nodes.end());
                const auto startNode = (*it)->nodes.front();
                auto lastNode = (*it)->nodes.back();

                do {
                    lastNode = appendNextPath(dataSet, nodes, lastNode, ways);
                } while (lastNode && lastNode != startNode);

                it = ways.erase(it);
            }

            return nodes;
        }
    }

    return {};
}

void Element::recomputeBoundingBox(const DataSet &dataSet)
{
    switch (type()) {
        case Type::Null:
        case Type::Node:
            break;
        case Type::Way:
            way()->bbox = std::accumulate(way()->nodes.begin(), way()->nodes.end(), OSM::BoundingBox(), [&dataSet](auto bbox, auto nodeId) {
                const auto nodeIt = std::lower_bound(dataSet.nodes.begin(), dataSet.nodes.end(), nodeId);
                if (nodeIt == dataSet.nodes.end() || (*nodeIt).id != nodeId) {
                    return bbox;
                }
                return OSM::unite(bbox, {(*nodeIt).coordinate, (*nodeIt).coordinate});
            });
            break;
        case Type::Relation:
            relation()->bbox = {};
            for_each_member(dataSet, *relation(), [this, &dataSet](auto mem) {
                mem.recomputeBoundingBox(dataSet);
                relation()->bbox = OSM::unite(relation()->bbox, mem.boundingBox());
            });
            break;
    }
}
