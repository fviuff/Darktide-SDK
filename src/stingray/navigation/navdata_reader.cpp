#include "stingray/navigation/navdata_reader.h"

#include <cstring>

namespace dtglb::stingray::navigation {

namespace {

struct Blob {
    const std::uint8_t* data;
    std::size_t size;
    bool failed = false;

    template <typename T> T at(std::size_t offset) {
        T value{};
        if (offset > size || sizeof(T) > size - offset) { failed = true; return value; }
        std::memcpy(&value, data + offset, sizeof(T));
        return value;
    }
    // an array or child reference at `offset`: {count or size, start}
    std::pair<std::uint32_t, std::size_t> array(std::size_t offset) {
        const auto count = at<std::uint32_t>(offset);
        const auto relative = at<std::int32_t>(offset + 4);
        const std::int64_t start = static_cast<std::int64_t>(offset) + 4 + relative;
        if (start < 0 || static_cast<std::size_t>(start) > size) { failed = true; return {0, 0}; }
        return {count, static_cast<std::size_t>(start)};
    }
    std::string text(std::size_t offset) {
        const auto [count, start] = array(offset);
        if (failed || count > size - start) { failed = true; return {}; }
        std::string s(reinterpret_cast<const char*>(data + start), count);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
};

bool read_mesh(Blob& b, NavDataMesh& mesh) {
    for (int i = 0; i < 4; ++i) mesh.cells_box[i] = b.at<std::int32_t>(4 * i);
    for (int i = 0; i < 6; ++i) mesh.settings[i] = b.at<float>(0x24 + 4 * i);
    mesh.cell_size = b.at<std::int32_t>(0x3c);
    const float pixel = 128.0f * mesh.settings[5];
    const auto [cell_count, cells] = b.array(0x44);
    for (std::uint32_t c = 0; c < cell_count && !b.failed; ++c) {
        const auto [cell_size, cell] = b.array(cells + 8 * c);
        (void)cell_size;
        const auto gx = b.at<std::int32_t>(cell), gy = b.at<std::int32_t>(cell + 4);
        const auto [floor_count, floors] = b.array(cell + 0x10);
        ++mesh.cells;
        for (std::uint32_t f = 0; f < floor_count && !b.failed; ++f) {
            const auto [floor_size, floor] = b.array(floors + 8 * f);
            (void)floor_size;
            ++mesh.floors;
            const auto [edge_count, edges] = b.array(floor + 0x10);
            const auto [vertex_count, vertices] = b.array(floor + 0x18);
            const auto [altitude_count, altitudes] = b.array(floor + 0x20);
            if (vertex_count != altitude_count || edge_count % 3) { b.failed = true; break; }
            for (std::uint32_t e = 0; e < edge_count && !b.failed; ++e) {
                const auto word = b.at<std::uint32_t>(edges + 4 * e);
                const std::uint32_t v = (word >> 14) & 0xfff;
                if (v >= vertex_count) { b.failed = true; break; }
                const float x = (static_cast<float>(gx) * mesh.cell_size + b.at<std::uint8_t>(vertices + 2 * v)) * pixel;
                const float y = (static_cast<float>(gy) * mesh.cell_size + b.at<std::uint8_t>(vertices + 2 * v + 1)) * pixel;
                mesh.vertices.push_back({x, y, b.at<float>(altitudes + 4 * v)});
            }
        }
    }
    return !b.failed;
}

}  // namespace

bool read_navdata(const std::vector<std::uint8_t>& bytes, NavData& out, std::string& error) {
    out = {};
    std::size_t at = 0;
    if (bytes.size() >= 38 && std::memcmp(bytes.data() + 38, "BlobAggFile", 11) == 0) at = 38;  // an extracted resource
    if (bytes.size() < at + 20 || std::memcmp(bytes.data() + at, "BlobAggFile\0", 12) != 0) { error = "not NavData (no BlobAggFile)"; return false; }
    std::uint32_t endian = 0, count = 0;
    std::memcpy(&endian, bytes.data() + at + 12, 4);
    std::memcpy(&count, bytes.data() + at + 16, 4);
    if (endian != 0) { error = "NavData of another byte order"; return false; }
    at += 20;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (bytes.size() - at < 16) { error = "NavData ends early"; return false; }
        std::uint32_t head[4];
        std::memcpy(head, bytes.data() + at, 16);
        at += 16;
        if (head[2] > bytes.size() - at || head[3] > head[2]) { error = "NavData entry sizes run past the end"; return false; }
        Blob b{bytes.data() + at, head[2]};
        switch (head[0]) {
            case 0x500: out.version = b.text(0); break;
            case 0x001: {
                NavDataMesh mesh;
                if (!read_mesh(b, mesh)) { error = "a NavData mesh does not read"; return false; }
                out.meshes.push_back(std::move(mesh));
                break;
            }
            case 0x002: {
                const auto [n, start] = b.array(0x0c);
                for (std::uint32_t v = 0; v < n && !b.failed; ++v)
                    out.graph_points.push_back({b.at<float>(start + 36 * v), b.at<float>(start + 36 * v + 4), b.at<float>(start + 36 * v + 8)});
                break;
            }
            case 0x00e: {
                const auto [n, start] = b.array(0x10);
                for (std::uint32_t s = 0; s < n && !b.failed; ++s)
                    if (b.text(start + 16 * s) == "SectorName") out.sector = b.text(start + 16 * s + 8);
                break;
            }
            default: break;
        }
        if (b.failed) { error = "a NavData entry does not read"; return false; }
        at += head[2];
    }
    return true;
}

}  // namespace dtglb::stingray::navigation
