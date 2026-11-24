#include "stingray/navigation/gwnav_generator.h"

#include "stingray/murmur_hash.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <malloc.h>
#endif

namespace dtglb::stingray::navigation {
namespace fs = std::filesystem;

std::string sector_guid(const std::string& name) {
    const std::uint64_t a = id64(name), b = id64(name + "#sector");
    char text[40];
    std::snprintf(text, sizeof text, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xffff), static_cast<unsigned>(a & 0xffff),
                  static_cast<unsigned>(b >> 48), static_cast<unsigned long long>(b & 0xffffffffffffull));
    return text;
}

#ifndef _WIN32
bool generate_navdata(const fs::path&, const NavInput&, const NavSettings&, const std::string&, const std::string&,
                      NavOutput&, std::string& error) {
    error = "navigation generation needs the game's gwnav plugin (Windows only)";
    return false;
}
#else
namespace {

// ---- the engine services (host API selectors) the plugin uses ----------------------------------------------------
// Selector numbers and slot layouts are the game executable's (its plugin host resolver): 4 allocator, 6 logging,
// 0x18 profiler, 0x20 thread (critical sections at slots 13-17), 0x23 crash handlers, 0x26 jobs. Every other slot of
// every table the plugin asks for answers with a stub that records the call, so an unexpected use is reported.

std::mutex g_log_mutex;
std::string g_log;
void log_line(const char* level, const char* system, const char* text) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log += std::string(level) + (system && *system ? std::string(" ") + system : std::string()) + ": " + (text ? text : "") + "\n";
}
void log_info(const char* s, const char* m) { log_line("info", s, m); }
void log_warning(const char* s, const char* m) { log_line("warning", s, m); }
void log_error(const char* s, const char* m) { log_line("error", s, m); }

std::uint64_t g_stub_id;  // written by each stub before it jumps to stub_called
std::uint64_t stub_called() {
    char text[96];
    std::snprintf(text, sizeof text, "unexpected engine call: selector 0x%x slot %u", static_cast<unsigned>(g_stub_id >> 16),
                  static_cast<unsigned>(g_stub_id & 0xffff));
    if ((g_stub_id >> 16) != 1) log_line("host", "", text);  // selector 1 (Lua registration at setup) is expected
    return 0;
}

void* make_stub(std::uint64_t id) {
    static std::uint8_t* page = nullptr;
    static std::size_t used = 0;
    constexpr std::size_t page_size = 1 << 16, stub_size = 32;
    if (!page || used + stub_size > page_size) {
        page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        used = 0;
        if (!page) return nullptr;
    }
    std::uint8_t* p = page + used;
    used += stub_size;
    std::uint8_t* q = p;
    const std::uint64_t where = reinterpret_cast<std::uint64_t>(&g_stub_id), target = reinterpret_cast<std::uint64_t>(&stub_called);
    *q++ = 0x48; *q++ = 0xb8; std::memcpy(q, &id, 8); q += 8;        // mov rax, id
    *q++ = 0x48; *q++ = 0xa3; std::memcpy(q, &where, 8); q += 8;     // mov [g_stub_id], rax
    *q++ = 0x48; *q++ = 0xb8; std::memcpy(q, &target, 8); q += 8;    // mov rax, stub_called
    *q++ = 0xff; *q++ = 0xe0;                                         // jmp rax
    FlushInstructionCache(GetCurrentProcess(), p, stub_size);
    return p;
}

// Allocators. Two kinds: heap allocators, and allocators over a buffer the plugin supplies (slot 13), which must
// place blocks inside that buffer (the plugin finds a block's buffer by address) and return null when it is full.
struct Allocator {
    std::string name;
    std::uint8_t* buffer = nullptr;
    std::map<std::size_t, std::size_t> free_ranges;                  // offset -> length
    std::map<void*, std::pair<std::size_t, std::size_t>> used;       // block -> (range offset, range length)
};
struct HeapBlock { Allocator* owner; std::size_t size; };
std::recursive_mutex g_heap_mutex;
std::map<void*, HeapBlock> g_heap_blocks;

unsigned block_alignment(unsigned align) { unsigned a = 16; while (a < align) a <<= 1; return a; }

