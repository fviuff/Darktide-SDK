#pragma once
// Reads a STATE_MACHINE resource back the way the engine's loader walks it (root FUN_1405a6860, state FUN_1405a77f0):
// layers (groups) of states, then the root's events, variables, bone rows, constraints and actor sets. Kept here: what
// an editor shows (each state's name, type, clips and loop flag); the rest is walked to the last byte.
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::state_machine {

struct StateMachineState {
    std::uint64_t name = 0;  // IdString64
    std::uint32_t type = 0;  // 0 clips, 1 empty, 2 blend, 4 ragdoll
    std::vector<std::uint64_t> animations;
    bool loop = false;
    std::uint32_t blend_type = 0;  // 1 additive
};

struct StateMachine {
    std::vector<std::vector<StateMachineState>> layers;
    std::vector<std::uint32_t> events, variables;  // IdString32
};

// file: a cooked resource (with the envelope) or a bare body
bool read_state_machine(const std::vector<std::uint8_t>& file, StateMachine& out, std::string& error);

}  // namespace dtglb::stingray::state_machine
