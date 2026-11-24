#include "stingray/flow/flow_graph.h"

#include "stingray/murmur_hash.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_map>

namespace dtglb::stingray::flow {
namespace {

struct TypeRow {
    std::uint32_t number;
    const char* name;
    Codec codec;
    const char *in_events, *out_events, *ports;
    std::uint32_t uses;
};
const TypeRow kTypeRows[] = {
#include "stingray/flow/flow_node_types.inc"
};

std::vector<std::string> split(const char* text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string item; in >> item;) out.push_back(item);
    return out;
}

constexpr std::size_t kHeaderWords = 23;
constexpr std::size_t kHeaderSize = kHeaderWords * 4;

std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}
void put(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}
void put_at(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) { std::memcpy(out.data() + at, &value, 4); }
void put_string(std::vector<std::uint8_t>& out, const std::string& text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}
void align(std::vector<std::uint8_t>& out, std::size_t to) { out.resize((out.size() + to - 1) / to * to, 0); }

// a node record's payload as read
class Payload {
public:
    Payload(const std::vector<std::uint8_t>& flow, std::size_t begin, std::size_t end) : flow_(flow), begin_(begin), end_(end) {}
    std::size_t size() const { return end_ - begin_; }
    std::size_t words() const { return size() / 4; }
    std::uint32_t at(std::size_t index) const { return word(flow_, begin_ + 4 * index); }
    // a flow offset as a byte position in the payload; false when outside it
    bool local(std::uint32_t offset, std::size_t& out) const {
        if (offset < begin_ || offset > end_) return false;
        out = offset - begin_;
        return true;
    }
    bool string(std::size_t& cursor, std::string& out) const {
        if (cursor >= size()) return false;
        const auto* first = flow_.data() + begin_ + cursor;
        const auto* nul = static_cast<const std::uint8_t*>(std::memchr(first, 0, size() - cursor));
        if (!nul) return false;
        out.assign(reinterpret_cast<const char*>(first), static_cast<std::size_t>(nul - first));
        cursor += out.size() + 1;
        return true;
    }
    bool links(std::size_t& cursor, std::size_t node_count, std::vector<Link>& out) const {
        if (cursor + 1 > words()) return false;
        const auto count = at(cursor++);
        if (count > words() - cursor) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto value = at(cursor++);
            Link link{static_cast<std::uint16_t>(value & 0xffffu), static_cast<std::uint16_t>(value >> 16)};
            if (link.node >= node_count) return false;
            out.push_back(link);
        }
        return true;
    }

private:
    const std::vector<std::uint8_t>& flow_;
    std::size_t begin_, end_;
};

void put_links(std::vector<std::uint8_t>& out, const std::vector<Link>& links) {
    put(out, static_cast<std::uint32_t>(links.size()));
    for (const auto& link : links) put(out, link.node | static_cast<std::uint32_t>(link.event) << 16);
}

std::string hex32(std::uint32_t value) {
    char text[12];
    std::snprintf(text, sizeof(text), "#%08x", value);
    return text;
}

// the port word holding a story's marker count
std::size_t marker_count_word(std::uint32_t type) { return type == 0x2a ? 8 : 2; }

