#include "stingray/lua_components.h"

#include <cmath>
#include <map>

#include "stingray/lua_constants.h"

namespace dtglb::stingray {

namespace {

// LuaJIT 2.1 opcodes (lj_bc.h order)
enum Op : std::uint8_t {
    MOV = 18, NOT = 19, CAT = 38, KSTR = 39, KCDATA = 40, KSHORT = 41, KNUM = 42, KPRI = 43, KNIL = 44, UGET = 45,
    FNEW = 51, TNEW = 52, TDUP = 53, GGET = 54, GSET = 55, TGETV = 56, TGETS = 57, TGETB = 58, TGETR = 59, TSETV = 60,
    TSETS = 61, TSETB = 62, CALLM = 65, CALL = 66, VARG = 71, RET1 = 76,
};

std::shared_ptr<LuaObject> unknown(std::string what) {
    auto object = std::make_shared<LuaObject>();
    object->kind = LuaObject::unknown;
    object->function = std::move(what);
    return object;
}

LuaData data_of(const LuaValue& value) {
    return std::visit([](const auto& v) -> LuaData { return v; }, value);
}

// table keys: a whole double is the same key as the integer
LuaData key_of(LuaData key) {
    if (const auto* d = std::get_if<double>(&key); d && std::floor(*d) == *d && std::abs(*d) < 9.0e15)
        return static_cast<std::int64_t>(*d);
    return key;
}

bool same_key(const LuaData& a, const LuaData& b) {
    if (a.index() != b.index()) return false;
    if (const auto* s = std::get_if<std::string>(&a)) return *s == std::get<std::string>(b);
    if (const auto* i = std::get_if<std::int64_t>(&a)) return *i == std::get<std::int64_t>(b);
    if (const auto* d = std::get_if<double>(&a)) return *d == std::get<double>(b);
    if (const auto* t = std::get_if<bool>(&a)) return *t == std::get<bool>(b);
    if (const auto* o = std::get_if<std::shared_ptr<LuaObject>>(&a)) return *o == std::get<std::shared_ptr<LuaObject>>(b);
    return false;
}

void set(LuaObject& table, LuaData key, LuaData value) {
    key = key_of(std::move(key));
    if (std::holds_alternative<std::monostate>(key)) return;
    for (auto& [k, v] : table.fields)
        if (same_key(k, key)) { v = std::move(value); return; }
    if (!std::holds_alternative<std::monostate>(value)) table.fields.emplace_back(std::move(key), std::move(value));
}

std::shared_ptr<LuaObject> table_of(const LuaTable& constant) {
    auto table = std::make_shared<LuaObject>();
    for (std::size_t i = 0; i < constant.array.size(); ++i) set(*table, static_cast<std::int64_t>(i), data_of(constant.array[i]));
    for (const auto& [k, v] : constant.hash) set(*table, data_of(k), data_of(v));
    return table;
}

LuaObject* as_table(const LuaData& value) {
    const auto* object = std::get_if<std::shared_ptr<LuaObject>>(&value);
    return object && *object && (*object)->kind == LuaObject::table ? object->get() : nullptr;
}

}  // namespace

const LuaData* LuaObject::get(const std::string& key) const {
    for (const auto& [k, v] : fields)
        if (const auto* s = std::get_if<std::string>(&k); s && *s == key) return &v;
    return nullptr;
}

const LuaData* LuaObject::get(std::int64_t index) const {
    for (const auto& [k, v] : fields)
        if (const auto* i = std::get_if<std::int64_t>(&k); i && *i == index) return &v;
    return nullptr;
}

namespace {

// runs the main chunk: the components it declares and the value it returns
bool run_main_chunk(const std::vector<std::uint8_t>& body, std::vector<Component>& components, LuaData& returned, std::string& error) {
    components.clear();
    returned = LuaData();
    std::vector<LuaPrototype> prototypes;
    if (!read_lua_prototypes(body, prototypes, error)) return false;
    if (prototypes.empty()) { error = "the Lua dump has no main chunk"; return false; }
    const LuaPrototype& main = prototypes.back();
    const auto gc = [&](std::uint32_t index) -> const std::variant<std::monostate, std::string, LuaTable>* {
        return index < main.gc.size() ? &main.gc[main.gc.size() - 1 - index] : nullptr;
    };
    const auto gc_string = [&](std::uint32_t index) {
        const auto* constant = gc(index);
        const auto* text = constant ? std::get_if<std::string>(constant) : nullptr;
        return text ? *text : std::string();
    };
    std::map<std::uint32_t, LuaData> r;
    std::map<std::string, LuaData> globals;
    const auto reg = [&](std::uint32_t at) -> LuaData {
        const auto found = r.find(at);
        return found == r.end() ? LuaData() : found->second;
    };
    for (const std::uint32_t ins : main.bytecode) {
        const std::uint32_t op = ins & 0xff, a = (ins >> 8) & 0xff, c = (ins >> 16) & 0xff, b = ins >> 24, d = ins >> 16;
        switch (op) {
            case KSTR: r[a] = gc_string(d); break;
            case KSHORT: r[a] = static_cast<std::int64_t>(static_cast<std::int16_t>(d)); break;
            case KNUM: r[a] = d < main.numbers.size() ? data_of(main.numbers[d]) : LuaData(); break;
            case KPRI: r[a] = d == 1 ? LuaData(false) : d == 2 ? LuaData(true) : LuaData(); break;
            case KNIL: for (std::uint32_t i = a; i <= d; ++i) r[i] = LuaData(); break;
            case MOV: r[a] = reg(d); break;
            case TNEW: r[a] = std::make_shared<LuaObject>(); break;
            case TDUP: {
                const auto* constant = gc(d);
                const auto* table = constant ? std::get_if<LuaTable>(constant) : nullptr;
                r[a] = table ? table_of(*table) : std::make_shared<LuaObject>();
                break;
            }
            case GGET: {
                const std::string name = gc_string(d);
                const auto found = globals.find(name);
                r[a] = found != globals.end() ? found->second : LuaData(unknown("global " + name));
                break;
            }
            case GSET: globals[gc_string(d)] = reg(a); break;
            case TGETS: {
                const LuaObject* table = as_table(reg(b));
                const LuaData* value = table ? table->get(gc_string(c)) : nullptr;
                r[a] = value ? *value : LuaData(unknown("field " + gc_string(c)));
                break;
            }
            case TSETS: if (LuaObject* table = as_table(reg(b))) set(*table, gc_string(c), reg(a)); break;
            case TSETB: if (LuaObject* table = as_table(reg(b))) set(*table, static_cast<std::int64_t>(c), reg(a)); break;
            case TSETV: if (LuaObject* table = as_table(reg(b))) set(*table, reg(c), reg(a)); break;
            case CALL: case CALLM: {
                // CALL A B C: the function in A, B - 1 arguments after it (CALLM: C + the open results, unknown here)
                const LuaData function = reg(a);
                const auto* callee = std::get_if<std::shared_ptr<LuaObject>>(&function);
                const bool global = callee && *callee && (*callee)->kind == LuaObject::unknown &&
                                    (*callee)->function.rfind("global ", 0) == 0;
                const std::uint32_t arguments = op == CALL ? (b ? b - 1 : 0) : c;
                if (global && (*callee)->function == "global component" && arguments >= 1 &&
                    std::holds_alternative<std::string>(reg(a + 1))) {
                    auto table = std::make_shared<LuaObject>();
                    components.push_back({std::get<std::string>(reg(a + 1)), table});
                    r[a] = table;
                } else if (global) {
                    auto call = std::make_shared<LuaObject>();
                    call->kind = LuaObject::call;
                    call->function = (*callee)->function.substr(7);
                    for (std::uint32_t i = 1; i <= arguments; ++i) call->arguments.push_back(reg(a + i));
                    r[a] = call;
                } else {
                    r[a] = unknown("call");
                }
                break;
            }
            case RET1: returned = reg(a); break;
            default:
                // anything else that writes register A leaves a value that isn't a constant
                if ((op >= NOT && op <= CAT) || op == KCDATA || op == UGET || op == FNEW || (op >= TGETV && op <= TGETR && op != TGETS) ||
                    op == VARG)
                    r[a] = unknown("value");
                break;
        }
    }
    for (auto& component : components) {
        const LuaData* data = component.data ? component.data->get("component_data") : nullptr;
        LuaObject* table = data ? as_table(*data) : nullptr;
        component.data = table ? std::get<std::shared_ptr<LuaObject>>(*data) : nullptr;
    }
    return true;
}

}  // namespace

bool read_lua_components(const std::vector<std::uint8_t>& body, std::vector<Component>& components, std::string& error) {
    LuaData returned;
    return run_main_chunk(body, components, returned, error);
}

bool read_lua_returned(const std::vector<std::uint8_t>& body, LuaData& returned, std::string& error) {
    std::vector<Component> components;
    return run_main_chunk(body, components, returned, error);
}

}  // namespace dtglb::stingray
