#include "stingray/material/dxil_constants.h"

#include "stingray/material/shader_section.h"
#include "stingray/murmur_hash.h"
#include "stingray/texture/texture_writer.h"

#include <cmath>
#include <cstring>
#include <functional>

namespace dtglb::stingray::material {
namespace {

// MD5 with DXIL's own padding (DxilHash ComputeHashRetail)
void md5_rounds(std::array<std::uint32_t, 4>& state, const std::uint8_t* block) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr std::array<std::uint32_t, 16> shifts = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    std::array<std::uint32_t, 16> x;
    std::memcpy(x.data(), block, 64);
    auto [a, b, c, d] = state;
    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        f += a + k[i] + x[g];
        const auto s = shifts[(i / 16) * 4 + i % 4];
        a = d; d = c; c = b;
        b += (f << s) | (f >> (32 - s));
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

struct Bits {
    std::uint8_t* data;
    std::size_t end;   // in bits
    std::size_t pos = 0;
    bool ok = true;

    std::uint64_t read(unsigned width) {
        std::uint64_t value = 0;
        if (pos + width > end) { ok = false; pos = end; return 0; }
        for (unsigned i = 0; i < width; ++i, ++pos)
            value |= static_cast<std::uint64_t>((data[pos >> 3] >> (pos & 7)) & 1u) << i;
        return value;
    }
    std::uint64_t vbr(unsigned width, unsigned* chunks = nullptr) {
        std::uint64_t value = 0;
        unsigned shift = 0, count = 0;
        const std::uint64_t top = 1ull << (width - 1);
        while (ok) {
            const auto piece = read(width);
            ++count;
            if (shift < 64) value |= (piece & (top - 1)) << shift;
            if (!(piece & top)) break;
            shift += width - 1;
            if (count > 16) { ok = false; break; }
        }
        if (chunks) *chunks = count;
        return value;
    }
    void align32() { pos = (pos + 31) & ~std::size_t(31); if (pos > end) { ok = false; pos = end; } }
    void write(std::size_t at, std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            const auto bit = at + i;
            if ((value >> i) & 1u) data[bit >> 3] |= static_cast<std::uint8_t>(1u << (bit & 7));
            else data[bit >> 3] &= static_cast<std::uint8_t>(~(1u << (bit & 7)));
        }
    }
};

struct AbbrevOp {
    enum Kind { Literal, Fixed, Vbr, Array, Char6, Blob } kind;
    std::uint64_t value = 0;
};
using Abbrev = std::vector<AbbrevOp>;

struct Operand {
    std::uint64_t value = 0;
    std::size_t pos = 0;
    unsigned width = 0;    // fixed width, or VBR chunk width
    unsigned chunks = 0;   // VBR chunks; 0 for a fixed field
    bool placed = false;   // false for literals (nothing stored)
};

Abbrev read_abbrev(Bits& bits) {
    Abbrev ops;
    const auto count = bits.vbr(5);
    for (std::uint64_t i = 0; i < count && bits.ok; ++i) {
        if (bits.read(1)) { ops.push_back({AbbrevOp::Literal, bits.vbr(8)}); continue; }
        switch (bits.read(3)) {
        case 1: ops.push_back({AbbrevOp::Fixed, bits.vbr(5)}); break;
        case 2: ops.push_back({AbbrevOp::Vbr, bits.vbr(5)}); break;
        case 3: ops.push_back({AbbrevOp::Array}); break;
        case 4: ops.push_back({AbbrevOp::Char6}); break;
        case 5: ops.push_back({AbbrevOp::Blob}); break;
        default: bits.ok = false;
        }
    }
    return ops;
}

Operand read_scalar(Bits& bits, const AbbrevOp& op) {
    Operand out;
    switch (op.kind) {
    case AbbrevOp::Literal: out.value = op.value; break;
    case AbbrevOp::Fixed: out.pos = bits.pos; out.width = static_cast<unsigned>(op.value); out.value = bits.read(out.width); out.placed = true; break;
    case AbbrevOp::Vbr: out.pos = bits.pos; out.width = static_cast<unsigned>(op.value); out.value = bits.vbr(out.width, &out.chunks); out.placed = true; break;
    case AbbrevOp::Char6: out.value = bits.read(6); break;
    default: bits.ok = false;
    }
    return out;
}

std::vector<Operand> read_abbreviated(Bits& bits, const Abbrev& abbrev) {
    std::vector<Operand> values;
    for (std::size_t i = 0; i < abbrev.size() && bits.ok; ++i) {
        const auto& op = abbrev[i];
        if (op.kind == AbbrevOp::Array) {
            if (i + 1 >= abbrev.size()) { bits.ok = false; break; }
            const auto count = bits.vbr(6);
            for (std::uint64_t n = 0; n < count && bits.ok; ++n) values.push_back(read_scalar(bits, abbrev[i + 1]));
            ++i;
        } else if (op.kind == AbbrevOp::Blob) {
            const auto size = bits.vbr(6);
            bits.align32();
            bits.pos += static_cast<std::size_t>(size) * 8;
            if (bits.pos > bits.end) bits.ok = false;
            bits.align32();
            values.resize(values.size() + static_cast<std::size_t>(size));
        } else {
            values.push_back(read_scalar(bits, op));
        }
    }
    return values;
}

constexpr std::uint64_t kBlockInfo = 0, kModule = 8, kConstants = 11, kFunction = 12, kTypes = 17;
enum class FloatKind : std::uint8_t { None, Half, Float, Double };

struct Walker {
    Bits& bits;
    std::map<std::uint64_t, std::vector<Abbrev>> blockinfo;
    std::vector<FloatKind> types;
    std::function<void(FloatKind, const Operand&)> on_float;