bool read_node(const std::vector<std::uint8_t>& flow, std::size_t start, std::size_t end, std::size_t node_count,
               Node& node, std::string& error) {
    node.type = word(flow, start);
    const auto query = word(flow, start + 4);
    const Payload p(flow, start + 8, end);
    const NodeType* type = node_type(node.type);
    if (!type) { error = "unknown node type " + std::to_string(node.type); return false; }
    const std::size_t ports = type->ports.size();
    if (p.words() < ports) { error = "record shorter than its type's ports"; return false; }
    node.words.resize(ports);
    for (std::size_t i = 0; i < ports; ++i) node.words[i] = p.at(i);
    std::size_t cursor = ports;
    const auto lists = [&](const std::vector<std::string>& names, const std::vector<std::uint32_t>& ids) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            EventList list;
            list.name = names[i];
            list.id = i < ids.size() ? ids[i] : 0;
            if (!p.links(cursor, node_count, list.links)) return false;
            node.outputs.push_back(std::move(list));
        }
        return true;
    };
    switch (type->codec) {
    case Codec::kFixed:
        if (!lists(type->out_events, {})) { error = "output event lists run past the record"; return false; }
        break;
    case Codec::kStory: {
        auto names = type->out_events;
        const auto markers = node.words[marker_count_word(node.type)];
        if (markers > p.words()) { error = "story marker count past the record"; return false; }
        for (std::uint32_t i = 0; i < markers; ++i) names.push_back("marker_" + std::to_string(i));
        if (!lists(names, {})) { error = "output event lists run past the record"; return false; }
        break;
    }
    case Codec::kNested: {
        if (p.words() < cursor + 8) { error = "nested descriptor past the record"; return false; }
        std::array<std::uint32_t, 4> counts{};
        std::array<std::size_t, 4> at{};
        for (int g = 0; g < 4; ++g) {
            counts[g] = p.at(cursor + 2 * g);
            if (!counts[g]) continue;
            const std::size_t row = g < 2 ? 4 : 12;
            if (!p.local(p.at(cursor + 2 * g + 1), at[g]) || at[g] % 4 || counts[g] > (p.size() - at[g]) / row) {
                error = "nested table outside the record"; return false;
            }
        }
        cursor += 8;
        std::vector<std::uint32_t> outs;
        for (std::uint32_t i = 0; i < counts[1]; ++i) outs.push_back(p.at(at[1] / 4 + i));
        std::vector<std::string> names;
        for (const auto id : outs) names.push_back(hex32(id));
        if (!lists(names, outs)) { error = "output event lists run past the record"; return false; }
        for (std::uint32_t i = 0; i < counts[0]; ++i) node.input_events.push_back(p.at(at[0] / 4 + i));
        for (int g = 2; g < 4; ++g)
            for (std::uint32_t i = 0; i < counts[g]; ++i) {
                const auto w = at[g] / 4 + 3 * i;
                (g == 2 ? node.input_variables : node.output_variables).push_back({p.at(w), p.at(w + 1), p.at(w + 2)});
            }
        break;
    }
    case Codec::kLua: {
        const auto& w = node.words;
        std::size_t at = 0;
        if (!p.local(w[5], at)) { error = "Lua callbacks outside the record"; return false; }
        for (std::uint32_t i = 0; i < w[4]; ++i) {
            if (at >= p.size()) { error = "Lua callbacks run past the record"; return false; }
            LuaCallback callback;
            callback.mode = flow[start + 8 + at];
            ++at;
            if (callback.mode > 1 || (callback.mode == 1 && !p.string(at, callback.module)) || !p.string(at, callback.function)) {
                error = "Lua callback unreadable"; return false;
            }
            node.callbacks.push_back(std::move(callback));
        }
        for (int side = 0; side < 2; ++side) {
            const auto count = w[6 + 2 * side];
            if (count && !p.local(w[7 + 2 * side], at)) { error = "Lua ports outside the record"; return false; }
            for (std::uint32_t i = 0; i < count; ++i) {
                if (at + 8 > p.size()) { error = "Lua ports run past the record"; return false; }
                LuaPort port;
                port.type = word(flow, start + 8 + at);
                port.slot = word(flow, start + 8 + at + 4);
                at += 8;
                if (!p.string(at, port.name)) { error = "Lua port name unreadable"; return false; }
                if (!side) at = (at + 3) & ~std::size_t{3};
                (side ? node.lua_outputs : node.lua_inputs).push_back(std::move(port));
            }
        }
        std::vector<std::string> names(w[10]);
        if (w[10] && !p.local(w[11], at)) { error = "Lua output names outside the record"; return false; }
        for (auto& name : names)
            if (!p.string(at, name)) { error = "Lua output name unreadable"; return false; }
        if (!lists(names, {})) { error = "output event lists run past the record"; return false; }
        break;
    }
    }
    std::size_t q = 0;
    if (query && p.local(query, q) && q < p.size()) {
        if (q % 4) { error = "query list misaligned"; return false; }
        std::size_t qc = q / 4;
        node.has_query = true;
        if (!p.links(qc, node_count, node.query)) { error = "query list runs past the record"; return false; }
    } else {
        node.query_offset = query;
    }
    return true;
}

// lays out a node's record (from its 8-aligned start in the flow)
bool write_node(const Node& node, std::size_t start, std::vector<std::uint8_t>& record, std::string& error) {
    const NodeType* type = node_type(node.type);
    if (!type) { error = "unknown node type " + std::to_string(node.type); return false; }
    if (node.words.size() != type->ports.size()) { error = "node words do not match its type's ports"; return false; }
    record.clear();
    put(record, node.type);
    put(record, 0);
    for (const auto value : node.words) put(record, value);
    const auto offset = [&] { return static_cast<std::uint32_t>(start + record.size()); };
    const auto word_at = [](std::size_t index) { return 8 + 4 * index; };
    const auto lists = [&] { for (const auto& list : node.outputs) put_links(record, list.links); };
    switch (type->codec) {
    case Codec::kFixed:
    case Codec::kStory: {
        auto expected = type->out_events.size();
        if (type->codec == Codec::kStory) expected += node.words[marker_count_word(node.type)];
        if (node.outputs.size() != expected) { error = "node has the wrong number of output event lists"; return false; }
        lists();
        break;
    }
    case Codec::kNested: {
        const std::size_t header = record.size();
        record.resize(record.size() + 32, 0);
        lists();
        std::array<std::uint32_t, 4> counts{static_cast<std::uint32_t>(node.input_events.size()),
                                            static_cast<std::uint32_t>(node.outputs.size()),
                                            static_cast<std::uint32_t>(node.input_variables.size()),
                                            static_cast<std::uint32_t>(node.output_variables.size())};
        std::array<std::uint32_t, 4> at{};
        at[0] = offset();
        for (const auto id : node.input_events) put(record, id);
        at[1] = offset();
        for (const auto& list : node.outputs) put(record, list.id);
        for (int g = 2; g < 4; ++g) {
            at[g] = offset();
            for (const auto& v : g == 2 ? node.input_variables : node.output_variables) {
                put(record, v.id);
                put(record, v.slot);
                put(record, v.size);
            }
        }
        for (int g = 0; g < 4; ++g) {
            put_at(record, header + 8 * g, counts[g]);
            put_at(record, header + 8 * g + 4, at[g]);
        }
        break;
    }
    case Codec::kLua: {
        lists();
        put_at(record, word_at(4), static_cast<std::uint32_t>(node.callbacks.size()));
        put_at(record, word_at(5), offset());
        for (const auto& callback : node.callbacks) {
            record.push_back(callback.mode);
            if (callback.mode == 1) put_string(record, callback.module);
            put_string(record, callback.function);
        }
        align(record, 4);
        for (int side = 0; side < 2; ++side) {
            const auto& ports = side ? node.lua_outputs : node.lua_inputs;
            put_at(record, word_at(6 + 2 * side), static_cast<std::uint32_t>(ports.size()));
            put_at(record, word_at(7 + 2 * side), offset());
            for (const auto& port : ports) {
                put(record, port.type);
                put(record, port.slot);
                put_string(record, port.name);
                if (!side) align(record, 4);
            }
        }
        align(record, 4);
        put_at(record, word_at(10), static_cast<std::uint32_t>(node.outputs.size()));
        put_at(record, word_at(11), offset());
        for (const auto& list : node.outputs) put_string(record, list.name);
        align(record, 4);
        break;
    }
    }
    if (node.has_query) {
        put_at(record, 4, offset());
        put_links(record, node.query);
    } else {
        put_at(record, 4, node.query_offset);
    }
    align(record, 8);
    return true;
}

}  // namespace

