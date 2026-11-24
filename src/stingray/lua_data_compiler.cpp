#include "stingray/lua_data_compiler.h"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <variant>

namespace dtglb::stingray {

namespace {

// ---- the script -------------------------------------------------------------------------------------------------
struct Value;
struct Field {
    enum Key { positional, name, number } key = positional;
    std::string text;   // name key
    double numeric = 0; // number key
    std::unique_ptr<Value> value;
    int line = 0;
};
struct Value {
    enum Kind { nil, boolean, number, string, table, local } kind = nil;
    bool truth = false;
    double numeric = 0;
    std::string text;  // string, or a local's name
    std::vector<Field> fields;
    int line = 0;
};

struct Parser {
    const std::string& s;
    std::size_t at = 0;
    int line = 1;
    std::string error;

    void skip() {
        for (;;) {
            while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n')) {
                if (s[at] == '\n') ++line;
                ++at;
            }
            if (s.compare(at, 2, "--") == 0) {
                while (at < s.size() && s[at] != '\n') ++at;
                continue;
            }
            return;
        }
    }
    bool fail(const std::string& what) {
        if (error.empty()) error = "line " + std::to_string(line) + ": " + what;
        return false;
    }
    bool peek(char c) {
        skip();
        return at < s.size() && s[at] == c;
    }
    bool take(char c) {
        if (!peek(c)) return false;
        ++at;
        return true;
    }
    std::string name() {
        skip();
        const std::size_t start = at;
        if (at < s.size() && (std::isalpha(static_cast<unsigned char>(s[at])) || s[at] == '_'))
            while (at < s.size() && (std::isalnum(static_cast<unsigned char>(s[at])) || s[at] == '_')) ++at;
        return s.substr(start, at - start);
    }
    // a name followed by `=` (a field key), without consuming anything otherwise
    bool name_key(std::string& out) {
        skip();
        const std::size_t mark = at;
        const int mark_line = line;
        out = name();
        if (!out.empty() && peek('=') && s.compare(at, 2, "==") != 0) {
            ++at;
            return true;
        }
        at = mark;
        line = mark_line;
        return false;
    }
    bool string_literal(std::string& out) {
        const char quote = s[at++];
        while (at < s.size() && s[at] != quote) {
            char c = s[at++];
            if (c == '\n') return fail("a string runs past the end of its line");
            if (c == '\\' && at < s.size()) {
                c = s[at++];
                switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
                        int v = c - '0';
                        for (int k = 0; k < 2 && at < s.size() && std::isdigit(static_cast<unsigned char>(s[at])); ++k) v = v * 10 + (s[at++] - '0');
                        if (v > 255) return fail("a string escape above 255");
                        out += static_cast<char>(v);
                        break;
                    }
                    default: out += c; break;  // \\ \" \'
                }
            } else {
                out += c;
            }
        }
        if (at >= s.size()) return fail("a string has no end");
        ++at;
        return true;
    }
    bool number_literal(double& out) {
        skip();
        bool negative = false;
        while (at < s.size() && s[at] == '-') {
            negative = !negative;
            ++at;
            skip();
        }
        char* end = nullptr;
        const double v = std::strtod(s.c_str() + at, &end);
        if (end == s.c_str() + at) return fail("a number was expected");
        at = static_cast<std::size_t>(end - s.c_str());
        out = negative ? -v : v;
        return true;
    }
    bool value(Value& out) {
        skip();
        out.line = line;
        if (at >= s.size()) return fail("a value was expected");
        const char c = s[at];
        if (c == '"' || c == '\'') {
            out.kind = Value::string;
            return string_literal(out.text);
        }
        if (c == '-' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) {
            out.kind = Value::number;
            return number_literal(out.numeric);
        }
        if (c == '{') {
            ++at;
            out.kind = Value::table;
            for (;;) {
                if (take('}')) return true;
                Field f;
                f.line = line;
                std::string key;
                if (take('[')) {
                    skip();
                    if (at < s.size() && (s[at] == '"' || s[at] == '\'')) {
                        f.key = Field::name;
                        if (!string_literal(f.text)) return false;
                    } else {
                        f.key = Field::number;
                        if (!number_literal(f.numeric)) return false;
                        if (std::isnan(f.numeric)) return fail("a table key is not a number");
                    }
                    if (!take(']') || !take('=')) return fail("`] =` was expected after a bracketed key");
                } else if (name_key(key)) {
                    f.key = Field::name;
                    f.text = key;
                }
                f.value = std::make_unique<Value>();
                if (!value(*f.value)) return false;
                out.fields.push_back(std::move(f));
                if (!take(',') && !take(';')) {
                    if (take('}')) return true;
                    return fail("`,` or `}` was expected in a table");
                }
            }
        }
        const std::string word = name();
        if (word == "true" || word == "false") {
            out.kind = Value::boolean;
            out.truth = word == "true";
        } else if (word == "nil") {
            out.kind = Value::nil;
        } else if (!word.empty()) {
            out.kind = Value::local;
            out.text = word;
        } else {
            return fail(std::string("unexpected `") + c + "`");
        }
        return true;
    }
};

