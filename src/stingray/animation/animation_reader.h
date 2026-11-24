#pragma once
// Reads an ANIMATION resource (the game's or ours) back and samples it the way the engine's interleaved evaluator
// does (stingray::InterleavedAnimationEvaluator: reset, advance_one). Body: u32 0, u32 0, u32 bone count, f32 length,
// u32 size, u32 beat count, beats {f32 time, f32}, then a stream of u16-tagged items:
//   1 packed start pose: per bone position (3 x u16, -10..10 m), rotation (4 x u16, -pi..pi), scale (3 x u16)
//   7 start pose as floats: per bone position 3, rotation 4 (xyzw), scale 3
//   2 trigger {u32 id, f32 time}, 3 end
//   4 / 5 / 6 position / rotation / scale key {u16 bone, f32 time, 3 or 4 floats}
//   0x8000 / 0xc000 / 0x4000 packed position / rotation / scale key: the first two words hold the tag (2 bits), the
//   bone (10 bits) and the time in ms (20 bits), then 3 x u16 (position, scale) or 4 x u16 (rotation) as above.
// A bone channel keeps its last four controls; a key enters once the third of them has been reached, and the stream
// stops at the first key that can't enter yet (so later keys wait too). Each channel is a Catmull-Rom style curve
// through its controls; rotations keep the sign closest to the previous control and are normalised.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::animation {

struct AnimationControl {
    float time = 0;
    std::array<float, 4> value{};
};
using ControlWindow = std::array<AnimationControl, 4>;

// channel 0 position, 1 rotation, 2 scale
bool sample_control_window(const ControlWindow& window, float time, int channel, std::array<float, 4>& out);

struct GameAnimationKey {
    std::uint16_t bone = 0;
    std::uint8_t channel = 0;
    float time = 0;
    float gate = 0;  // stream time at which the key enters its window
    std::array<float, 4> value{};
};

struct GameAnimation {
    float length = 0;
    bool packed_start = false;
    std::vector<std::array<std::array<float, 4>, 3>> start;  // per bone: position, rotation, scale
    std::vector<GameAnimationKey> keys;
    std::vector<std::array<float, 2>> beats;
    std::vector<std::pair<float, std::uint32_t>> triggers;  // time, id
    std::size_t packed_keys = 0;
    std::uint32_t size = 0;      // the header's size word
    std::size_t body_size = 0, end = 0;  // body bytes, and where the end item finished
};

struct BonePose {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> scale{1, 1, 1};
};

// file: a cooked resource (with the envelope) or a bare body
bool read_game_animation(const std::vector<std::uint8_t>& file, GameAnimation& out, std::string& error);
// local pose of every bone (BONES order) at a time
bool sample_game_animation(const GameAnimation& animation, float time, std::vector<BonePose>& out, std::string& error);

}  // namespace dtglb::stingray::animation