const std::vector<NodeType>& node_types() {
    static const std::vector<NodeType> types = [] {
        std::vector<NodeType> out;
        for (const auto& row : kTypeRows) {
            NodeType type;
            type.number = row.number;
            type.name = row.name;
            type.codec = row.codec;
            type.game_uses = row.uses;
            type.in_events = split(row.in_events);
            type.out_events = split(row.out_events);
            for (const auto& port : split(row.ports)) {
                const auto a = port.find(':'), b = port.find(':', a + 1);
                type.ports.push_back({port.substr(0, a), port.substr(a + 1, b - a - 1), port.substr(b + 1)});
            }
            out.push_back(std::move(type));
        }
        return out;
    }();
    return types;
}

const NodeType* node_type(std::uint32_t number) {
    static const auto index = [] {
        std::unordered_map<std::uint32_t, const NodeType*> out;
        for (const auto& type : node_types()) out.emplace(type.number, &type);
        return out;
    }();
    const auto found = index.find(number);
    return found == index.end() ? nullptr : found->second;
}

const NodeType* node_type(const std::string& name) {
    static const auto index = [] {
        std::unordered_map<std::string, const NodeType*> out;
        for (const auto& type : node_types())
            if (!type.name.empty()) out.emplace(type.name, &type);
        return out;
    }();
    const auto found = index.find(name);
    return found == index.end() ? nullptr : found->second;
}

std::string tag_kind(std::uint32_t tag) {
    static const std::map<std::uint32_t, std::string> kinds{
        {0, "nil"}, {1, "unit"}, {2, "actor"}, {3, "mover"}, {4, "vector3"}, {5, "float"}, {6, "bool"}, {7, "string"},
        {8, "id64"}, {9, "quaternion"}, {10, "integer"}, {11, "camera"}, {12, "light"}, {13, "mesh"}, {14, "material"},
        {16, "level"}};
    const auto found = kinds.find(tag);
    return found == kinds.end() ? std::string() : found->second;
}

std::uint32_t kind_size(const std::string& kind) {
    static const std::map<std::string, std::uint32_t> sizes{
        {"unit", 4}, {"actor", 4}, {"mover", 8}, {"level", 8}, {"float", 4}, {"uint", 4}, {"integer", 4}, {"bool", 4},
        {"id32", 4}, {"id64", 8}, {"camera", 8}, {"light", 8}, {"material", 8}, {"mesh", 8}, {"vector3", 12},
        {"quaternion", 16}, {"effect_id", 4}, {"story_id", 4}, {"trigger_block", 12}, {"contact_block", 52},
        {"mover_actor_collision_block", 56}, {"mover_mover_collision_block", 24}, {"raycast_hit_block", 40}};
    const auto found = sizes.find(kind);
    return found == sizes.end() ? 0 : found->second;
}

bool read_graph(const std::vector<std::uint8_t>& flow, const std::vector<std::uint8_t>& dynamic, Graph& graph,
                std::string& error) {
    graph = Graph{};
    if (flow.size() < kHeaderSize) { error = "flow is shorter than its header"; return false; }
    std::array<std::uint32_t, kHeaderWords> h{};
    for (std::size_t i = 0; i < kHeaderWords; ++i) h[i] = word(flow, 4 * i);
    const auto inside = [&](std::size_t offset, std::size_t size) { return offset <= flow.size() && size <= flow.size() - offset; };
    if (h[0] != 0 || h[21] != flow.size()) { error = "flow header does not describe this flow"; return false; }
    const std::uint32_t nodes = h[1], events = h[3], extra = h[5], variables = h[6], preload = h[9], subroutines = h[11];
    if (nodes > 0x10000 || !inside(h[2], 4ull * nodes) || !inside(h[4], 8ull * (events + extra)) ||
        !inside(h[7], 12ull * variables) || !inside(h[8], 4) || !inside(h[10], 8ull * preload) ||
        !inside(h[12], 4ull * subroutines)) {
        error = "flow table lies outside the flow"; return false;
    }
    for (std::uint32_t i = 0; i < events; ++i) {
        const auto target = word(flow, h[4] + 8 * i + 4);
        graph.external_events.push_back({word(flow, h[4] + 8 * i), static_cast<std::uint16_t>(target & 0xffffu),
                                         static_cast<std::uint16_t>(target >> 16)});
    }
    for (std::uint32_t i = 0; i < extra; ++i)
        graph.extra.push_back({word(flow, h[4] + 8 * (events + i)), word(flow, h[4] + 8 * (events + i) + 4)});
    for (std::uint32_t i = 0; i < variables; ++i)
        graph.variables.push_back({word(flow, h[7] + 12 * i), word(flow, h[7] + 12 * i + 4), word(flow, h[7] + 12 * i + 8)});
    const auto query = word(flow, h[8]);
    if (!inside(h[8] + 4, 4ull * query)) { error = "flow query table lies outside the flow"; return false; }
    for (std::uint32_t i = 0; i < query; ++i) graph.query.push_back(word(flow, h[8] + 4 + 4 * i));
    for (std::uint32_t i = 0; i < preload; ++i) graph.preload.push_back({word(flow, h[10] + 8 * i), word(flow, h[10] + 8 * i + 4)});
    for (std::uint32_t i = 0; i < subroutines; ++i) graph.subroutines.push_back(word(flow, h[12] + 4 * i));
    for (int k = 0; k < 4; ++k) {
        if (!inside(h[17 + k], 4ull * h[13 + k])) { error = "flow list lies outside the flow"; return false; }
        for (std::uint32_t i = 0; i < h[13 + k]; ++i) graph.lists[k].push_back(word(flow, h[17 + k] + 4 * i));
    }
    graph.nodes.resize(nodes);
    for (std::uint32_t i = 0; i < nodes; ++i) {
        const std::size_t start = word(flow, h[2] + 4 * i);
        const std::size_t end = i + 1 < nodes ? word(flow, h[2] + 4 * (i + 1)) : flow.size();
        if (end == start) { graph.nodes[i].empty = true; continue; }
        if (end < start + 8 || end > flow.size() || (end - start) % 4 || start % 8) {
            error = "flow node " + std::to_string(i) + " has no valid extent"; return false;
        }
        if (!read_node(flow, start, end, nodes, graph.nodes[i], error)) {
            error = "flow node " + std::to_string(i) + ": " + error; return false;
        }
    }
    graph.dynamic_size = h[22];
    graph.dynamic = dynamic;
    return true;
}

