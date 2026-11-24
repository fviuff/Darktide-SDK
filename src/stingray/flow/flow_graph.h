#pragma once
// A compiled flow graph (a unit's flow, a level's flow, a standalone .flow resource) with every node typed: the node
// type's name, its ports (one payload word each: a slot in the dynamic data, or a static value), its output events
// with their links, and the per-node data some types carry (nested graphs: the child's events and variables; Lua
// nodes: callbacks and ports; stories: marker times). read_graph and write_graph are exact inverses for every flow the
// game ships.
//
// Container: a 0x5c-byte header of u32 words, then
//   node index (u32 offset per node) | external events {id32, node | in << 16} | extra pairs {u32, u32} |
//   variables {u32, u32, u32} | query {count, u32...} [pad 8] | preload {u32, u32} | subroutines (u32 node index) |
//   4 u32 lists [pad 8] | node records (8-aligned).
// Node record: {type, query offset (into the flow, 0 = none), payload}. Payload in order:
//   the type's port words | nested: {count, offset} for input events, output events, input variables, output
//   variables | output event lists {count, links} | nested: input event id32s, output event id32s, variable rows
//   {id32, slot, byte count} (inputs then outputs) | Lua: callbacks {u8 mode, module\0, function\0} (4-aligned),
//   input ports {u32 type, u32 slot, name\0} (each 4-aligned), output ports (packed, then 4-aligned), output event
//   names (packed, 4-aligned at the end) | query list {count, links} | zero padding to 8 bytes.
// A link is node | input event << 16; input event 0xffff asks the target to pull its variables (query).
// Data connections share a slot; constants are stored in the slot.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dtglb::stingray::flow {

constexpr std::uint32_t kNoSlot = 0xffffffffu;
constexpr std::uint16_t kQueryEvent = 0xffff;
constexpr std::uint32_t kNullUnit = 0x0001ffff;  // a unit slot holding no unit

enum class Codec { kFixed, kNested, kLua, kStory };

struct PortType {
    std::string direction;  // in, out, state (runtime storage), static (a value in the word itself)
    std::string kind;       // float, vector3, unit, string, typed (the type tag is the word before), ...
    std::string name;
};
struct NodeType {
    std::uint32_t number = 0;
    std::string name;  // empty: not identified
    Codec codec = Codec::kFixed;
    std::vector<std::string> in_events, out_events;  // nested types: plus the child's; Lua: per node
    std::vector<PortType> ports;                     // one per payload word, in order
    std::uint32_t game_uses = 0;                     // nodes of this type in the game's own flows
};
const std::vector<NodeType>& node_types();
const NodeType* node_type(std::uint32_t number);
const NodeType* node_type(const std::string& name);
// the kind named by a flow type tag (typed ports, nested variables, Lua ports), empty when unknown
std::string tag_kind(std::uint32_t tag);
// bytes a value of this kind takes in the dynamic data; 0 for strings and kinds without a slot
std::uint32_t kind_size(const std::string& kind);

struct Link {
    std::uint16_t node = 0, event = 0;
};
struct EventList {
    std::string name;      // the type's event name; nested: #<id32>; Lua: the node's name; story markers: marker_<i>
    std::uint32_t id = 0;  // nested: the child's output event (IdString32)
    std::vector<Link> links;
};
struct NestedVariable {
    std::uint32_t id = 0;    // the child's variable (IdString32)
    std::uint32_t slot = kNoSlot;
    std::uint32_t size = 0;  // byte count (128: a string)
};
struct LuaCallback {
    std::uint8_t mode = 1;  // 1: module + function, 0: function only
    std::string module, function;
};
struct LuaPort {
    std::uint32_t type = 0;  // flow type tag
    std::uint32_t slot = kNoSlot;
    std::string name;
};

struct Node {
    bool empty = false;  // an index entry without a record
    std::uint32_t type = 0;
    std::vector<std::uint32_t> words;  // one per port of the type (counts and offsets the codec computes included)
    std::vector<EventList> outputs;
    bool has_query = false;            // a query list in the record
    std::vector<Link> query;
    std::uint32_t query_offset = 0;    // without a query list in the record: the stored offset
    std::vector<std::uint32_t> input_events;  // nested: the child's input events (IdString32)
    std::vector<NestedVariable> input_variables, output_variables;
    std::vector<LuaCallback> callbacks;       // Lua: one per input event
    std::vector<LuaPort> lua_inputs, lua_outputs;
};

struct Graph {
    struct External {
        std::uint32_t id = 0;  // event name (IdString32)
        std::uint16_t node = 0, in = 0;
    };
    std::vector<External> external_events;
    std::vector<std::array<std::uint32_t, 2>> extra, preload;
    std::vector<std::array<std::uint32_t, 3>> variables;
    std::vector<std::uint32_t> query, subroutines;
    std::array<std::vector<std::uint32_t>, 4> lists;
    std::vector<Node> nodes;
    std::uint32_t dynamic_size = 0;  // header word: the dynamic data's size
    std::vector<std::uint8_t> dynamic;
};