void* buffer_allocate(Allocator* a, std::size_t size, unsigned align) {
    size = std::max<std::size_t>(16, (size + 15) & ~std::size_t(15));
    align = block_alignment(align);
    for (auto it = a->free_ranges.begin(); it != a->free_ranges.end(); ++it) {
        const std::size_t off = it->first, len = it->second;
        const std::size_t base = reinterpret_cast<std::size_t>(a->buffer + off);
        const std::size_t start = (base + align - 1) & ~std::size_t(align - 1), pad = start - base;
        if (pad + size > len) continue;
        a->free_ranges.erase(it);
        if (pad) a->free_ranges[off] = pad;
        if (len > pad + size) a->free_ranges[off + pad + size] = len - pad - size;
        void* p = reinterpret_cast<void*>(start);
        a->used[p] = {off + pad, size};
        return p;
    }
    return nullptr;
}
std::size_t buffer_free(Allocator* a, void* p) {
    const auto it = a->used.find(p);
    if (it == a->used.end()) return 0;
    std::size_t off = it->second.first, len = it->second.second;
    const std::size_t freed = len;
    a->used.erase(it);
    auto next = a->free_ranges.lower_bound(off);
    if (next != a->free_ranges.end() && next->first == off + len) { len += next->second; next = a->free_ranges.erase(next); }
    if (next != a->free_ranges.begin()) {
        const auto prev = std::prev(next);
        if (prev->first + prev->second == off) { off = prev->first; len += prev->second; a->free_ranges.erase(prev); }
    }
    a->free_ranges[off] = len;
    return freed;
}

void* allocate(Allocator* a, std::size_t size, unsigned align) {
    std::lock_guard<std::recursive_mutex> lock(g_heap_mutex);
    if (a && a->buffer) return buffer_allocate(a, size, align);
    void* p = _aligned_malloc(size ? size : 1, block_alignment(align));
    if (p) g_heap_blocks[p] = {a, size};
    return p;
}
// the allocator the plugin names decides: a block at the start of a buffer has the buffer's own address
std::size_t deallocate(Allocator* a, void* p) {
    if (!p) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_heap_mutex);
    if (a && a->buffer) return buffer_free(a, p);
    const auto it = g_heap_blocks.find(p);
    if (it == g_heap_blocks.end()) return 0;
    const std::size_t n = it->second.size;
    g_heap_blocks.erase(it);
    _aligned_free(p);
    return n;
}
std::size_t allocated_size(Allocator* a, void* p) {
    std::lock_guard<std::recursive_mutex> lock(g_heap_mutex);
    if (a && a->buffer) { const auto u = a->used.find(p); return u == a->used.end() ? 0 : u->second.second; }
    const auto it = g_heap_blocks.find(p);
    return it == g_heap_blocks.end() ? 0 : it->second.size;
}
struct Sized { void* p; std::size_t size; };
void* alloc_make(const char* name) { auto* a = new Allocator; a->name = name ? name : ""; return a; }
void alloc_destroy(Allocator* a) {
    if (!a) return;
    std::lock_guard<std::recursive_mutex> lock(g_heap_mutex);
    for (auto it = g_heap_blocks.begin(); it != g_heap_blocks.end();) {
        if (it->second.owner == a) { _aligned_free(it->first); it = g_heap_blocks.erase(it); }
        else ++it;
    }
    delete a;
}
void* alloc_allocate(Allocator* a, std::size_t size, unsigned align) { return allocate(a, size, align); }
Sized* alloc_allocate_sized(Sized* out, Allocator* a, std::size_t size, unsigned align) {
    out->p = allocate(a, size, align);
    out->size = out->p ? size : 0;
    return out;
}
void* alloc_reallocate(Allocator* a, void* p, std::size_t size, unsigned align) {
    if (!size) { deallocate(a, p); return nullptr; }
    void* q = allocate(a, size, align);
    if (q && p) {
        std::memcpy(q, p, std::min(allocated_size(a, p), size));
        deallocate(a, p);
    }
    return q;
}
Sized* alloc_reallocate_sized(Sized* out, Allocator* a, void* p, std::size_t size, unsigned align) {
    out->p = alloc_reallocate(a, p, size, align);
    out->size = out->p ? size : 0;
    return out;
}
std::size_t alloc_deallocate(Allocator* a, void* p) { return deallocate(a, p); }
std::size_t alloc_allocated_size(Allocator* a, void* p) { return allocated_size(a, p); }
void* alloc_make_over_buffer(const char* name, void* buffer, std::size_t size, int, int) {
    auto* a = static_cast<Allocator*>(alloc_make(name));
    a->buffer = static_cast<std::uint8_t*>(buffer);
    a->free_ranges[0] = size;
    return a;
}
std::uint64_t alloc_none() { return 0; }