bool write_graph(const Graph& graph, std::vector<std::uint8_t>& flow, std::string& error) {
    if (graph.nodes.size() > 0x10000) { error = "a flow holds at most 65536 nodes"; return false; }
    flow.assign(kHeaderSize, 0);
    const auto node_index = flow.size();
    flow.resize(flow.size() + 4 * graph.nodes.size(), 0);
    const auto events_offset = flow.size();
    // the tables the game searches by name are sorted by it (equal names keep their order)
    auto external_events = graph.external_events;
    std::stable_sort(external_events.begin(), external_events.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    auto extra = graph.extra;
    std::stable_sort(extra.begin(), extra.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    auto variables = graph.variables;
    std::stable_sort(variables.begin(), variables.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    for (const auto& event : external_events) {
        if (event.node >= graph.nodes.size()) { error = "external event names a missing node"; return false; }
        put(flow, event.id);
        put(flow, event.node | static_cast<std::uint32_t>(event.in) << 16);
    }
    for (const auto& pair : extra) { put(flow, pair[0]); put(flow, pair[1]); }
    const auto variables_offset = flow.size();
    for (const auto& v : variables) for (const auto x : v) put(flow, x);
    const auto query_offset = flow.size();
    put(flow, static_cast<std::uint32_t>(graph.query.size()));
    for (const auto v : graph.query) put(flow, v);
    align(flow, 8);
    const auto preload_offset = flow.size();
    for (const auto& pair : graph.preload) { put(flow, pair[0]); put(flow, pair[1]); }
    const auto subroutines_offset = flow.size();
    for (const auto v : graph.subroutines) put(flow, v);
    std::array<std::size_t, 4> list_offsets{};
    for (int k = 0; k < 4; ++k) {
        list_offsets[k] = flow.size();
        for (const auto v : graph.lists[k]) put(flow, v);
    }
    align(flow, 8);
    std::vector<std::uint8_t> record;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        put_at(flow, node_index + 4 * i, static_cast<std::uint32_t>(flow.size()));
        const auto& node = graph.nodes[i];
        if (node.empty) continue;
        for (const auto& list : node.outputs)
            for (const auto& link : list.links)
                if (link.node >= graph.nodes.size()) { error = "flow node " + std::to_string(i) + " links to a missing node"; return false; }
        if (!write_node(node, flow.size(), record, error)) { error = "flow node " + std::to_string(i) + ": " + error; return false; }
        flow.insert(flow.end(), record.begin(), record.end());
    }
    const std::array<std::uint32_t, kHeaderWords> header{
        0, static_cast<std::uint32_t>(graph.nodes.size()), static_cast<std::uint32_t>(node_index),
        static_cast<std::uint32_t>(external_events.size()), static_cast<std::uint32_t>(events_offset),
        static_cast<std::uint32_t>(extra.size()), static_cast<std::uint32_t>(variables.size()),
        static_cast<std::uint32_t>(variables_offset), static_cast<std::uint32_t>(query_offset),
        static_cast<std::uint32_t>(graph.preload.size()), static_cast<std::uint32_t>(preload_offset),
        static_cast<std::uint32_t>(graph.subroutines.size()), static_cast<std::uint32_t>(subroutines_offset),
        static_cast<std::uint32_t>(graph.lists[0].size()), static_cast<std::uint32_t>(graph.lists[1].size()),
        static_cast<std::uint32_t>(graph.lists[2].size()), static_cast<std::uint32_t>(graph.lists[3].size()),
        static_cast<std::uint32_t>(list_offsets[0]), static_cast<std::uint32_t>(list_offsets[1]),
        static_cast<std::uint32_t>(list_offsets[2]), static_cast<std::uint32_t>(list_offsets[3]),
        static_cast<std::uint32_t>(flow.size()), graph.dynamic_size};
    std::memcpy(flow.data(), header.data(), kHeaderSize);
    return true;
}

std::vector<PortRef> node_ports(const Graph& graph, const Node& node) {
    std::vector<PortRef> out;
    const NodeType* type = node_type(node.type);
    if (!type || node.empty) return out;
    const auto string_size = [&](std::uint32_t slot) -> std::uint32_t {
        if (slot >= graph.dynamic.size()) return 0;
        const auto* first = graph.dynamic.data() + slot;
        const auto* nul = std::memchr(first, 0, graph.dynamic.size() - slot);
        return nul ? static_cast<std::uint32_t>(static_cast<const std::uint8_t*>(nul) - first + 1) : 0;
    };
    const auto add = [&](const std::string& name, const std::string& direction, const std::string& kind, std::uint32_t slot,
                         std::uint32_t size) {
        PortRef ref{name, direction, kind, slot, size};
        if (slot != kNoSlot && kind == "string") ref.size = string_size(slot);
        out.push_back(std::move(ref));
    };
    for (std::size_t i = 0; i < type->ports.size() && i < node.words.size(); ++i) {
        const auto& port = type->ports[i];
        if (port.direction == "static" || port.kind == "ptr_lo" || port.kind == "ptr_hi") {
            add(port.name, port.direction, port.kind, kNoSlot, 0);
            continue;
        }
        std::string kind = port.kind;
        if ((kind == "typed" || kind == "lua_typed") && i) kind = tag_kind(node.words[i - 1]);
        std::uint32_t size = kind_size(kind);
        if (kind == "float_array") size = 4 * node.words[marker_count_word(node.type)];
        if (kind == "lua_reference_array") size = 8 * static_cast<std::uint32_t>(node.callbacks.size());
        add(port.name, port.direction, kind, node.words[i], size);
    }
    for (int side = 0; side < 2; ++side)
        for (const auto& v : side ? node.output_variables : node.input_variables)
            add(hex32(v.id), side ? "out" : "in", v.size == 128 ? "string" : "", v.slot, v.size == 128 ? 0 : v.size);
    for (int side = 0; side < 2; ++side)
        for (const auto& port : side ? node.lua_outputs : node.lua_inputs) {
            const auto kind = tag_kind(port.type);
            add(port.name, side ? "out" : "in", kind, port.slot, kind_size(kind));
        }
    return out;
}

namespace {
bool parse_id32_text(const std::string& text, std::uint32_t& out) {
    if (text.size() == 9 && text[0] == '#') return std::sscanf(text.c_str() + 1, "%8x", &out) == 1;
    if (text.empty()) return false;
    out = id32_from_id64(text);
    return true;
}
std::vector<float> floats(const std::string& text) {
    std::vector<float> out;
    std::string item;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',' || text[i] == ' ') {
            if (!item.empty()) out.push_back(std::strtof(item.c_str(), nullptr));
            item.clear();
        } else {
            item += text[i];
        }
    }
    return out;
}

std::string number(float v) {
    char t[32];
    std::snprintf(t, sizeof(t), "%.9g", v);
    return t;
}
}  // namespace