bool read_graph(const std::vector<std::uint8_t>& flow, const std::vector<std::uint8_t>& dynamic, Graph& graph,
                std::string& error);
bool write_graph(const Graph& graph, std::vector<std::uint8_t>& flow, std::string& error);

// The ports of a node with their slots: the type's ports, then (nested) the variables, (Lua) the Lua ports.
struct PortRef {
    std::string name, direction, kind;
    std::uint32_t slot = kNoSlot;  // static ports: kNoSlot (the value is the word)
    std::uint32_t size = 0;        // bytes the slot holds (strings: up to the NUL)
};
std::vector<PortRef> node_ports(const Graph& graph, const Node& node);
// the word behind node_ports(...)[port] (its slot, or a static port's value); null when there is none
std::uint32_t* port_word(Node& node, std::size_t port);
// a value in the dynamic data as text (float "1.5", vector3 "x, y, z", quaternion "x, y, z, w", bool "true", id32
// "#<8 hex>", id64 16 hex, integers, strings as they are); empty for kinds without a text form or no slot
std::string value_text(const std::string& kind, const std::vector<std::uint8_t>& dynamic, std::uint32_t slot);
// text (as value_text writes it; id32 / id64 may also be names) as a value's bytes; false when it doesn't read as one
bool value_bytes(const std::string& kind, const std::string& text, std::vector<std::uint8_t>& out);
// the flow type tag of a kind (typed ports take it in the word before), ~0u when it has none
std::uint32_t kind_tag(const std::string& kind);

// Editing. Node numbers never move: a removed node stays as an empty index entry (as the game's own flows have them).
// The container tables follow the nodes as the game's flows have them: subroutines list every flow_subroutine
// (0x25) node, list 0 every 0x3d node.
// Appends a value to the dynamic data (aligned); its offset.
std::uint32_t add_slot(Graph& graph, const std::vector<std::uint8_t>& value, std::uint32_t alignment = 4);
// A new node of a fixed-layout type at the end: inputs unset, outputs and state with slots of their own (a unit output
// holds no unit), static words 0, no links; engine event sources (unit_spawned, unit_unspawned, unit_changed,
// level_spawned) get their external event row, controller nodes their registration list. Its number, or false with the reason.
bool add_node(Graph& graph, std::uint32_t type, std::size_t& index, std::string& error);
// Takes a node out: links, query links and external events to it go, inputs that read only its outputs become unset.
void remove_node(Graph& graph, std::size_t index);
// Whether inputs reading this output (index into node_ports) pull its node with a query link before they run: outputs
// of nodes without events (getters) and nested nodes' imported variables; a Lua node's when a consumer already pulls
// it. As the game's flows have it.
bool pulled_output(const Graph& graph, std::size_t node, std::size_t port);
// every node's query list as its inputs ask for it: per input in port order, a query link to each other node writing
// its slot whose output is pulled
std::vector<std::vector<Link>> derived_queries(const Graph& graph);
// sets a node's query list to the derived one
void rebuild_query(Graph& graph, std::size_t node);

// A flow seen from a node that stands for it in a parent flow (level unit, nested level, subroutine): the input events
// a parent can send (its external_event nodes' names), the output events it sends up (its subroutine_out_event names),
// its named variables {id32, slot, type tag}.
struct Interface {
    std::vector<std::uint32_t> inputs, outputs;
    std::vector<std::array<std::uint32_t, 3>> variables;
};
Interface flow_interface(const Graph& child);
// A level unit (0xa) or level reference (0x11c) node importing every input and output event of the child's interface
// (no variables); the level binds it to its unit / nested level by a row {placed number, node, non-empty outputs}.
bool add_nested_node(Graph& graph, std::uint32_t type, const Interface& child, std::size_t& index, std::string& error);

// A Lua node the game's flows use, as most of its nodes are laid out: the module and function its first callback calls,
// the callbacks (one per input event), its ports (in / out, kind, name), output events, payload word 1, whether the
// game lists such nodes in the script access table (the "extra" table: {node id, node}).
struct LuaTemplate {
    std::string module, function;
    std::vector<std::string> callbacks;
    std::vector<PortType> ports;
    std::vector<std::string> events;
    std::uint32_t word1 = 0;
    bool addressable = false;
    std::uint32_t game_uses = 0;
};
const std::vector<LuaTemplate>& lua_templates();
// A new Lua node: a fresh node id, callback data 0, a {-2, 0} reference cache entry per callback, every port with a slot
// of its own (inputs empty / no unit), listed for script access when its kind is.
bool add_lua_node(Graph& graph, const LuaTemplate& lua, std::size_t& index, std::string& error);

// every slot word of a node (type ports with a slot, nested variables, Lua ports) with its direction
void for_each_slot(Node& node, const std::function<void(std::uint32_t& slot, const std::string& direction)>& visit);

}  // namespace dtglb::stingray::flow