// ---- bytecode -----------------------------------------------------------------------------------------------------
// LuaJIT 2.1 opcodes (lj_bc.h)
enum Op : std::uint8_t {
    MOV = 18, KSTR = 39, KSHORT = 41, KNUM = 42, KPRI = 43, TNEW = 52, TDUP = 53, TSETV = 60, TSETS = 61, TSETB = 62,
    RET0 = 75, RET1 = 76,
};

void uleb(std::vector<std::uint8_t>& out, std::uint32_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

// a 33-bit uleb whose lowest bit says "number" (lj_bcwrite's knum form)
void uleb33(std::vector<std::uint8_t>& out, std::uint32_t v, bool number) {
    std::uint8_t first = static_cast<std::uint8_t>(((v & 0x3f) << 1) | (number ? 1 : 0));
    if (v < 0x40) {
        out.push_back(first);
        return;
    }
    out.push_back(static_cast<std::uint8_t>(first | 0x80));
    v >>= 6;
    uleb(out, v);
}

bool integral(double v, std::int32_t& k) {
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return false;
    k = static_cast<std::int32_t>(v);
    return static_cast<double>(k) == v;
}

// a template table constant: its array part (from index 0) and hash part, values as KTAB items
struct Template {
    std::vector<const Value*> array;  // index 0 first (always nil)
    std::vector<std::pair<const Field*, const Value*>> hash;
};

struct Compiler {
    std::vector<std::uint32_t> code;
    std::vector<int> lines;
    // gc constants by index (D = 0 first): strings, then templates as they come
    struct Constant {
        bool table = false;
        std::string text;
        Template tab;
    };
    std::vector<Constant> kgc;
    std::map<std::string, std::uint32_t> strings;
    std::vector<double> kn;
    std::map<std::uint64_t, std::uint32_t> numbers;
    std::map<std::string, int> locals;  // name -> register
    int frame = 1;
    std::string error;

    void emit(std::uint32_t ins, int line) {
        code.push_back(ins);
        lines.push_back(line);
    }
    static std::uint32_t ad(Op op, int a, std::uint32_t d) { return op | (static_cast<std::uint32_t>(a) << 8) | (d << 16); }
    static std::uint32_t abc(Op op, int a, int b, int c) {
        return op | (static_cast<std::uint32_t>(a) << 8) | (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(b) << 24);
    }
    void use(int reg) {
        if (reg + 1 > frame) frame = reg + 1;
    }
    bool too_many(std::size_t index, const char* what) {
        if (index <= 0xffff) return false;
        error = std::string("more ") + what + " than one Lua function can hold (65536)";
        return true;
    }
    std::uint32_t string_constant(const std::string& text) {
        const auto found = strings.find(text);
        if (found != strings.end()) return found->second;
        const auto index = static_cast<std::uint32_t>(kgc.size());
        kgc.push_back({false, text, {}});
        strings.emplace(text, index);
        return index;
    }
    std::uint32_t number_constant(double v) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, 8);
        const auto found = numbers.find(bits);
        if (found != numbers.end()) return found->second;
        const auto index = static_cast<std::uint32_t>(kn.size());
        kn.push_back(v);
        numbers.emplace(bits, index);
        return index;
    }
    static bool constant(const Value& v) { return v.kind == Value::boolean || v.kind == Value::number || v.kind == Value::string; }

    // a number into a register
    bool load_number(double v, int reg, int line) {
        use(reg);
        std::int32_t k = 0;
        if (integral(v, k) && k >= -32768 && k <= 32767 && !(v == 0 && std::signbit(v)))
            emit(ad(KSHORT, reg, static_cast<std::uint16_t>(static_cast<std::int16_t>(k))), line);
        else {
            const auto index = number_constant(v);
            if (too_many(index, "numbers")) return false;
            emit(ad(KNUM, reg, index), line);
        }
        return true;
    }

    // the value in a register: a local's own, else loaded or built at `free`; returns the register (-1 on error)
    int place(const Value& v, int free) {
        switch (v.kind) {
            case Value::local: {
                const auto found = locals.find(v.text);
                if (found == locals.end()) {
                    error = "line " + std::to_string(v.line) + ": `" + v.text + "` is not a local of the script";
                    return -1;
                }
                return found->second;
            }
            case Value::table: return build(v, free) ? free : -1;
            case Value::nil: case Value::boolean:
                use(free);
                emit(ad(KPRI, free, v.kind == Value::nil ? 0 : v.truth ? 2 : 1), v.line);
                return free;
            case Value::number: return load_number(v.numeric, free, v.line) ? free : -1;
            case Value::string: {
                use(free);
                const auto index = string_constant(v.text);
                if (too_many(index, "constants")) return -1;
                emit(ad(KSTR, free, index), v.line);
                return free;
            }
        }
        return -1;
    }

    // a table constructor built into register `reg`
    bool build(const Value& v, int reg) {
        use(reg);
        Template tab;
        std::vector<const Field*> rest;
        std::uint32_t position = 0, constant_positions = 0;
        std::size_t hash_count = 0;
        for (const auto& f : v.fields) {
            if (f.key == Field::positional) ++position;
            if (f.value->kind == Value::nil) continue;  // nothing to store
            if (!constant(*f.value)) {
                rest.push_back(&f);
                continue;
            }
            if (f.key == Field::positional) {
                if (tab.array.empty()) tab.array.push_back(nullptr);
                while (tab.array.size() < position) tab.array.push_back(nullptr);
                tab.array.push_back(f.value.get());
                ++constant_positions;
            } else {
                tab.hash.emplace_back(&f, f.value.get());
                ++hash_count;
            }
        }
        const int line = v.line;
        if (!tab.array.empty() || !tab.hash.empty()) {
            const auto index = static_cast<std::uint32_t>(kgc.size());
            if (too_many(index, "constants")) return false;
            kgc.push_back({true, std::string(), std::move(tab)});
            emit(ad(TDUP, reg, index), line);
        } else {
            // size hints as LuaJIT's: array slots (index 0 included, at least 3) and log2 of the hash size
            std::uint32_t narr = position ? std::max<std::uint32_t>(position + 1, 3) : 0;
            narr = std::min<std::uint32_t>(narr, 0x7ff);
            std::uint32_t nhash = static_cast<std::uint32_t>(rest.size()) - std::min<std::uint32_t>(position, static_cast<std::uint32_t>(rest.size()));
            std::uint32_t hbits = 0;
            while (nhash && (1u << hbits) < nhash) ++hbits;
            emit(ad(TNEW, reg, narr | (hbits << 11)), line);
        }
        // the other fields, in order
        position = 0;
        for (const auto& f : v.fields) {
            if (f.key == Field::positional) ++position;
            if (f.value->kind == Value::nil || constant(*f.value)) continue;
            const int value = place(*f.value, reg + 1);
            if (value < 0) return false;
            const int key_reg = value == reg + 1 ? reg + 2 : reg + 1;
            if (f.key == Field::name) {
                const auto index = string_constant(f.text);
                if (too_many(index, "constants")) return false;
                if (index <= 255) {
                    emit(abc(TSETS, value, reg, static_cast<int>(index)), f.line);
                } else {
                    use(key_reg);
                    emit(ad(KSTR, key_reg, index), f.line);
                    emit(abc(TSETV, value, reg, key_reg), f.line);
                }
            } else {
                const double key = f.key == Field::positional ? static_cast<double>(position) : f.numeric;
                std::int32_t k = 0;
                if (integral(key, k) && k >= 0 && k <= 255) {
                    emit(abc(TSETB, value, reg, k), f.line);
                } else {
                    if (!load_number(key, key_reg, f.line)) return false;
                    emit(abc(TSETV, value, reg, key_reg), f.line);
                }
            }
        }
        return true;
    }
};