std::string value_text(const std::string& kind, const std::vector<std::uint8_t>& data, std::uint32_t slot) {
    if (slot == kNoSlot || slot >= data.size()) return {};
    const auto* p = data.data() + slot;
    const std::size_t left = data.size() - slot;
    const auto f = [&](int i) { float v; std::memcpy(&v, p + 4 * i, 4); return v; };
    const auto u = [&] { std::uint32_t v; std::memcpy(&v, p, 4); return v; };
    if (kind == "string") {
        const auto* nul = static_cast<const std::uint8_t*>(std::memchr(p, 0, left));
        return std::string(reinterpret_cast<const char*>(p), nul ? static_cast<std::size_t>(nul - p) : left);
    }
    if (left < kind_size(kind)) return {};
    if (kind == "float") return number(f(0));
    if (kind == "vector3") return number(f(0)) + ", " + number(f(1)) + ", " + number(f(2));
    if (kind == "quaternion") return number(f(0)) + ", " + number(f(1)) + ", " + number(f(2)) + ", " + number(f(3));
    if (kind == "integer") return std::to_string(static_cast<std::int32_t>(u()));
    if (kind == "uint" || kind == "index") return std::to_string(u());
    if (kind == "id32") return hex32(u());
    if (kind == "bool") return u() ? "true" : "false";
    if (kind == "id64") {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        char t[20];
        std::snprintf(t, sizeof(t), "%016llx", static_cast<unsigned long long>(v));
        return t;
    }
    return {};
}

bool value_bytes(const std::string& kind, const std::string& text, std::vector<std::uint8_t>& out) {
    out.clear();
    const auto put = [&](const void* v, std::size_t n) { out.insert(out.end(), static_cast<const std::uint8_t*>(v), static_cast<const std::uint8_t*>(v) + n); };
    if (kind == "string") {
        out.assign(text.begin(), text.end());
        out.push_back(0);
        return true;
    }
    if (kind == "float" || kind == "vector3" || kind == "quaternion") {
        const auto values = floats(text);
        const std::size_t n = kind == "float" ? 1 : kind == "vector3" ? 3 : 4;
        if (values.size() != n) return false;
        put(values.data(), 4 * n);
        return true;
    }
    if (kind == "bool") {
        const std::uint32_t v = text == "true" || text == "1";
        put(&v, 4);
        return true;
    }
    if (kind == "id32") {
        std::uint32_t v;
        if (!parse_id32_text(text, v)) return false;
        put(&v, 4);
        return true;
    }
    if (kind == "id64") {
        std::uint64_t v = 0;
        if (text.size() == 16 && std::sscanf(text.c_str(), "%16llx", reinterpret_cast<unsigned long long*>(&v)) == 1) {
        } else {
            v = id64(text);
        }
        put(&v, 8);
        return true;
    }
    if (kind == "integer" || kind == "uint" || kind == "index") {
        char* end = nullptr;
        const long long v = std::strtoll(text.c_str(), &end, 10);
        if (end == text.c_str()) return false;
        const auto w = static_cast<std::uint32_t>(v);
        put(&w, 4);
        return true;
    }
    return false;
}

