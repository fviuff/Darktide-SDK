#include "stingray/volume_record.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace dtglb::stingray {

namespace {

template <typename T>
void put(std::vector<std::uint8_t>& out, T value) {
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

template <typename T>
T get(const std::vector<std::uint8_t>& in, std::size_t at) {
    T value{};
    if (at + sizeof(T) <= in.size()) std::memcpy(&value, in.data() + at, sizeof(T));
    return value;
}

double cross(const std::array<double, 2>& o, const std::array<double, 2>& p, const std::array<double, 2>& q) {
    return (p[0] - o[0]) * (q[1] - o[1]) - (q[0] - o[0]) * (p[1] - o[1]);
}

}  // namespace

bool make_volume_record(VolumeShape shape, bool box_shape, std::vector<std::uint8_t>& record, std::string& error) {
    auto& outline = shape.outline;
    if (!(shape.top - shape.bottom > 1e-4)) { error = "the volume needs a height"; return false; }
    for (std::size_t i = 0; i < outline.size() && outline.size() > 3;) {  // points on a straight edge add nothing
        const auto n = outline.size();
        if (std::abs(cross(outline[(i + n - 1) % n], outline[i], outline[(i + 1) % n])) < 1e-10) outline.erase(outline.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    if (outline.size() < 3) { error = "the volume's outline needs at least 3 points"; return false; }
    double twice_area = 0.0;
    for (std::size_t i = 0; i < outline.size(); ++i)
        twice_area += outline[i][0] * outline[(i + 1) % outline.size()][1] - outline[(i + 1) % outline.size()][0] * outline[i][1];
    if (twice_area < 0.0) std::reverse(outline.begin(), outline.end());
    // ear clipping: n - 2 triangles over the outline's own points, like retail's: from the first point, each ear
    // {previous, point, next} cut off, then on with the point before it; a point that is no ear passes to the next
    std::vector<std::array<std::uint32_t, 3>> floor;
    std::vector<std::uint32_t> left(outline.size());
    std::iota(left.begin(), left.end(), 0u);
    std::size_t i = 0, tried = 0;
    while (left.size() > 3) {
        const auto p = left[(i + left.size() - 1) % left.size()], q = left[i], r = left[(i + 1) % left.size()];
        bool ear = cross(outline[p], outline[q], outline[r]) > 1e-12;
        for (const auto o : left) {
            if (!ear) break;
            if (o == p || o == q || o == r) continue;
            if (cross(outline[p], outline[q], outline[o]) >= 0 && cross(outline[q], outline[r], outline[o]) >= 0 &&
                cross(outline[r], outline[p], outline[o]) >= 0)
                ear = false;
        }
        if (ear) {
            floor.push_back({p, q, r});
            left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
            i = (i + left.size() - 1) % left.size();
            tried = 0;
        } else {
            if (++tried > left.size()) { error = "the volume's outline crosses itself"; return false; }
            i = (i + 1) % left.size();
        }
    }
    floor.push_back({left[(i + 2) % 3], left[i % 3], left[(i + 1) % 3]});  // the last one from where the cutting stopped
    const auto count = static_cast<std::uint32_t>(outline.size());
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), static_cast<float>(shape.bottom)};
    float hi[3] = {-lo[0], -lo[1], static_cast<float>(shape.top)};
    for (const auto& p : outline)
        for (int axis = 0; axis < 2; ++axis) {
            lo[axis] = std::min(lo[axis], static_cast<float>(p[axis]));
            hi[axis] = std::max(hi[axis], static_cast<float>(p[axis]));
        }
    const bool box = box_shape && count == 4 && std::all_of(outline.begin(), outline.end(), [&](const std::array<double, 2>& p) {
        return (std::abs(p[0] - lo[0]) < 1e-5 || std::abs(p[0] - hi[0]) < 1e-5) &&
               (std::abs(p[1] - lo[1]) < 1e-5 || std::abs(p[1] - hi[1]) < 1e-5);
    });
    std::vector<float> areas;
    float total = 0.0f;
    // in single precision, as the engine's compiler does (the stored areas match bit for bit)
    for (const auto& t : floor) {
        const float ox = static_cast<float>(outline[t[0]][0]), oy = static_cast<float>(outline[t[0]][1]);
        const float ax = static_cast<float>(outline[t[1]][0]) - ox, ay = static_cast<float>(outline[t[1]][1]) - oy;
        const float bx = static_cast<float>(outline[t[2]][0]) - ox, by = static_cast<float>(outline[t[2]][1]) - oy;
        const float first = ax * by, second = bx * ay;
        areas.push_back((first - second) * 0.5f);
        total += areas.back();
    }
    const std::uint32_t triangle_offset = 0x40 + 8 * count;
    const std::uint32_t shape_offset = box ? triangle_offset + 16 * static_cast<std::uint32_t>(floor.size()) : 0;
    const std::uint32_t size = triangle_offset + 16 * static_cast<std::uint32_t>(floor.size()) + (box ? 8 : 0);
    record.clear();
    put(record, shape.frame);
    for (float v : lo) put(record, v);
    for (float v : hi) put(record, v);
    put(record, shape.name);
    put(record, total);
    put(record, static_cast<std::uint32_t>(floor.size()));
    put(record, triangle_offset);
    put(record, shape_offset);
    put<std::uint64_t>(record, 0);  // the engine's PhysX shape, set at runtime
    put(record, count);
    put(record, size);
    for (const auto& p : outline) { put(record, static_cast<float>(p[0])); put(record, static_cast<float>(p[1])); }
    for (std::size_t t = 0; t < floor.size(); ++t) {
        put(record, areas[t]);
        for (const auto i : floor[t]) put(record, i);
    }
    if (box) { put<std::uint32_t>(record, 1); put<std::uint32_t>(record, 0); }
    return true;
}

bool read_volume_record(const std::vector<std::uint8_t>& record, VolumeShape& shape, std::string& error) {
    if (record.size() < 0x40) { error = "volume record is too short"; return false; }
    const auto points = get<std::uint32_t>(record, 0x38);
    if (0x40 + 8ull * points > record.size()) { error = "volume outline runs past its record"; return false; }
    shape = {};
    shape.frame = get<std::uint32_t>(record, 0);
    shape.name = get<std::uint32_t>(record, 0x1c);
    shape.bottom = get<float>(record, 0x0c);
    shape.top = get<float>(record, 0x18);
    for (std::uint32_t k = 0; k < points; ++k)
        shape.outline.push_back({get<float>(record, 0x40 + 8 * k), get<float>(record, 0x44 + 8 * k)});
    return true;
}

}  // namespace dtglb::stingray