    void block(std::uint64_t id, unsigned width) {
        std::vector<Abbrev> abbrevs = blockinfo[id];
        std::uint64_t info_target = 0;
        FloatKind current = FloatKind::None;
        while (bits.ok && bits.pos < bits.end) {
            const auto code = bits.read(width);
            if (code == 0) { bits.align32(); return; }
            if (code == 1) {
                const auto sub = bits.vbr(8);
                const auto sub_width = static_cast<unsigned>(bits.vbr(4));
                bits.align32();
                const auto words = bits.read(32);
                if (sub == kBlockInfo || sub == kModule || sub == kConstants || sub == kFunction || sub == kTypes) {
                    block(sub, sub_width);
                } else {
                    bits.pos += static_cast<std::size_t>(words) * 32;
                    if (bits.pos > bits.end) bits.ok = false;
                }
                continue;
            }
            if (code == 2) {
                auto abbrev = read_abbrev(bits);
                if (id == kBlockInfo) blockinfo[info_target].push_back(std::move(abbrev));
                else abbrevs.push_back(std::move(abbrev));
                continue;
            }
            std::vector<Operand> record;
            if (code == 3) {
                Operand head;
                head.value = bits.vbr(6);
                record.push_back(head);
                const auto count = bits.vbr(6);
                for (std::uint64_t i = 0; i < count && bits.ok; ++i) {
                    Operand op;
                    op.pos = bits.pos; op.width = 6; op.placed = true;
                    op.value = bits.vbr(6, &op.chunks);
                    record.push_back(op);
                }
            } else {
                if (code - 4 >= abbrevs.size()) { bits.ok = false; return; }
                record = read_abbreviated(bits, abbrevs[static_cast<std::size_t>(code - 4)]);
            }
            if (record.empty() || !bits.ok) continue;
            const auto record_code = record[0].value;
            if (id == kBlockInfo && record_code == 1 && record.size() > 1) {
                info_target = record[1].value;
            } else if (id == kTypes && record_code != 1 && record_code != 19) {   // NUMENTRY, STRUCT_NAME
                types.push_back(record_code == 3 ? FloatKind::Float : record_code == 4 ? FloatKind::Double :
                                record_code == 10 ? FloatKind::Half : FloatKind::None);
            } else if (id == kConstants && record_code == 1 && record.size() > 1) {      // SETTYPE
                current = record[1].value < types.size() ? types[static_cast<std::size_t>(record[1].value)] : FloatKind::None;
            } else if (id == kConstants && record_code == 6 && record.size() > 1) {      // FLOAT
                on_float(current, record[1]);
            }
        }
    }
};

bool patch_program(std::uint8_t* bitcode, std::size_t size, const std::map<std::uint32_t, std::uint32_t>& edits,
                   std::set<std::uint32_t>& matched, bool& changed, std::string& error) {
    if (size < 8 || std::memcmp(bitcode, "BC\xc0\xde", 4) != 0) { error = "not LLVM bitcode"; return false; }
    Bits bits{bitcode + 4, (size - 4) * 8};
    Walker walker{bits, {}, {}, {}};
    walker.on_float = [&](FloatKind kind, const Operand& op) {
        if (kind != FloatKind::Float || !op.placed) return;
        const auto found = edits.find(static_cast<std::uint32_t>(op.value));
        if (found == edits.end()) return;
        const std::uint64_t wanted = found->second;
        if (op.chunks == 0) {
            if (op.width < 32 && wanted >> op.width) { error = "a shader constant is stored too narrow for the new value"; bits.ok = false; return; }
            bits.write(op.pos, wanted, op.width);
        } else {
            const unsigned payload = op.width - 1;
            if (payload * op.chunks < 64 && wanted >> (payload * op.chunks)) {
                error = "the new value of shader constant " + std::to_string(found->first) +
                        " takes more bits than the old one is stored in";
                bits.ok = false;
                return;
            }
            for (unsigned c = 0; c < op.chunks; ++c) {
                std::uint64_t piece = (wanted >> (payload * c)) & ((1ull << payload) - 1);
                if (c + 1 < op.chunks) piece |= 1ull << payload;
                bits.write(op.pos + static_cast<std::size_t>(c) * op.width, piece, op.width);
            }
        }
        matched.insert(found->first);
        changed = true;
    };
    walker.block(~0ull, 2);
    if (!bits.ok && error.empty()) error = "the shader's bitcode could not be read";
    return error.empty();
}

std::uint32_t u32_at(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint32_t v; std::memcpy(&v, bytes.data() + at, 4); return v;
}

}  // namespace

