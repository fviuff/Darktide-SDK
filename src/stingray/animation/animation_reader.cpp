#include "stingray/animation/animation_reader.h"

#include <cmath>
#include <cstring>
#include <limits>

#include "stingray/cooked_resource.h"

namespace dtglb::stingray::animation {

namespace {

struct Reader {
    const std::vector<std::uint8_t>& data;
    std::size_t at = 0;
    bool failed = false;
    template <class T> T get() {
        T v{};
        if (at + sizeof(T) > data.size()) { failed = true; at = data.size(); return v; }
        std::memcpy(&v, data.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
};

float unpack_vector(std::uint16_t v) { return static_cast<float>(v) * 0.0003051851f - 10.0f; }
float unpack_rotation(std::uint16_t v) { return static_cast<float>(v) * 9.587673e-05f - 3.1415927f; }

}  // namespace

bool sample_control_window(const ControlWindow& state, float query, int channel, std::array<float, 4>& out) {
    if (query >= state[3].time || (channel != 2 && query >= state[2].time && state[1].time == 0 && state[2].time == 0)) {
        out = state[3].value;
        return true;
    }
    const ControlWindow w = query >= state[2].time ? ControlWindow{state[1], state[2], state[3], state[3]} : state;
    const double dt = double(w[2].time) - w[1].time;
    const double d0 = double(w[2].time) - w[0].time, d1 = double(w[3].time) - w[1].time;
    if (dt <= 0 || d0 <= 0 || d1 <= 0) return false;
    const double u = (double(query) - w[1].time) / dt, u2 = u * u, u3 = u2 * u;
    std::array<double, 4> result{};
    const int width = channel == 1 ? 4 : 3;
    for (int c = 0; c < width; ++c) {
        const double m1 = (double(w[2].value[c]) - w[0].value[c]) * dt / d0;
        const double m2 = (double(w[3].value[c]) - w[1].value[c]) * dt / d1;
        result[c] = (2 * u3 - 3 * u2 + 1) * w[1].value[c] + (u3 - 2 * u2 + u) * m1 + (-2 * u3 + 3 * u2) * w[2].value[c] +
                    (u3 - u2) * m2;
    }
    if (channel == 1) {
        double norm = 0;
        for (double v : result) norm += v * v;
        if (norm == 0) result = {0, 0, 0, 1};
        else {
            if (!std::isfinite(norm)) return false;
            for (double& v : result) v /= std::sqrt(norm);
        }
    }
    for (int c = 0; c < width; ++c) {
        if (!std::isfinite(result[c]) || std::abs(result[c]) > std::numeric_limits<float>::max()) return false;
        out[c] = static_cast<float>(result[c]);
    }
    return true;
}

bool read_game_animation(const std::vector<std::uint8_t>& file, GameAnimation& out, std::string& error) {
    std::vector<std::uint8_t> body;
    std::string stream, envelope_error;
    if (!parse_cooked_resource_envelope(file, "animation", body, stream, envelope_error)) body = file;
    Reader r{body};
    GameAnimation a;
    r.get<std::uint32_t>();
    r.get<std::uint32_t>();
    const auto bones = r.get<std::uint32_t>();
    a.length = r.get<float>();
    a.size = r.get<std::uint32_t>();
    const auto beats = r.get<std::uint32_t>();
    if (r.failed || bones > 1024 || beats > 4096) { error = "animation header isn't one"; return false; }
    for (std::uint32_t i = 0; i < beats; ++i) {
        const float time = r.get<float>();
        a.beats.push_back({time, r.get<float>()});
    }
    const auto start = r.get<std::uint16_t>();
    a.packed_start = start == 1;
    a.start.resize(bones);
    for (auto& pose : a.start) {
        if (start == 1) {
            for (int c = 0; c < 3; ++c) pose[0][c] = unpack_vector(r.get<std::uint16_t>());
            for (int c = 0; c < 4; ++c) pose[1][c] = unpack_rotation(r.get<std::uint16_t>());
            for (int c = 0; c < 3; ++c) pose[2][c] = unpack_vector(r.get<std::uint16_t>());
        } else if (start == 7) {
            for (int c = 0; c < 3; ++c) pose[0][c] = r.get<float>();
            for (int c = 0; c < 4; ++c) pose[1][c] = r.get<float>();
            for (int c = 0; c < 3; ++c) pose[2][c] = r.get<float>();
        } else {
            error = "animation start pose item " + std::to_string(start);
            return false;
        }
    }
    // the evaluator's windows: the last control time per channel at each slot, for the entry rule
    std::vector<std::array<ControlWindow, 3>> windows(bones);
    for (std::size_t b = 0; b < bones; ++b)
        for (int c = 0; c < 3; ++c)
            for (auto& control : windows[b][c]) control.value = a.start[b][c];
    float gate = 0;
    for (;;) {
        if (r.failed) { error = "animation stream runs past the end"; return false; }
        const auto word = r.get<std::uint16_t>();
        const std::uint16_t tag = (word & 0xc000) ? (word & 0xc000) : word;
        if (tag == 3) { a.end = r.at; break; }
        if (tag == 2) {
            const auto id = r.get<std::uint32_t>();
            a.triggers.push_back({r.get<float>(), id});
            continue;
        }
        GameAnimationKey key;
        if (tag == 4 || tag == 5 || tag == 6) {
            key.bone = r.get<std::uint16_t>();
            key.time = r.get<float>();
            key.channel = tag == 4 ? 0 : tag == 5 ? 1 : 2;
            for (int c = 0; c < (tag == 5 ? 4 : 3); ++c) key.value[c] = r.get<float>();
        } else if (tag == 0x8000 || tag == 0xc000 || tag == 0x4000) {
            const auto low = r.get<std::uint16_t>();
            key.bone = static_cast<std::uint16_t>((word >> 4) & 0x3ff);
            key.time = static_cast<float>(((static_cast<std::uint32_t>(word) << 16) | low) & 0xfffff) * 0.001f;
            key.channel = tag == 0x8000 ? 0 : tag == 0xc000 ? 1 : 2;
            if (key.channel == 1) for (int c = 0; c < 4; ++c) key.value[c] = unpack_rotation(r.get<std::uint16_t>());
            else for (int c = 0; c < 3; ++c) key.value[c] = unpack_vector(r.get<std::uint16_t>());
            ++a.packed_keys;
        } else {
            error = "animation stream item " + std::to_string(word);
            return false;
        }
        if (r.failed) { error = "animation stream runs past the end"; return false; }
        if (key.bone >= bones) { error = "animation key for bone " + std::to_string(key.bone); return false; }
        auto& w = windows[key.bone][key.channel];
        // the key enters once the window's third control is reached; the stream waits for it
        gate = std::max(gate, w[2].time);
        key.gate = gate;
        if (key.channel == 1) {
            double dot = 0;
            for (int c = 0; c < 4; ++c) dot += double(w[3].value[c]) * key.value[c];
            if (dot < 0) for (float& v : key.value) v = -v;
        }
        w = {w[1], w[2], w[3], AnimationControl{key.time, key.value}};
        a.keys.push_back(key);
    }
    a.body_size = body.size();
    out = std::move(a);
    return true;
}

bool sample_game_animation(const GameAnimation& a, float time, std::vector<BonePose>& out, std::string& error) {
    if (!std::isfinite(time) || time < 0) { error = "animation sample time must be finite and not negative"; return false; }
    std::vector<std::array<ControlWindow, 3>> windows(a.start.size());
    for (std::size_t b = 0; b < a.start.size(); ++b)
        for (int c = 0; c < 3; ++c)
            for (auto& control : windows[b][c]) control = AnimationControl{0, a.start[b][c]};
    for (const auto& key : a.keys) {
        if (key.gate > time) break;
        auto& w = windows[key.bone][key.channel];
        w = {w[1], w[2], w[3], AnimationControl{key.time, key.value}};
    }
    out.assign(a.start.size(), BonePose{});
    for (std::size_t b = 0; b < out.size(); ++b) {
        std::array<float, 4> p{}, q{}, s{};
        if (!sample_control_window(windows[b][0], time, 0, p) || !sample_control_window(windows[b][1], time, 1, q) ||
            !sample_control_window(windows[b][2], time, 2, s)) {
            error = "animation curve gives a non-finite value or an empty interval";
            return false;
        }
        out[b].position = {p[0], p[1], p[2]};
        out[b].rotation = q;
        out[b].scale = {s[0], s[1], s[2]};
    }
    return true;
}

}  // namespace dtglb::stingray::animation