void ktab_item(std::vector<std::uint8_t>& out, const Value* v) {
    if (!v || v->kind == Value::nil) { out.push_back(0); return; }
    if (v->kind == Value::boolean) { out.push_back(v->truth ? 2 : 1); return; }
    if (v->kind == Value::string) {
        uleb(out, static_cast<std::uint32_t>(5 + v->text.size()));
        out.insert(out.end(), v->text.begin(), v->text.end());
        return;
    }
    std::int32_t k = 0;
    if (integral(v->numeric, k)) {
        out.push_back(3);
        uleb(out, static_cast<std::uint32_t>(k));
        return;
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v->numeric, 8);
    out.push_back(4);
    uleb(out, static_cast<std::uint32_t>(bits));
    uleb(out, static_cast<std::uint32_t>(bits >> 32));
}

}  // namespace

bool compile_lua_data(const std::string& source, const std::string& chunk_name, LuaDumpForm form, std::vector<std::uint8_t>& dump,
                      std::string& error) {
    Parser p{source};
    Compiler c;
    struct Local {
        std::string name;
        std::uint32_t start = 0;  // first instruction it is live for (the function header is instruction 0)
    };
    std::vector<Local> declared;
    std::vector<std::unique_ptr<Value>> statements;  // kept: the constants point into them until the dump is written
    bool returned = false;
    for (;;) {
        p.skip();
        if (p.at >= source.size()) break;
        if (returned) { error = "line " + std::to_string(p.line) + ": nothing may follow the return"; return false; }
        const int line = p.line;
        const std::string word = p.name();
        if (word == "local") {
            const std::string name = p.name();
            if (name.empty() || !p.take('=')) { error = "line " + std::to_string(line) + ": `local <name> =` was expected"; return false; }
            statements.push_back(std::make_unique<Value>());
            Value& v = *statements.back();
            if (!p.value(v)) { error = p.error; return false; }
            const int reg = static_cast<int>(declared.size());
            const int placed = c.place(v, reg);
            if (placed < 0) { error = c.error; return false; }
            if (placed != reg) {  // a local naming another: its own copy
                c.use(reg);
                c.emit(Compiler::ad(MOV, reg, static_cast<std::uint32_t>(placed)), line);
            }
            if (c.locals.count(name)) { error = "line " + std::to_string(line) + ": `" + name + "` is declared twice"; return false; }
            c.locals[name] = reg;
            declared.push_back({name, static_cast<std::uint32_t>(c.code.size() + 1)});
        } else if (word == "return") {
            statements.push_back(std::make_unique<Value>());
            Value& v = *statements.back();
            if (!p.value(v)) { error = p.error; return false; }
            const int reg = c.place(v, static_cast<int>(declared.size()));
            if (reg < 0) { error = c.error; return false; }
            c.emit(Compiler::ad(RET1, reg, 2), p.line);
            returned = true;
        } else {
            error = "line " + std::to_string(line) + ": `local` or `return` was expected";
            return false;
        }
    }
    if (!returned) c.emit(Compiler::ad(RET0, 0, 1), p.line);
    if (c.frame > 250) { error = "the script's tables nest too deep"; return false; }
    const int lines_total = p.line;

    // the main function (lj_bcwrite: header, bytecode, constants, debug info)
    std::vector<std::uint8_t> body, debug;
    const std::size_t line_size = lines_total < 256 ? 1 : lines_total < 65536 ? 2 : 4;
    for (const int line : c.lines)
        for (std::size_t b = 0; b < line_size; ++b) debug.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(line) >> (8 * b)));
    std::uint32_t last = 0;
    const auto end = static_cast<std::uint32_t>(c.code.size() + 1);
    for (const auto& local : declared) {
        debug.insert(debug.end(), local.name.begin(), local.name.end());
        debug.push_back(0);
        uleb(debug, local.start - last);
        uleb(debug, end - local.start);
        last = local.start;
    }
    debug.push_back(0);
    body.push_back(0x02);  // vararg (the main chunk)
    body.push_back(0);     // parameters
    body.push_back(static_cast<std::uint8_t>(c.frame));
    body.push_back(0);     // upvalues
    uleb(body, static_cast<std::uint32_t>(c.kgc.size()));
    uleb(body, static_cast<std::uint32_t>(c.kn.size()));
    uleb(body, static_cast<std::uint32_t>(c.code.size()));
    uleb(body, static_cast<std::uint32_t>(debug.size()));
    uleb(body, 0);  // first line
    uleb(body, static_cast<std::uint32_t>(lines_total));
    for (const auto ins : c.code)
        for (int b = 0; b < 4; ++b) body.push_back(static_cast<std::uint8_t>(ins >> (8 * b)));
    // gc constants from the highest index down (instruction operand d names kgc[-d-1])
    for (auto it = c.kgc.rbegin(); it != c.kgc.rend(); ++it) {
        if (!it->table) {
            uleb(body, static_cast<std::uint32_t>(5 + it->text.size()));
            body.insert(body.end(), it->text.begin(), it->text.end());
            continue;
        }
        body.push_back(1);
        uleb(body, static_cast<std::uint32_t>(it->tab.array.size()));
        uleb(body, static_cast<std::uint32_t>(it->tab.hash.size()));
        for (const Value* v : it->tab.array) ktab_item(body, v);
        for (const auto& [field, value] : it->tab.hash) {
            Value key;
            if (field->key == Field::name) {
                key.kind = Value::string;
                key.text = field->text;
            } else {
                key.kind = Value::number;
                key.numeric = field->numeric;
            }
            ktab_item(body, &key);
            ktab_item(body, value);
        }
    }
    for (const double v : c.kn) {
        std::int32_t k = 0;
        if (integral(v, k) && !(v == 0 && std::signbit(v))) {
            uleb33(body, static_cast<std::uint32_t>(k), false);
        } else {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &v, 8);
            uleb33(body, static_cast<std::uint32_t>(bits), true);
            uleb(body, static_cast<std::uint32_t>(bits >> 32));
        }
    }
    body.insert(body.end(), debug.begin(), debug.end());

    dump.clear();
    dump.push_back(0x1b);
    if (form == LuaDumpForm::game) {
        dump.push_back('F');
        dump.push_back('S');
        dump.push_back(0x82);
        uleb(dump, 0);
    } else {
        dump.push_back('L');
        dump.push_back('J');
        dump.push_back(2);
        uleb(dump, 8);  // two-slot frames (64-bit LuaJIT)
    }
    uleb(dump, static_cast<std::uint32_t>(chunk_name.size()));
    dump.insert(dump.end(), chunk_name.begin(), chunk_name.end());
    uleb(dump, static_cast<std::uint32_t>(body.size()));
    dump.insert(dump.end(), body.begin(), body.end());
    dump.push_back(0);
    return true;
}

std::vector<std::uint8_t> lua_resource_body(const std::vector<std::uint8_t>& dump) {
    std::vector<std::uint8_t> out;
    const auto size = static_cast<std::uint32_t>(dump.size());
    for (const std::uint32_t w : {0u, size, 40u, 2u, 0u, size + 40u})
        for (int b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>(w >> (8 * b)));
    out.insert(out.end(), dump.begin(), dump.end());
    return out;
}

}  // namespace dtglb::stingray
