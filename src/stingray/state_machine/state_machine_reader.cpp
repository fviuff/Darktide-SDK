#include "stingray/state_machine/state_machine_reader.h"

#include <cstring>

#include "stingray/cooked_resource.h"

namespace dtglb::stingray::state_machine {

namespace {

struct Reader {
    const std::vector<std::uint8_t>& data;
    std::size_t at = 0;
    bool failed = false;
    template <class T> T get() {
        T v{};
        if (failed || at + sizeof(T) > data.size()) { failed = true; return v; }
        std::memcpy(&v, data.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::uint32_t count() {
        const auto n = u32();
        if (n > (1u << 24)) failed = true;
        return failed ? 0 : n;
    }
    void skip(std::size_t n) {
        if (failed || at + n > data.size()) { failed = true; return; }
        at += n;
    }
    std::vector<std::uint32_t> u32s() {
        std::vector<std::uint32_t> out(count());
        for (auto& v : out) v = u32();
        return out;
    }
};

StateMachineState read_state(Reader& r) {
    StateMachineState s;
    s.name = r.get<std::uint64_t>();
    r.skip(16);  // two more ids
    s.type = r.u32();
    s.animations.resize(r.count());
    for (auto& a : s.animations) a = r.get<std::uint64_t>();
    r.skip(static_cast<std::size_t>(r.count()) * 4);  // probabilities
    r.u32();                                          // randomization type
    s.loop = r.get<std::uint8_t>() != 0;
    s.blend_type = r.u32();
    r.skip(static_cast<std::size_t>(r.count()) * 9);   // events {id, target, flag}
    r.skip(static_cast<std::size_t>(r.count()) * 28);  // transitions
    for (auto selectors = r.count(), i = 0u; i < selectors && !r.failed; ++i) {
        r.u32();
        r.skip(static_cast<std::size_t>(r.count()) * 14);  // cases
    }
    r.skip(8);
    r.skip(static_cast<std::size_t>(r.count()) * 12);  // markers
    r.skip(8);                                         // exit event, time
    r.u32s();                                          // expression bytecode
    r.u32s();                                          // expression starts
    r.skip(16);
    r.u32s();  // constraints
    r.skip(12);
    return s;
}

}  // namespace

bool read_state_machine(const std::vector<std::uint8_t>& file, StateMachine& out, std::string& error) {
    std::vector<std::uint8_t> body;
    std::string stream, envelope_error;
    if (!parse_cooked_resource_envelope(file, "state_machine", body, stream, envelope_error)) body = file;
    Reader r{body};
    StateMachine sm;
    sm.layers.resize(r.count());
    for (auto& layer : sm.layers) {
        layer.resize(r.count());
        for (auto& state : layer) {
            if (r.failed) break;
            state = read_state(r);
        }
        r.u32();
    }
    sm.events = r.u32s();
    sm.variables = r.u32s();
    r.u32s();                                         // variable defaults
    r.skip(static_cast<std::size_t>(r.count()) * 8);  // variable bounds
    for (auto rows = r.count(), i = 0u; i < rows && !r.failed; ++i) {
        r.u32s();
        r.u32s();
        r.skip(1);
    }
    r.u32s();
    r.skip(static_cast<std::size_t>(r.count()) * 12);
    r.skip(r.count());  // constraint blob
    for (auto sets = r.count(), i = 0u; i < sets && !r.failed; ++i) {
        r.u32s();
        r.u32s();
    }
    r.skip(static_cast<std::size_t>(r.count()) * 28);  // base matches {3 x u64, u32}
    r.u32();
    if (r.failed) { error = "state machine ends early (" + std::to_string(body.size()) + " bytes)"; return false; }
    if (r.at != body.size()) {
        error = "state machine has " + std::to_string(body.size() - r.at) + " bytes left after the last section";
        return false;
    }
    out = std::move(sm);
    return true;
}

}  // namespace dtglb::stingray::state_machine