void* cs_create(Allocator*) { auto* c = new CRITICAL_SECTION; InitializeCriticalSection(c); return c; }
void cs_destroy(CRITICAL_SECTION* c, Allocator*) { if (c) { DeleteCriticalSection(c); delete c; } }
void cs_enter(CRITICAL_SECTION* c) { EnterCriticalSection(c); }
void cs_leave(CRITICAL_SECTION* c) { LeaveCriticalSection(c); }
int cs_try(CRITICAL_SECTION* c) { return TryEnterCriticalSection(c) ? 1 : 0; }

// Jobs: run(function, item count, input arrays, their sizes in bytes, number of input arrays, user, 1, flag, name)
// -> job; wait(job). The job function takes {u32 count, ?, ?, input arrays, user} and works through `count` items
// (the item i of each input array at i * its size / count). As the engine's job system does, the items are split into
// batches run on worker threads (threads of their own: the plugin keeps a scratch heap per thread and tears a job's
// down when the job is done); run returns when all are done.
struct JobDescription { std::uint32_t count, pad; std::uint64_t unknown; void** inputs; void* user; };
void* job_run(void (*function)(JobDescription*), unsigned count, void** inputs, unsigned* sizes, int input_count, void* user, int, int,
              const char*) {
    if (!count) return reinterpret_cast<void*>(1);
    unsigned threads_wanted = std::thread::hardware_concurrency();
    if (const char* forced = std::getenv("DTGLB_NAV_THREADS")) threads_wanted = static_cast<unsigned>(std::atoi(forced));
    const unsigned workers = std::max(1u, std::min(count, threads_wanted));
    const unsigned per_batch = (count + workers - 1) / workers;
    const auto arrays = static_cast<std::size_t>(std::max(input_count, 0));
    std::vector<std::vector<void*>> starts;
    std::vector<JobDescription> batches;
    for (unsigned first = 0; first < count; first += per_batch) {
        std::vector<void*> at(arrays);
        for (std::size_t i = 0; i < arrays; ++i) at[i] = static_cast<std::uint8_t*>(inputs[i]) + std::size_t(first) * (sizes[i] / count);
        starts.push_back(std::move(at));
        batches.push_back({std::min(per_batch, count - first), 0, 0, nullptr, user});
    }
    for (std::size_t b = 0; b < batches.size(); ++b) batches[b].inputs = starts[b].data();
    std::vector<std::thread> threads;
    for (auto& batch : batches) threads.emplace_back([function, &batch] { function(&batch); });
    for (auto& thread : threads) thread.join();
    return reinterpret_cast<void*>(1);
}
void job_wait(void*) {}

void* no_handler(int) { return nullptr; }
void profile_start(const char*) {}
void profile_stop() {}

std::map<unsigned, std::vector<void*>> g_tables;
void* resolve(unsigned selector) {
    if (selector == 0xc) return nullptr;  // application options: none (the plugin checks)
    auto& t = g_tables[selector];
    if (t.empty()) {
        t.resize(256);
        for (unsigned i = 0; i < t.size(); ++i) t[i] = make_stub((std::uint64_t(selector) << 16) | i);
        auto set = [&](unsigned slot, auto* function) { t[slot] = reinterpret_cast<void*>(function); };
        switch (selector) {
            case 4:
                set(0, &alloc_make); set(1, &alloc_destroy); set(2, &alloc_make); set(3, &alloc_allocate);
                set(4, &alloc_allocate_sized); set(5, &alloc_reallocate); set(6, &alloc_reallocate_sized);
                set(7, &alloc_deallocate); set(8, &alloc_allocated_size); set(12, &alloc_none);
                set(13, &alloc_make_over_buffer); set(14, &alloc_destroy);
                break;
            case 6: set(0, &log_info); set(1, &log_warning); set(2, &log_error); break;
            case 0x18: set(0, &profile_start); set(1, &profile_stop); break;
            case 0x20: set(13, &cs_create); set(14, &cs_destroy); set(15, &cs_enter); set(16, &cs_leave); set(17, &cs_try); break;
            case 0x23: set(0, &no_handler); break;
            case 0x26: set(0, &job_run); set(1, &job_wait); break;
            default: break;
        }
    }
    return t.data();
}