std::array<std::uint8_t, 16> dxil_container_hash(const std::uint8_t* data, std::size_t size) {
    const std::uint8_t* body = data + 20;
    const std::size_t length = size - 20;
    const std::size_t left = length & 63, full = length - left;
    std::array<std::uint32_t, 4> state = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    for (std::size_t at = 0; at < full; at += 64) md5_rounds(state, body + at);
    const std::uint32_t bit_length = static_cast<std::uint32_t>(length << 3);
    const std::uint32_t last = static_cast<std::uint32_t>(1u | (length << 1));
    std::array<std::uint8_t, 64> block{};
    if (left < 56) {
        std::memcpy(block.data(), &bit_length, 4);
        std::memcpy(block.data() + 4, body + full, left);
        block[4 + left] = 0x80;
        std::memcpy(block.data() + 60, &last, 4);
        md5_rounds(state, block.data());
    } else {
        std::memcpy(block.data(), body + full, left);
        block[left] = 0x80;
        md5_rounds(state, block.data());
        block.fill(0);
        std::memcpy(block.data(), &bit_length, 4);
        std::memcpy(block.data() + 60, &last, 4);
        md5_rounds(state, block.data());
    }
    std::array<std::uint8_t, 16> out;
    std::memcpy(out.data(), state.data(), 16);
    return out;
}