std::uint32_t* port_word(Node& node, std::size_t port) {
    const NodeType* type = node_type(node.type);
    if (!type || node.empty) return nullptr;
    if (port < type->ports.size()) return port < node.words.size() ? &node.words[port] : nullptr;
    port -= type->ports.size();
    for (auto* list : {&node.input_variables, &node.output_variables}) {
        if (port < list->size()) return &(*list)[port].slot;
        port -= list->size();
    }
    for (auto* list : {&node.lua_inputs, &node.lua_outputs}) {
        if (port < list->size()) return &(*list)[port].slot;
        port -= list->size();
    }
    return nullptr;
}

std::uint32_t kind_tag(const std::string& kind) {
    for (std::uint32_t tag = 0; tag <= 16; ++tag)
        if (tag_kind(tag) == kind) return tag;
    return ~0u;
}

std::uint32_t add_slot(Graph& graph, const std::vector<std::uint8_t>& value, std::uint32_t alignment) {
    // the first 32 bytes are the runtime's (graph pointers); the size stays a multiple of 8
    if (graph.dynamic.size() < 32) graph.dynamic.resize(32, 0);
    align(graph.dynamic, alignment);
    const auto offset = static_cast<std::uint32_t>(graph.dynamic.size());
    graph.dynamic.insert(graph.dynamic.end(), value.begin(), value.end());
    align(graph.dynamic, 8);
    graph.dynamic_size = static_cast<std::uint32_t>(graph.dynamic.size());
    return offset;
}

namespace {
bool slot_port(const PortType& port) {
    return port.direction != "static" && port.kind != "ptr_lo" && port.kind != "ptr_hi";
}
}  // namespace

void for_each_slot(Node& node, const std::function<void(std::uint32_t& slot, const std::string& direction)>& visit) {
    if (node.empty) return;
    if (const NodeType* type = node_type(node.type))
        for (std::size_t i = 0; i < type->ports.size() && i < node.words.size(); ++i)
            if (slot_port(type->ports[i])) visit(node.words[i], type->ports[i].direction);
    for (auto& v : node.input_variables) visit(v.slot, "in");
    for (auto& v : node.output_variables) visit(v.slot, "out");
    for (auto& p : node.lua_inputs) visit(p.slot, "in");
    for (auto& p : node.lua_outputs) visit(p.slot, "out");
}

bool add_node(Graph& graph, std::uint32_t type_number, std::size_t& index, std::string& error) {
    const NodeType* type = node_type(type_number);
    if (!type || type->name.empty()) { error = "unknown node type"; return false; }
    if (type->codec != Codec::kFixed) { error = type->name + " nodes carry per-node data and are made by their own tools"; return false; }
    if (type_number == 0xdc) { error = "touch_contact nodes are not written yet"; return false; }
    if (graph.nodes.size() >= 0x10000) { error = "a flow holds at most 65536 nodes"; return false; }
    Node node;
    node.type = type_number;
    for (const auto& port : type->ports) {
        if (port.kind == "resource_offset" || port.kind == "ptr_lo" || port.kind == "ptr_hi" ||
            port.kind == "lua_reference_array") {
            error = type->name + " nodes need data this editor does not write yet (" + port.name + ")"; return false;
        }
        std::uint32_t word = 0;
        if (port.direction == "in") {
            word = kNoSlot;
        } else if (port.direction != "static") {
            const auto size = port.kind == "string" ? 128u : kind_size(port.kind);
            if (!size) { error = type->name + " nodes have an output of unknown size (" + port.name + ")"; return false; }
            std::vector<std::uint8_t> initial(size, 0);
            if (port.kind == "unit") std::memcpy(initial.data(), &kNullUnit, 4);
            word = add_slot(graph, initial, size >= 8 ? 8 : 4);
        }
        node.words.push_back(word);
    }
    for (const auto& name : type->out_events) node.outputs.push_back({name, 0, {}});
    index = graph.nodes.size();
    graph.nodes.push_back(std::move(node));
    // engine event sources are found through the external event table by the engine's event name
    const char* lifecycle = type_number == 0x7 ? "unit_spawned" : type_number == 0xed ? "unit_unspawned"
                          : type_number == 0x117 ? "unit_changed" : type_number == 0x11b ? "level_spawned" : nullptr;
    if (lifecycle) graph.external_events.push_back({id32_from_id64(lifecycle), static_cast<std::uint16_t>(index), 0});
    // registration lists: controller buttons, controller axes
    if (type_number == 0x3d) graph.lists[0].push_back(static_cast<std::uint32_t>(index));
    if (type_number == 0x62) graph.lists[1].push_back(static_cast<std::uint32_t>(index));
    return true;
}