// ---- the plugin --------------------------------------------------------------------------------------------------
using GetApi = void* (*)(unsigned);
using Create = void* (*)();
using Destroy = void (*)(void*);
using PushSeed = void (*)(void*, const float*);
using BeginTriangles = void (*)(void*);
using PushTriangle = void (*)(void*, const float*, const float*, const float*);
// push tag volume (slot +0x48): context, outline (float3 each), count, bottom, top, tag descriptor {u32 exclusive,
// f32 color[4], i32 layer, i32 smart object, i32 user data} (the Lua wrapper's converter, 180003360)
using PushTagVolume = void (*)(void*, const float*, unsigned, float, float, const std::uint32_t*);
// generate to disk: context, base folder, output folder (relative), sector GUIDs, count, sector name, GUIDs to
// rebuild, count, write the sparse companion, database index, radius, height, slope, step, min navigable surface,
// altitude tolerance, raster precision, cell size, height field sampling, consume terrains
using Generate = std::uint64_t (*)(void*, const char*, const char*, const char**, unsigned, const char*, unsigned,
                                   const char**, int, int, float, float, float, float, float, float, float, float,
                                   float, int);

struct Plugin {
    HMODULE module = nullptr;
    void** generation = nullptr;
};

bool load_plugin(const fs::path& game_folder, Plugin& plugin, std::string& error) {
    static std::mutex mutex;
    static Plugin loaded;
    static std::string load_error;
    static bool tried = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!tried) {
        tried = true;
        const fs::path dll = game_folder / "binaries" / "plugins" / "gwnav_pluginw64_release.dll";
        loaded.module = LoadLibraryW(dll.wstring().c_str());
        if (!loaded.module) {
            load_error = "cannot load " + dll.string() + " (Windows error " + std::to_string(GetLastError()) + ")";
        } else {
            const auto plugin_api = reinterpret_cast<GetApi>(GetProcAddress(loaded.module, "get_plugin_api"));
            const auto c_api = reinterpret_cast<GetApi>(GetProcAddress(loaded.module, "?get_gwnav_c_api_dynamic@@YAPEAXI@Z"));
            void** life = plugin_api ? static_cast<void**>(plugin_api(0)) : nullptr;
            if (!life || !c_api || *reinterpret_cast<unsigned*>(life) != 0x4e || !life[0x48 / 8]) {
                load_error = dll.string() + " is not the navigation plugin this tool knows (plugin API 0x4e)";
            } else {
                // the plugin's setup (+0x48) takes the host resolver; it replaces the crash handlers, so keep ours
                const auto previous = SetUnhandledExceptionFilter(nullptr);
                reinterpret_cast<void (*)(void* (*)(unsigned))>(life[0x48 / 8])(&resolve);
                SetUnhandledExceptionFilter(previous);
                loaded.generation = static_cast<void**>(c_api(8));
                if (!loaded.generation) load_error = "the navigation plugin has no generation API";
            }
        }
    }
    if (!load_error.empty()) { error = load_error; return false; }
    plugin = loaded;
    return true;
}