bool patch_dxil_float_constants(std::vector<std::uint8_t>& container, const std::map<std::uint32_t, std::uint32_t>& edits,
                                std::set<std::uint32_t>& matched, std::string& error) {
    if (container.size() < 32 || std::memcmp(container.data(), "DXBC", 4) != 0) { error = "not a DXBC container"; return false; }
    const auto parts = u32_at(container, 28);
    if (32 + static_cast<std::size_t>(parts) * 4 > container.size()) { error = "truncated DXBC container"; return false; }
    bool changed = false;
    for (std::uint32_t i = 0; i < parts; ++i) {
        const std::size_t offset = u32_at(container, 32 + i * 4);
        if (offset + 8 > container.size()) { error = "truncated DXBC part"; return false; }
        const bool program = std::memcmp(container.data() + offset, "DXIL", 4) == 0 ||
                             std::memcmp(container.data() + offset, "STAT", 4) == 0;
        const std::size_t part = offset + 8, part_size = u32_at(container, offset + 4);
        if (!program) continue;
        if (part + 24 > container.size() || part + part_size > container.size() ||
            std::memcmp(container.data() + part + 8, "DXIL", 4) != 0) { error = "unexpected DXIL program header"; return false; }
        const std::size_t start = part + 8 + u32_at(container, part + 16), size = u32_at(container, part + 20);
        if (start + size > part + part_size) { error = "DXIL bitcode outside its part"; return false; }
        if (!patch_program(container.data() + start, size, edits, matched, changed, error)) return false;
    }
    if (changed) {
        const auto digest = dxil_container_hash(container.data(), container.size());
        std::memcpy(container.data() + 4, digest.data(), 16);
    }
    return true;
}

bool patch_stream_float_constants(std::vector<std::uint8_t>& stream, const std::map<std::uint32_t, std::uint32_t>& edits,
                                  std::set<std::uint32_t>& matched, std::string& error) {
    if (edits.empty() || stream.size() < 28) return true;
    const std::uint32_t offset = u32_at(stream, 12), size = u32_at(stream, 16);
    if (!size || offset == 0xffffffffu || offset + static_cast<std::uint64_t>(size) > stream.size()) return true;
    const std::vector<std::uint8_t> bytes(stream.begin() + offset, stream.begin() + offset + size);
    ShaderSection section;
    if (!decode_shader_section(bytes, section, error)) return false;
    bool changed = false;
    for (auto& device : section.device_data)
        for (auto& group : device.groups)
            for (auto& pass : group)
                for (auto& stage : pass.stages)
                    for (auto& variant : stage) {
                        std::vector<std::uint8_t> code(variant.size);
                        if (!texture::oodle_decompress(variant.bytecode, code, error)) return false;
                        std::set<std::uint32_t> found;
                        if (!patch_dxil_float_constants(code, edits, found, error)) return false;
                        if (found.empty()) continue;
                        matched.insert(found.begin(), found.end());
                        std::vector<std::uint8_t> packed;
                        if (!texture::oodle_compress(code, packed, error)) return false;
                        variant.bytecode = std::move(packed);
                        variant.hash = murmur64(variant.bytecode.data(), variant.bytecode.size());
                        changed = true;
                    }
    if (!changed) return true;
    const auto encoded = encode_shader_section(section);
    const std::size_t pad = (4 - (offset + encoded.size()) % 4) % 4;
    std::vector<std::uint8_t> out(stream.begin(), stream.begin() + offset);
    out.insert(out.end(), encoded.begin(), encoded.end());
    out.insert(out.end(), pad, 0);
    out.insert(out.end(), stream.begin() + offset + size, stream.end());
    const auto shift = static_cast<std::int64_t>(encoded.size() + pad) - static_cast<std::int64_t>(size);
    const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(out.data() + at, &value, 4); };
    put(16, static_cast<std::uint32_t>(encoded.size()));
    for (const std::size_t at : {std::size_t(4), std::size_t(20)}) {
        const auto block = u32_at(stream, at);
        if (block != 0xffffffffu && block >= offset + size) put(at, static_cast<std::uint32_t>(block + shift));
    }
    stream = std::move(out);
    return true;
}

}