void remove_node(Graph& graph, std::size_t index) {
    if (index >= graph.nodes.size() || graph.nodes[index].empty) return;
    std::vector<std::uint32_t> written;  // the slots the node writes
    for_each_slot(graph.nodes[index], [&](std::uint32_t& slot, const std::string& direction) {
        if (direction != "in" && slot != kNoSlot) written.push_back(slot);
    });
    graph.nodes[index] = Node{};
    graph.nodes[index].empty = true;
    const auto to_it = [&](const Link& link) { return link.node == index; };
    for (auto& node : graph.nodes) {
        for (auto& list : node.outputs) list.links.erase(std::remove_if(list.links.begin(), list.links.end(), to_it), list.links.end());
        if (node.has_query) {
            node.query.erase(std::remove_if(node.query.begin(), node.query.end(), to_it), node.query.end());
            if (node.query.empty()) node.has_query = false;  // no list at all, as the game writes a node without queries
        }
    }
    auto& events = graph.external_events;
    events.erase(std::remove_if(events.begin(), events.end(), [&](const Graph::External& e) { return e.node == index; }), events.end());
    const auto drop = [&](std::vector<std::uint32_t>& list, std::uint32_t mask) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](std::uint32_t v) { return (v & mask) == index; }), list.end());
    };
    drop(graph.subroutines, 0xffffffffu);
    for (auto& list : graph.lists) drop(list, 0xffffffffu);
    drop(graph.query, 0xffffu);
    graph.extra.erase(std::remove_if(graph.extra.begin(), graph.extra.end(),
                                     [&](const std::array<std::uint32_t, 2>& e) { return (e[1] & 0xffffu) == index; }),
                      graph.extra.end());
    // inputs that read only what the node wrote
    std::vector<std::uint32_t> still;
    for (auto& node : graph.nodes)
        for_each_slot(node, [&](std::uint32_t& slot, const std::string& direction) {
            if (direction != "in") still.push_back(slot);
        });
    for (auto& node : graph.nodes)
        for_each_slot(node, [&](std::uint32_t& slot, const std::string& direction) {
            if (direction == "in" && std::find(written.begin(), written.end(), slot) != written.end() &&
                std::find(still.begin(), still.end(), slot) == still.end())
                slot = kNoSlot;
        });
}

namespace {
// which nodes write each slot (node, port index into node_ports)
std::map<std::uint32_t, std::vector<std::pair<std::size_t, std::size_t>>> slot_writers(const Graph& graph,
                                                                                       std::vector<std::vector<PortRef>>& ports) {
    std::map<std::uint32_t, std::vector<std::pair<std::size_t, std::size_t>>> writers;
    ports.resize(graph.nodes.size());
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        ports[i] = node_ports(graph, graph.nodes[i]);
        for (std::size_t p = 0; p < ports[i].size(); ++p)
            if (ports[i][p].direction == "out" && ports[i][p].slot != kNoSlot) writers[ports[i][p].slot].push_back({i, p});
    }
    return writers;
}
}  // namespace

bool pulled_output(const Graph& graph, std::size_t index, std::size_t port) {
    if (index >= graph.nodes.size() || graph.nodes[index].empty) return false;
    const auto& node = graph.nodes[index];
    const NodeType* type = node_type(node.type);
    if (!type) return false;
    switch (type->codec) {
    case Codec::kFixed:
    case Codec::kStory:
        return type->in_events.empty() && type->out_events.empty();
    case Codec::kNested:
        return port >= type->ports.size();  // the imported variables; the node's own handle is pushed
    case Codec::kLua:
        // the Lua function decides (its source definition, which the flow doesn't keep): pulled when a consumer
        // already pulls the node
        for (const auto& other : graph.nodes)
            for (const auto& q : other.query)
                if (q.node == index) return true;
        return false;
    }
    return false;
}

std::vector<std::vector<Link>> derived_queries(const Graph& graph) {
    std::vector<std::vector<PortRef>> ports;
    const auto writers = slot_writers(graph, ports);
    std::map<std::pair<std::size_t, std::size_t>, bool> pulled;
    std::vector<std::vector<Link>> out(graph.nodes.size());
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
        for (const auto& port : ports[i]) {
            if (port.direction != "in" || port.slot == kNoSlot) continue;
            const auto w = writers.find(port.slot);
            if (w == writers.end()) continue;
            for (const auto& [from, from_port] : w->second) {
                if (from == i) continue;
                auto known = pulled.find({from, from_port});
                if (known == pulled.end()) known = pulled.emplace(std::make_pair(from, from_port), pulled_output(graph, from, from_port)).first;
                if (known->second) out[i].push_back({static_cast<std::uint16_t>(from), kQueryEvent});
            }
        }
    return out;
}

void rebuild_query(Graph& graph, std::size_t index) {
    if (index >= graph.nodes.size() || graph.nodes[index].empty) return;
    auto lists = derived_queries(graph);
    auto& node = graph.nodes[index];
    node.query = std::move(lists[index]);
    node.has_query = !node.query.empty();
    node.query_offset = 0;
}

Interface flow_interface(const Graph& child) {
    Interface out;
    for (const auto& e : child.external_events)
        if (e.node < child.nodes.size() && !child.nodes[e.node].empty && child.nodes[e.node].type == 0x0 &&
            std::find(out.inputs.begin(), out.inputs.end(), e.id) == out.inputs.end())
            out.inputs.push_back(e.id);
    for (const auto& node : child.nodes)
        if (!node.empty && node.type == 0x17 && !node.words.empty() &&
            std::find(out.outputs.begin(), out.outputs.end(), node.words[0]) == out.outputs.end())
            out.outputs.push_back(node.words[0]);
    out.variables = child.variables;
    return out;
}