// A fault inside the plugin ends the process; say where (plugin addresses at its preferred base 0x180000000) first.
HMODULE g_plugin_module = nullptr;
LONG WINAPI report_fault(EXCEPTION_POINTERS* e) {
    const auto base = reinterpret_cast<std::uint64_t>(g_plugin_module);
    const auto where = [&](std::uint64_t at) {
        return at >= base && at < base + 0x1000000 ? at - base + 0x180000000ull : at;
    };
    std::fprintf(stderr, "the game's navigation generator crashed (exception %08lx at %llx)\n", e->ExceptionRecord->ExceptionCode,
                 static_cast<unsigned long long>(where(reinterpret_cast<std::uint64_t>(e->ExceptionRecord->ExceptionAddress))));
    CONTEXT context = *e->ContextRecord;
    for (int depth = 0; depth < 16 && context.Rip; ++depth) {
        DWORD64 image = 0;
        RUNTIME_FUNCTION* entry = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        std::fprintf(stderr, "  %llx\n", static_cast<unsigned long long>(where(context.Rip)));
        if (!entry) break;
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, entry, &context, &handler_data, &frame, nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (!g_log.empty()) std::fprintf(stderr, "%s", g_log.c_str());
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}  // namespace

bool generate_navdata(const fs::path& game_folder, const NavInput& input, const NavSettings& settings,
                      const std::string& sector, const std::string& guid, NavOutput& out, std::string& error) {
    if (input.indices.empty() || input.indices.size() % 3) { error = "no triangles to build navigation on"; return false; }
    for (const auto i : input.indices)
        if (i >= input.vertices.size()) { error = "a navigation input triangle names a vertex that is not there"; return false; }
    if (sector.empty() || sector.find_first_of("/\\:") != std::string::npos) { error = "bad sector name: " + sector; return false; }
    Plugin plugin;
    if (!load_plugin(game_folder, plugin, error)) return false;

    static std::mutex generation_mutex;  // the plugin's generator keeps global state
    std::lock_guard<std::mutex> lock(generation_mutex);
    {
        std::lock_guard<std::mutex> log_lock(g_log_mutex);
        g_log.clear();
    }
    std::error_code ec;
    const fs::path work = fs::temp_directory_path(ec) / ("dtglb_nav_" + std::to_string(GetCurrentProcessId()) + "_" + sector);
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    if (ec) { error = "cannot make " + work.string() + ": " + ec.message(); return false; }

    void** g = plugin.generation;
    g_plugin_module = plugin.module;
    const auto previous_filter = SetUnhandledExceptionFilter(&report_fault);
    void* context = reinterpret_cast<Create>(g[0])();
    if (!context) { error = "the navigation generator gave no context"; return false; }
    reinterpret_cast<BeginTriangles>(g[6])(context);
    std::size_t unusable = 0;
    for (std::size_t t = 0; t < input.indices.size(); t += 3) {
        const auto& a = input.vertices[input.indices[t]];
        const auto& b = input.vertices[input.indices[t + 1]];
        const auto& c = input.vertices[input.indices[t + 2]];
        bool finite = true;
        for (const auto* v : {&a, &b, &c})
            for (const float x : *v) finite = finite && std::isfinite(x) && std::fabs(x) < 1e6f;
        if (!finite) { ++unusable; continue; }
        reinterpret_cast<PushTriangle>(g[7])(context, a.data(), b.data(), c.data());
    }
    if (unusable) log_line("input", "", (std::to_string(unusable) + " triangles with coordinates that are not numbers left out").c_str());
    for (const auto& seed : input.seeds) reinterpret_cast<PushSeed>(g[5])(context, seed.data());
    for (const auto& volume : input.tag_volumes) {
        if (volume.outline.size() < 3) continue;
        std::uint32_t tag[8] = {volume.exclusive ? 1u : 0u};
        std::memcpy(tag + 1, volume.color.data(), 16);
        tag[5] = static_cast<std::uint32_t>(volume.layer);
        tag[6] = static_cast<std::uint32_t>(volume.smart_object);
        tag[7] = static_cast<std::uint32_t>(-1);
        reinterpret_cast<PushTagVolume>(g[9])(context, volume.outline.front().data(), static_cast<unsigned>(volume.outline.size()),
                                              volume.bottom, volume.top, tag);
    }
    const std::string base = work.string(), output_folder = "navigation";
    const char* ids[1] = {guid.c_str()};
    const std::uint64_t ok = reinterpret_cast<Generate>(g[10])(
        context, base.c_str(), output_folder.c_str(), ids, 1, sector.c_str(), 0, nullptr, 1, 0, settings.entity_radius,
        settings.entity_height, settings.slope_max, settings.step_max, settings.min_navigable_surface,
        settings.altitude_tolerance, settings.raster_precision, settings.cell_size, settings.height_field_sampling,
        settings.consume_terrain ? 1 : 0);
    reinterpret_cast<Destroy>(g[1])(context);
    SetUnhandledExceptionFilter(previous_filter);
    {
        std::lock_guard<std::mutex> log_lock(g_log_mutex);
        out.log = g_log;
    }
    const fs::path folder = work / output_folder;
    out.mesh = read_file(folder / (sector + ".NavData"));
    out.sparse = read_file(folder / (sector + "_sparse.NavData"));
    fs::remove_all(work, ec);
    if (!ok || out.mesh.empty() || out.sparse.empty()) {
        error = "the navigation generator made no navigation" + (out.log.empty() ? std::string() : ":\n" + out.log);
        return false;
    }
    return true;
}
#endif

}  // namespace dtglb::stingray::navigation
