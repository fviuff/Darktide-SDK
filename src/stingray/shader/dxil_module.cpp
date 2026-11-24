#include "stingray/shader/dxil_module.h"

#include <cmath>
#include <cstring>
#include <functional>

namespace dtglb::stingray::shader {
namespace {

std::uint32_t u32(const std::vector<std::uint8_t>& data, std::size_t at) {
    std::uint32_t value = 0;
    if (at + 4 <= data.size()) std::memcpy(&value, data.data() + at, 4);
    return value;
}

// ---------------------------------------------------------------------------------------------------------
// Bitstream

struct Bits {
    const std::vector<std::uint8_t>& data;
    std::uint64_t pos = 0, end = 0;
    bool failed = false;

    explicit Bits(const std::vector<std::uint8_t>& d, std::size_t start) : data(d), pos(start * 8), end(d.size() * 8) {}

    std::uint64_t read(unsigned width) {
        if (!width) return 0;
        if (pos + width > end) { failed = true; pos = end; return 0; }
        std::uint64_t value = 0;
        for (unsigned got = 0; got < width;) {
            const std::uint64_t byte = data[pos >> 3];
            const unsigned shift = pos & 7, take = std::min<unsigned>(8 - shift, width - got);
            value |= ((byte >> shift) & ((1u << take) - 1)) << got;
            got += take;
            pos += take;
        }
        return value;
    }
    std::uint64_t vbr(unsigned width) {
        std::uint64_t value = 0;
        const std::uint64_t top = 1ull << (width - 1);
        for (unsigned shift = 0; shift < 64 && !failed; shift += width - 1) {
            const std::uint64_t piece = read(width);
            value |= (piece & (top - 1)) << shift;
            if (!(piece & top)) return value;
        }
        failed = true;
        return value;
    }
    void align32() { pos = (pos + 31) & ~31ull; }
};

struct AbbrevOp {
    enum Kind { Literal, Fixed, Vbr, Array, Char6, Blob } kind = Literal;
    std::uint64_t value = 0;
};
using Abbrev = std::vector<AbbrevOp>;

const char kChar6[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";

Abbrev read_abbrev(Bits& bits) {
    Abbrev ops;
    const auto count = bits.vbr(5);
    for (std::uint64_t i = 0; i < count && !bits.failed; ++i) {
        if (bits.read(1)) {
            ops.push_back({AbbrevOp::Literal, bits.vbr(8)});
        } else {
            const auto encoding = bits.read(3);
            if (encoding == 1) ops.push_back({AbbrevOp::Fixed, bits.vbr(5)});
            else if (encoding == 2) ops.push_back({AbbrevOp::Vbr, bits.vbr(5)});
            else if (encoding == 3) ops.push_back({AbbrevOp::Array, 0});
            else if (encoding == 4) ops.push_back({AbbrevOp::Char6, 0});
            else if (encoding == 5) ops.push_back({AbbrevOp::Blob, 0});
            else bits.failed = true;
        }
    }
    return ops;
}

std::uint64_t scalar(Bits& bits, const AbbrevOp& op) {
    switch (op.kind) {
        case AbbrevOp::Literal: return op.value;
        case AbbrevOp::Fixed: return bits.read(static_cast<unsigned>(op.value));
        case AbbrevOp::Vbr: return bits.vbr(static_cast<unsigned>(op.value));
        case AbbrevOp::Char6: return static_cast<std::uint8_t>(kChar6[bits.read(6)]);
        default: bits.failed = true; return 0;
    }
}

struct Block {
    unsigned width = 2;
    std::vector<Abbrev> abbrevs;
};

struct Item {
    enum Kind { Record, Enter, End } kind = End;
    std::uint64_t code = 0;
    std::vector<std::uint64_t> ops;
    std::uint64_t id = 0, length = 0;
    Block sub;
};

using BlockInfo = std::map<std::uint64_t, std::vector<Abbrev>>;

// the next record / sub-block / end of one block
bool next_item(Bits& bits, const BlockInfo& info, Block& block, Item& item) {
    item = {};
    for (;;) {
        if (bits.pos >= bits.end || bits.failed) return false;
        const auto abbrev = bits.read(block.width);
        if (abbrev == 0) { bits.align32(); item.kind = Item::End; return true; }
        if (abbrev == 1) {
            item.kind = Item::Enter;
            item.id = bits.vbr(8);
            item.sub.width = static_cast<unsigned>(bits.vbr(4));
            bits.align32();
            item.length = bits.read(32);
            const auto found = info.find(item.id);
            if (found != info.end()) item.sub.abbrevs = found->second;
            return true;
        }
        if (abbrev == 2) { block.abbrevs.push_back(read_abbrev(bits)); continue; }
        item.kind = Item::Record;
        if (abbrev == 3) {
            item.code = bits.vbr(6);
            const auto count = bits.vbr(6);
            for (std::uint64_t i = 0; i < count && !bits.failed; ++i) item.ops.push_back(bits.vbr(6));
            return !bits.failed;
        }
        if (abbrev - 4 >= block.abbrevs.size()) { bits.failed = true; return false; }
        const Abbrev& ops = block.abbrevs[abbrev - 4];
        std::vector<std::uint64_t> values;
        for (std::size_t i = 0; i < ops.size() && !bits.failed; ++i) {
            if (ops[i].kind == AbbrevOp::Array) {
                if (i + 1 >= ops.size()) { bits.failed = true; break; }
                const auto count = bits.vbr(6);
                for (std::uint64_t k = 0; k < count && !bits.failed; ++k) values.push_back(scalar(bits, ops[i + 1]));
                ++i;
            } else if (ops[i].kind == AbbrevOp::Blob) {
                const auto size = bits.vbr(6);
                bits.align32();
                for (std::uint64_t k = 0; k < size && !bits.failed; ++k) values.push_back(bits.read(8));
                bits.align32();
            } else {
                values.push_back(scalar(bits, ops[i]));
            }
        }
        if (values.empty()) { bits.failed = true; return false; }
        item.code = values.front();
        item.ops.assign(values.begin() + 1, values.end());
        return !bits.failed;
    }
}

std::int64_t sign_rotated(std::uint64_t value) {
    if (!(value & 1)) return static_cast<std::int64_t>(value >> 1);
    if (value != 1) return -static_cast<std::int64_t>(value >> 1);
    return INT64_MIN;
}

float float_bits(std::uint64_t bits) {
    const auto b = static_cast<std::uint32_t>(bits);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

double double_bits(std::uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

double half_bits(std::uint64_t bits) {
    const std::uint32_t h = static_cast<std::uint32_t>(bits) & 0xffff;
    const std::uint32_t sign = h >> 15, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    double value;
    if (exponent == 0) value = std::ldexp(static_cast<double>(mantissa), -24);
    else if (exponent == 31) value = mantissa ? NAN : INFINITY;
    else value = std::ldexp(static_cast<double>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// Container

std::map<std::string, std::vector<std::uint8_t>> parse_container(const std::vector<std::uint8_t>& data, std::string& error) {
    std::map<std::string, std::vector<std::uint8_t>> parts;
    if (data.size() < 32 || std::memcmp(data.data(), "DXBC", 4) != 0) { error = "not a DXBC container"; return parts; }
    const auto count = u32(data, 28);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto offset = u32(data, 32 + static_cast<std::size_t>(i) * 4);
        const auto size = u32(data, offset + 4);
        if (offset + 8ull + size > data.size()) { error = "DXBC part runs past the container"; return {}; }
        parts[std::string(reinterpret_cast<const char*>(data.data() + offset), 4)] =
            std::vector<std::uint8_t>(data.begin() + offset + 8, data.begin() + offset + 8 + size);
    }
    return parts;
}

bool program_bitcode(const std::vector<std::uint8_t>& part, std::uint32_t& kind, std::vector<std::uint8_t>& bitcode,
                     std::string& error) {
    if (part.size() < 24 || std::memcmp(part.data() + 8, "DXIL", 4) != 0) { error = "not a DXIL program"; return false; }
    kind = u32(part, 0) >> 16;
    const auto offset = u32(part, 16), size = u32(part, 20);
    if (8ull + offset + size > part.size()) { error = "DXIL program runs past its part"; return false; }
    bitcode.assign(part.begin() + 8 + offset, part.begin() + 8 + offset + size);
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Module

struct ModuleReader {
    Module& m;
    Bits& bits;
    BlockInfo info;
    std::string error;

    const Type* type(std::uint64_t index) {
        if (index < m.types_.size()) return m.types_[index];
        error = "type index out of range";
        return nullptr;
    }

    Value* new_value(Value::Kind kind, const Type* t) {
        Value& v = m.value_storage_.emplace_back();
        v.kind = kind;
        v.type = t;
        return &v;
    }

    void skip(std::uint64_t length) { bits.pos += length * 32; }

    bool blockinfo(Block& block) {
        std::uint64_t current = 0;
        for (;;) {
            const auto abbrev = bits.read(block.width);
            if (bits.failed) return false;
            if (abbrev == 0) { bits.align32(); return true; }
            if (abbrev == 2) {
                info[current].push_back(read_abbrev(bits));
            } else if (abbrev == 3) {
                const auto code = bits.vbr(6);
                std::vector<std::uint64_t> ops;
                for (auto count = bits.vbr(6); count--;) ops.push_back(bits.vbr(6));
                if (code == 1 && !ops.empty()) current = ops[0];
            } else {
                error = "unexpected item in BLOCKINFO";
                return false;
            }
        }
    }

    bool types(Block& block) {
        std::string name;
        struct Pending { Type* t; std::vector<std::uint64_t> refs; };
        std::vector<Pending> pending;
        Item item;
        while (next_item(bits, info, block, item)) {
            if (item.kind == Item::Enter) { skip(item.length); continue; }
            if (item.kind == Item::End) break;
            const auto& ops = item.ops;
            if (item.code == 1) continue;
            if (item.code == 19) {
                name.clear();
                for (auto c : ops) name += static_cast<char>(c);
                continue;
            }
            Type& t = m.type_storage_.emplace_back();
            switch (item.code) {
                case 2: t.kind = Type::Void; break;
                case 3: t.kind = Type::Float; break;
                case 4: t.kind = Type::Double; break;
                case 10: t.kind = Type::Half; break;
                case 5: t.kind = Type::Label; break;
                case 16: t.kind = Type::Metadata; break;
                case 7: t.kind = Type::Int; t.width = ops.empty() ? 0 : static_cast<std::uint32_t>(ops[0]); break;
                case 8: t.kind = Type::Pointer; pending.push_back({&t, {ops.empty() ? 0 : ops[0]}}); break;
                case 11: case 12:
                    t.kind = item.code == 11 ? Type::Array : Type::Vector;
                    t.count = ops.size() > 0 ? ops[0] : 0;
                    pending.push_back({&t, {ops.size() > 1 ? ops[1] : 0}});
                    break;
                case 18: case 20:
                    t.kind = Type::Struct;
                    if (item.code == 20) t.name = name;
                    pending.push_back({&t, std::vector<std::uint64_t>(ops.size() > 1 ? ops.begin() + 1 : ops.end(), ops.end())});
                    name.clear();
                    break;
                case 6: t.kind = Type::Struct; t.name = name; name.clear(); break;
                case 21:
                    t.kind = Type::Function;
                    pending.push_back({&t, std::vector<std::uint64_t>(ops.size() > 1 ? ops.begin() + 1 : ops.end(), ops.end())});
                    break;
                default: t.kind = Type::Unknown; break;
            }
            m.types_.push_back(&t);
        }
        for (auto& p : pending) {
            Type& t = *p.t;
            if (t.kind == Type::Pointer) t.pointee = type(p.refs[0]);
            else if (t.kind == Type::Array || t.kind == Type::Vector) t.element = type(p.refs[0]);
            else if (t.kind == Type::Struct) for (auto r : p.refs) t.members.push_back(type(r));
            else if (t.kind == Type::Function && !p.refs.empty()) {
                t.ret = type(p.refs[0]);
                for (std::size_t i = 1; i < p.refs.size(); ++i) t.params.push_back(type(p.refs[i]));
            }
        }
        return error.empty() && !bits.failed;
    }

    void global(const std::vector<std::uint64_t>& ops) {
        if (ops.size() < 3) return;
        const bool explicit_type = ops[1] & 2;
        const Type* value_type = type(ops[0]);
        if (!explicit_type && value_type) value_type = value_type->pointee;
        Type& pointer = m.type_storage_.emplace_back();
        pointer.kind = Type::Pointer;
        pointer.pointee = value_type;
        Value* v = new_value(Value::Global, &pointer);
        v->value_type = value_type;
        v->init_id = ops[2];
        v->index = m.values_.size();
        m.values_.push_back(v);
    }

    void function_decl(const std::vector<std::uint64_t>& ops) {
        if (ops.size() < 3) return;
        const Type* ftype = type(ops[0]);
        if (ftype && ftype->kind == Type::Pointer) ftype = ftype->pointee;
        Function& f = m.function_storage_.emplace_back();
        f.type = ftype;
        f.prototype = ops[2] != 0;
        Type& pointer = m.type_storage_.emplace_back();
        pointer.kind = Type::Pointer;
        pointer.pointee = ftype;
        Value* v = new_value(Value::FunctionRef, &pointer);
        v->function = &f;
        v->index = m.values_.size();
        f.value = v;
        m.functions.push_back(&f);
        m.values_.push_back(v);
        if (!f.prototype) m.bodies_.push_back(&f);
    }

    bool constants(Block& block, std::vector<const Value*>& table) {
        const Type* current = nullptr;
        std::vector<Value*> aggregates;
        Item item;
        while (next_item(bits, info, block, item)) {
            if (item.kind == Item::Enter) { skip(item.length); continue; }
            if (item.kind == Item::End) break;
            const auto& ops = item.ops;
            if (item.code == 1) { current = ops.empty() ? nullptr : type(ops[0]); continue; }
            Value* v = nullptr;
            switch (item.code) {
                case 2: v = new_value(Value::Null, current); break;
                case 3: v = new_value(Value::Undef, current); break;
                case 4: v = new_value(Value::Const, current); v->int_value = ops.empty() ? 0 : sign_rotated(ops[0]); break;
                case 6:
                    v = new_value(Value::Const, current);
                    v->bits = ops.empty() ? 0 : ops[0];
                    if (current && current->kind == Type::Float) v->float_value = float_bits(v->bits);
                    else if (current && current->kind == Type::Double) v->float_value = double_bits(v->bits);
                    else if (current && current->kind == Type::Half) v->float_value = half_bits(v->bits);
                    else { error = "float constant of a non-float type"; return false; }
                    break;
                case 7: v = new_value(Value::Aggregate, current); v->raw_elements = ops; aggregates.push_back(v); break;
                case 8: case 9: v = new_value(Value::Data, current); v->items = ops; break;
                case 22: {
                    v = new_value(Value::Data, current);
                    const Type* element = current ? current->element : nullptr;
                    if (element && element->kind == Type::Float) for (auto x : ops) v->float_items.push_back(float_bits(x));
                    else if (element && element->kind == Type::Double) for (auto x : ops) v->float_items.push_back(double_bits(x));
                    else v->items = ops;
                    break;
                }
                default: v = new_value(Value::Expr, current); v->items = ops; break;
            }
            v->index = table.size();
            table.push_back(v);
        }
        for (Value* v : aggregates)
            for (auto i : v->raw_elements) v->elements.push_back(i < table.size() ? table[i] : nullptr);
        return error.empty() && !bits.failed;
    }

    bool metadata(Block& block) {
        std::string name;
        Item item;
        while (next_item(bits, info, block, item)) {
            if (item.kind == Item::Enter) { skip(item.length); continue; }
            if (item.kind == Item::End) break;
            const auto& ops = item.ops;
            const auto index = m.metadata_count_;
            switch (item.code) {
                case 1: {
                    Module::RawMetadata md;
                    md.kind = 1;
                    for (auto c : ops) md.string += static_cast<char>(c);
                    m.metadata_[index] = md;
                    ++m.metadata_count_;
                    break;
                }
                case 2: {
                    Module::RawMetadata md;
                    md.kind = 2;
                    md.value = ops.size() > 1 ? ops[1] : 0;
                    m.metadata_[index] = md;
                    ++m.metadata_count_;
                    break;
                }
                case 3: case 5: {
                    Module::RawMetadata md;
                    md.kind = 3;
                    for (auto x : ops) md.node.push_back(x ? static_cast<std::int64_t>(x) - 1 : -1);
                    m.metadata_[index] = md;
                    ++m.metadata_count_;
                    break;
                }
                case 4: name.clear(); for (auto c : ops) name += static_cast<char>(c); break;
                case 10: m.named_metadata[name] = ops; break;
                case 6: break;
                default: ++m.metadata_count_; break;
            }
        }
        return !bits.failed;
    }

    bool function(Block& block) {
        if (m.bodies_.empty()) { error = "function body without a declaration"; return false; }
        Function* f = m.bodies_.front();
        m.bodies_.erase(m.bodies_.begin());
        std::vector<const Value*> values = m.values_;
        if (f->type)
            for (std::size_t i = 0; i < f->type->params.size(); ++i) {
                Value* a = new_value(Value::Argument, f->type->params[i]);
                a->index = values.size();
                values.push_back(a);
            }
        std::vector<std::vector<Instruction*>> blocks;
        std::vector<Instruction*> current;
        std::uint64_t declared = 0;
        const bool relative = m.version_ >= 1;
        Type& bool_type = m.type_storage_.emplace_back();
        bool_type.kind = Type::Int;
        bool_type.width = 1;

        const auto forward = [&](std::size_t id, const Type* t) {
            Value* v = new_value(Value::Forward, t);
            v->forward_id = id;
            return static_cast<const Value*>(v);
        };
        const auto rel = [&](std::uint64_t op) -> std::size_t { return relative ? values.size() - op : op; };
        // a value then (for forward references) its type
        const auto value_type_pair = [&](const std::vector<std::uint64_t>& ops, std::size_t& slot) -> const Value* {
            if (slot >= ops.size()) { error = "instruction operands run out"; return nullptr; }
            const std::size_t id = rel(ops[slot]);
            if (id < values.size()) { ++slot; return values[id]; }
            const Type* t = slot + 1 < ops.size() ? type(ops[slot + 1]) : nullptr;
            slot += 2;
            return forward(id, t);
        };
        const auto value = [&](const std::vector<std::uint64_t>& ops, std::size_t slot, const Type* t) -> const Value* {
            if (slot >= ops.size()) { error = "instruction operands run out"; return nullptr; }
            const std::size_t id = rel(ops[slot]);
            return id < values.size() ? values[id] : forward(id, t);
        };
        const auto signed_value = [&](std::uint64_t raw, const Type* t) -> const Value* {
            const std::size_t id = relative ? static_cast<std::size_t>(static_cast<std::int64_t>(values.size()) - sign_rotated(raw))
                                            : static_cast<std::size_t>(raw);
            return id < values.size() ? values[id] : forward(id, t);
        };
        const auto define = [&](Instruction* ins) {
            if (ins->type && ins->type->kind != Type::Void) {
                Value* v = new_value(Value::InstructionResult, ins->type);
                v->instruction = ins;
                v->index = values.size();
                ins->value = v;
                values.push_back(v);
            }
            current.push_back(ins);
        };
        const auto make = [&](Instruction::Op op, const Type* t) {
            Instruction& ins = m.instruction_storage_.emplace_back();
            ins.op = op;
            ins.type = t;
            return &ins;
        };
        const auto finish_block = [&] { blocks.push_back(std::move(current)); current.clear(); };

        Item item;
        while (next_item(bits, info, block, item)) {
            if (item.kind == Item::Enter) {
                if (item.id == 11) { if (!constants(item.sub, values)) return false; }
                else if (item.id == 15) { if (!metadata(item.sub)) return false; }
                else skip(item.length);
                continue;
            }
            if (item.kind == Item::End) break;
            const auto& ops = item.ops;
            std::size_t slot = 0;
            switch (item.code) {
                case 1: declared = ops.empty() ? 0 : ops[0]; break;
                case 33: case 35: break;  // debug locations
                case 2: {  // BINOP [a, b, opcode]
                    const Value* a = value_type_pair(ops, slot);
                    const Value* b = value(ops, slot, a ? a->type : nullptr);
                    Instruction* ins = make(Instruction::Binop, a ? a->type : nullptr);
                    ins->operands = {a, b};
                    ins->opcode = slot + 1 < ops.size() ? static_cast<std::uint32_t>(ops[slot + 1]) : 0;
                    define(ins);
                    break;
                }
                case 3: {  // CAST [a, type, opcode]
                    const Value* a = value_type_pair(ops, slot);
                    Instruction* ins = make(Instruction::Cast, slot < ops.size() ? type(ops[slot]) : nullptr);
                    ins->operands = {a};
                    ins->opcode = slot + 1 < ops.size() ? static_cast<std::uint32_t>(ops[slot + 1]) : 0;
                    define(ins);
                    break;
                }
                case 29: case 5: {  // VSELECT [t, f, cond] / SELECT
                    const Value* t = value_type_pair(ops, slot);
                    const Value* f2 = value(ops, slot, t ? t->type : nullptr);
                    std::size_t cond_slot = slot + 1;
                    const Value* cond = item.code == 29 ? value_type_pair(ops, cond_slot) : value(ops, slot + 1, &bool_type);
                    Instruction* ins = make(Instruction::Select, t ? t->type : nullptr);
                    ins->operands = {cond, t, f2};
                    define(ins);
                    break;
                }
                case 26: {  // EXTRACTVAL [agg, indices...]
                    const Value* agg = value_type_pair(ops, slot);
                    Instruction* ins = make(Instruction::ExtractValue, nullptr);
                    const Type* t = agg ? agg->type : nullptr;
                    for (std::size_t i = slot; i < ops.size() && t; ++i) {
                        ins->indices.push_back(ops[i]);
                        t = t->kind == Type::Struct ? (ops[i] < t->members.size() ? t->members[ops[i]] : nullptr) : t->element;
                    }
                    ins->type = t;
                    ins->operands = {agg};
                    define(ins);
                    break;
                }
                case 27: {  // INSERTVAL
                    const Value* agg = value_type_pair(ops, slot);
                    const Value* val = value_type_pair(ops, slot);
                    Instruction* ins = make(Instruction::InsertValue, agg ? agg->type : nullptr);
                    ins->operands = {agg, val};
                    ins->indices.assign(ops.begin() + std::min(slot, ops.size()), ops.end());
                    define(ins);
                    break;
                }
                case 6: {  // EXTRACTELT
                    const Value* vec = value_type_pair(ops, slot);
                    const Value* idx = value_type_pair(ops, slot);
                    Instruction* ins = make(Instruction::ExtractElement, vec && vec->type ? vec->type->element : nullptr);
                    ins->operands = {vec, idx};
                    define(ins);
                    break;
                }
                case 7: {  // INSERTELT
                    const Value* vec = value_type_pair(ops, slot);
                    const Value* elt = value(ops, slot, vec && vec->type ? vec->type->element : nullptr);
                    std::size_t idx_slot = slot + 1;
                    const Value* idx = value_type_pair(ops, idx_slot);
                    Instruction* ins = make(Instruction::InsertElement, vec ? vec->type : nullptr);
                    ins->operands = {vec, elt, idx};
                    define(ins);
                    break;
                }
                case 9: case 28: {  // CMP / CMP2 [a, b, predicate]
                    const Value* a = value_type_pair(ops, slot);
                    const Value* b = value(ops, slot, a ? a->type : nullptr);
                    const Type* result = &bool_type;
                    if (a && a->type && a->type->kind == Type::Vector) {
                        Type& v = m.type_storage_.emplace_back();
                        v.kind = Type::Vector;
                        v.count = a->type->count;
                        v.element = &bool_type;
                        result = &v;
                    }
                    Instruction* ins = make(Instruction::Cmp, result);
                    ins->operands = {a, b};
                    ins->predicate = slot + 1 < ops.size() ? static_cast<std::uint32_t>(ops[slot + 1]) : 0;
                    define(ins);
                    break;
                }
                case 10: {  // RET
                    Instruction* ins = make(Instruction::Ret, nullptr);
                    if (!ops.empty()) ins->operands = {value_type_pair(ops, slot)};
                    define(ins);
                    finish_block();
                    break;
                }
                case 11: {  // BR [true bb, false bb, cond]
                    Instruction* ins = make(Instruction::Br, nullptr);
                    if (ops.size() == 1) {
                        ins->targets = {static_cast<std::uint32_t>(ops[0])};
                    } else if (ops.size() >= 3) {
                        ins->operands = {value(ops, 2, &bool_type)};
                        ins->targets = {static_cast<std::uint32_t>(ops[0]), static_cast<std::uint32_t>(ops[1])};
                    }
                    define(ins);
                    finish_block();
                    break;
                }
                case 12: {  // SWITCH [opty, cond, default, (value id, bb)...]
                    if (ops.size() < 3) { error = "switch without operands"; return false; }
                    Instruction* ins = make(Instruction::Switch, nullptr);
                    ins->operands = {value(ops, 1, type(ops[0]))};
                    ins->default_target = static_cast<std::uint32_t>(ops[2]);
                    for (std::size_t i = 3; i + 1 < ops.size(); i += 2)
                        ins->cases.emplace_back(ops[i] < values.size() ? values[ops[i]] : nullptr, static_cast<std::uint32_t>(ops[i + 1]));
                    define(ins);
                    finish_block();
                    break;
                }
                case 15: { Instruction* ins = make(Instruction::Unreachable, nullptr); define(ins); finish_block(); break; }
                case 16: {  // PHI [ty, (val, bb)...]
                    if (ops.empty()) { error = "phi without a type"; return false; }
                    const Type* t = type(ops[0]);
                    Instruction* ins = make(Instruction::Phi, t);
                    for (std::size_t i = 1; i + 1 < ops.size(); i += 2) {
                        ins->operands.push_back(signed_value(ops[i], t));
                        ins->blocks.push_back(static_cast<std::uint32_t>(ops[i + 1]));
                    }
                    define(ins);
                    break;
                }
                case 19: {  // ALLOCA [instty, opty, op, align]
                    if (ops.size() < 4) { error = "alloca without operands"; return false; }
                    const Type* t = type(ops[0]);
                    const bool explicit_type = ops[3] & (1 << 6);
                    const Type* result = t;
                    if (explicit_type) {
                        Type& p = m.type_storage_.emplace_back();
                        p.kind = Type::Pointer;
                        p.pointee = t;
                        result = &p;
                    }
                    Instruction* ins = make(Instruction::Alloca, result);
                    ins->allocated = explicit_type ? t : (t ? t->pointee : nullptr);
                    ins->operands = {ops[2] < values.size() ? values[ops[2]] : nullptr};
                    define(ins);
                    break;
                }
                case 20: {  // LOAD [op, ty?, align, vol]
                    const Value* ptr = value_type_pair(ops, slot);
                    const Type* t = slot + 3 == ops.size() ? type(ops[slot]) : (ptr && ptr->type ? ptr->type->pointee : nullptr);
                    Instruction* ins = make(Instruction::Load, t);
                    ins->operands = {ptr};
                    define(ins);
                    break;
                }
                case 44: {  // STORE [ptr, val, align, vol]
                    const Value* ptr = value_type_pair(ops, slot);
                    const Value* val = value_type_pair(ops, slot);
                    Instruction* ins = make(Instruction::Store, nullptr);
                    ins->operands = {ptr, val};
                    define(ins);
                    break;
                }
                case 43: {  // GEP [inbounds, ty, (ptr, idx...) with types]
                    if (ops.size() < 2) { error = "gep without operands"; return false; }
                    Instruction* ins = make(Instruction::Gep, nullptr);
                    ins->source = type(ops[1]);
                    slot = 2;
                    while (slot < ops.size() && error.empty()) ins->operands.push_back(value_type_pair(ops, slot));
                    const Type* t = ins->source;
                    for (std::size_t i = 2; i < ins->operands.size() && t; ++i) {
                        if (t->kind == Type::Struct) {
                            const auto at = static_cast<std::size_t>(ins->operands[i] ? ins->operands[i]->constant() : 0);
                            t = at < t->members.size() ? t->members[at] : nullptr;
                        } else {
                            t = t->element;
                        }
                    }
                    Type& p = m.type_storage_.emplace_back();
                    p.kind = Type::Pointer;
                    p.pointee = t;
                    ins->type = &p;
                    define(ins);
                    break;
                }
                case 34: {  // CALL [attrs, cc, fnty?, callee, args...]
                    if (ops.size() < 3) { error = "call without operands"; return false; }
                    const auto cc = ops[1];
                    slot = 2;
                    const Type* ftype = nullptr;
                    if (cc & (1 << 15)) ftype = type(ops[slot++]);
                    const Value* callee = value_type_pair(ops, slot);
                    if (!ftype && callee && callee->type) ftype = callee->type->pointee;
                    if (!ftype) { error = "call of an unknown function type"; return false; }
                    Instruction* ins = make(Instruction::Call, ftype->ret);
                    ins->callee = callee;
                    for (const Type* p : ftype->params) ins->operands.push_back(value(ops, slot++, p));
                    define(ins);
                    break;
                }
                case 36: { Instruction* ins = make(Instruction::Fence, nullptr); define(ins); break; }
                case 38: {  // ATOMICRMW [ptr, val, op, ...]
                    const Value* ptr = value_type_pair(ops, slot);
                    const Value* val = value(ops, slot, ptr && ptr->type ? ptr->type->pointee : nullptr);
                    Instruction* ins = make(Instruction::AtomicRmw, val ? val->type : nullptr);
                    ins->operands = {ptr, val};
                    ins->opcode = slot + 1 < ops.size() ? static_cast<std::uint32_t>(ops[slot + 1]) : 0;
                    define(ins);
                    break;
                }
                case 46: {  // CMPXCHG
                    const Value* ptr = value_type_pair(ops, slot);
                    const Value* cmp = value_type_pair(ops, slot);
                    const Value* fresh = value(ops, slot, cmp ? cmp->type : nullptr);
                    Type& s = m.type_storage_.emplace_back();
                    s.kind = Type::Struct;
                    s.members = {cmp ? cmp->type : nullptr, &bool_type};
                    Instruction* ins = make(Instruction::CmpXchg, &s);
                    ins->operands = {ptr, cmp, fresh};
                    define(ins);
                    break;
                }
                default:
                    error = "unsupported instruction record " + std::to_string(item.code);
                    return false;
            }
            if (!error.empty()) return false;
        }
        if (!current.empty()) blocks.push_back(std::move(current));
        // resolve forward references
        for (auto& b : blocks)
            for (Instruction* ins : b)
                for (auto& operand : ins->operands)
                    if (operand && operand->kind == Value::Forward) {
                        if (operand->forward_id >= values.size()) { error = "forward reference past the function's values"; return false; }
                        operand = values[operand->forward_id];
                    }
        if (declared && blocks.size() != declared) {
            error = "function has " + std::to_string(blocks.size()) + " blocks, declared " + std::to_string(declared);
            return false;
        }
        f->blocks = std::move(blocks);
        return !bits.failed;
    }

    bool module(Block& block) {
        Item item;
        while (next_item(bits, info, block, item)) {
            if (item.kind == Item::Enter) {
                bool ok = true;
                switch (item.id) {
                    case 0: ok = blockinfo(item.sub); break;
                    case 17: ok = types(item.sub); break;
                    case 11: ok = constants(item.sub, m.values_); break;
                    case 15: ok = metadata(item.sub); break;
                    case 12: ok = function(item.sub); break;
                    default: skip(item.length); break;
                }
                if (!ok) return false;
            } else if (item.kind == Item::Record) {
                if (item.code == 1 && !item.ops.empty()) m.version_ = item.ops[0];
                else if (item.code == 7) global(item.ops);
                else if (item.code == 8) function_decl(item.ops);
            } else {
                break;
            }
        }
        for (const Value* v : m.values_)
            if (v->kind == Value::Global && v->init_id && v->init_id - 1 < m.values_.size())
                const_cast<Value*>(v)->init = m.values_[v->init_id - 1];
        return error.empty() && !bits.failed;
    }
};

bool Module::read(const std::vector<std::uint8_t>& bitcode, std::string& error) {
    if (bitcode.size() < 4 || std::memcmp(bitcode.data(), "BC\xc0\xde", 4) != 0) { error = "not LLVM bitcode"; return false; }
    Bits bits(bitcode, 4);
    ModuleReader reader{*this, bits};
    Block root;
    Item item;
    while (next_item(bits, reader.info, root, item)) {
        if (item.kind != Item::Enter) continue;
        bool ok = true;
        if (item.id == 0) ok = reader.blockinfo(item.sub);
        else if (item.id == 8) ok = reader.module(item.sub);
        else reader.skip(item.length);
        if (!ok) {
            error = reader.error.empty() ? "the bitcode is truncated" : reader.error;
            return false;
        }
    }
    return true;
}

Metadata Module::md(std::uint64_t index, int depth) const {
    Metadata out;
    if (depth > 64) return out;  // distinct nodes can refer back to themselves
    const auto found = metadata_.find(index);
    if (found == metadata_.end()) return out;
    const RawMetadata& raw = found->second;
    if (raw.kind == 1) {
        out.kind = Metadata::String;
        out.string = raw.string;
    } else if (raw.kind == 2) {
        out.kind = Metadata::ValueRef;
        out.value = raw.value < values_.size() ? values_[raw.value] : nullptr;
    } else if (raw.kind == 3) {
        out.kind = Metadata::Node;
        for (auto i : raw.node) out.items.push_back(i < 0 ? Metadata() : md(static_cast<std::uint64_t>(i), depth + 1));
    }
    return out;
}

}  // namespace dtglb::stingray::shader