bool add_nested_node(Graph& graph, std::uint32_t type_number, const Interface& child, std::size_t& index, std::string& error) {
    if (type_number != 0xa && type_number != 0x11c) { error = "only level unit and level reference nodes are made here"; return false; }
    if (graph.nodes.size() >= 0x10000) { error = "a flow holds at most 65536 nodes"; return false; }
    for (std::size_t a = 0; a < child.outputs.size(); ++a)
        for (std::size_t b = a + 1; b < child.outputs.size(); ++b)
            if ((child.outputs[a] & 0xffffu) == (child.outputs[b] & 0xffffu)) {
                error = "two of its output events share their low 16 bits, which the game can't tell apart"; return false;
            }
    Node node;
    node.type = type_number;
    std::vector<std::uint8_t> handle(type_number == 0xa ? 4 : 8, 0);
    if (type_number == 0xa) std::memcpy(handle.data(), &kNullUnit, 4);
    node.words.push_back(add_slot(graph, handle, type_number == 0xa ? 4 : 8));
    node.input_events = child.inputs;
    for (const auto id : child.outputs) {
        char name[12];
        std::snprintf(name, sizeof(name), "#%08x", id);
        node.outputs.push_back({name, id, {}});
    }
    index = graph.nodes.size();
    graph.nodes.push_back(std::move(node));
    return true;
}

namespace {
struct LuaRow {
    const char *module, *function, *callbacks, *ports, *events;
    std::uint32_t word1;
    bool addressable;
    std::uint32_t uses;
};
const LuaRow kLuaRows[] = {
#include "stingray/flow/flow_lua_nodes.inc"
};
}  // namespace

const std::vector<LuaTemplate>& lua_templates() {
    static const std::vector<LuaTemplate> templates = [] {
        std::vector<LuaTemplate> out;
        for (const auto& row : kLuaRows) {
            LuaTemplate t;
            t.module = row.module;
            t.function = row.function;
            t.callbacks = split(row.callbacks);
            for (const auto& port : split(row.ports)) {
                const auto a = port.find(':'), b = port.find(':', a + 1);
                t.ports.push_back({port.substr(0, a), port.substr(a + 1, b - a - 1), port.substr(b + 1)});
            }
            t.events = split(row.events);
            t.word1 = row.word1;
            t.addressable = row.addressable;
            t.game_uses = row.uses;
            out.push_back(std::move(t));
        }
        return out;
    }();
    return templates;
}

bool add_lua_node(Graph& graph, const LuaTemplate& lua, std::size_t& index, std::string& error) {
    if (graph.nodes.size() >= 0x10000) { error = "a flow holds at most 65536 nodes"; return false; }
    if (lua.callbacks.empty()) { error = "a Lua node needs a function"; return false; }
    // its id: unique among the flow's Lua nodes (the script access table finds nodes by it)
    std::uint32_t id = 0;
    for (std::uint32_t attempt = 0;; ++attempt) {
        id = id32_from_id64(lua.module + "." + lua.function + "#" + std::to_string(graph.nodes.size()) + "#" + std::to_string(attempt));
        bool used = false;
        for (const auto& node : graph.nodes) used = used || (!node.empty && node.type == 0x11 && !node.words.empty() && node.words[0] == id);
        if (!used) break;
    }
    Node node;
    node.type = 0x11;
    node.words.assign(12, 0);
    node.words[0] = id;
    node.words[1] = lua.word1;
    node.words[2] = add_slot(graph, std::vector<std::uint8_t>(4, 0));  // callback data
    std::vector<std::uint8_t> cache;  // a {-2, 0} reference per callback
    for (std::size_t i = 0; i < lua.callbacks.size(); ++i) {
        const std::int32_t unset = -2;
        const std::uint32_t zero = 0;
        cache.insert(cache.end(), reinterpret_cast<const std::uint8_t*>(&unset), reinterpret_cast<const std::uint8_t*>(&unset) + 4);
        cache.insert(cache.end(), reinterpret_cast<const std::uint8_t*>(&zero), reinterpret_cast<const std::uint8_t*>(&zero) + 4);
    }
    node.words[3] = add_slot(graph, cache);
    for (const auto& function : lua.callbacks) node.callbacks.push_back({1, lua.module, function});
    for (const auto& port : lua.ports) {
        const auto tag = kind_tag(port.kind);
        if (tag == ~0u) { error = lua.function + " has a port of a kind the flow has no tag for (" + port.kind + ")"; return false; }
        std::uint32_t size = port.kind == "string" ? (port.direction == "in" ? 1u : 128u) : kind_size(port.kind);
        if (!size) { error = lua.function + " has a port of unknown size (" + port.name + ")"; return false; }
        std::vector<std::uint8_t> initial(size, 0);
        if (port.kind == "unit") std::memcpy(initial.data(), &kNullUnit, 4);
        const auto slot = add_slot(graph, initial, size >= 8 && port.kind != "string" ? 8 : 4);
        (port.direction == "in" ? node.lua_inputs : node.lua_outputs).push_back({tag, slot, port.name});
    }
    for (const auto& event : lua.events) node.outputs.push_back({event, 0, {}});
    index = graph.nodes.size();
    graph.nodes.push_back(std::move(node));
    if (lua.addressable) graph.extra.push_back({id, static_cast<std::uint32_t>(index)});
    return true;
}

}  // namespace dtglb::stingray::flow
