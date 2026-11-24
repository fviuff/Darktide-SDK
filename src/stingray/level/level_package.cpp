#include "stingray/level/level_package.h"

#include "gltf/json_value.h"
#include "stingray/cooked_resource.h"
#include "stingray/lua_data_compiler.h"
#include "stingray/level/level_navigation.h"
#include "stingray/dynamic_data.h"
#include "stingray/flow/flow_graph.h"
#include "stingray/murmur_hash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace dtglb::stingray::level {
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> load(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string hex16(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

json::Value key(const std::string& type, const std::string& name) {
    auto k = json::Value::object();
    k.set("type", json::Value::of(type));
    k.set("name", json::Value::of(name));
    return k;
}

json::Value file_record(const std::string& path, std::size_t size) {
    auto f = json::Value::object();
    f.set("path", json::Value::of(path));
    f.set("size", json::Value::of(static_cast<double>(size)));
    return f;
}

}  // namespace

LevelDocument new_level_document(const std::string& path) {
    LevelDocument d;
    d.name = path + ".level";
    // the level flow container with nothing in it (identical in all 248 game levels without level flow)
    const std::uint32_t words[24] = {0, 0, 0x5c, 0, 0x5c, 0, 0, 0x5c, 0x5c, 0, 0x60, 0, 0x60, 0, 0, 0, 0, 0x60, 0x60, 0x60, 0x60, 0x60, 0x20, 0};
    d.flow.resize(sizeof words);
    std::memcpy(d.flow.data(), words, sizeof words);
    d.flow_data.assign(0x20, 0);
    d.level_data.assign(4, 0);  // dynamic data with no entries
    return d;
}

ModFiles mod_files(const std::vector<fs::path>& custom_folders) {
    ModFiles out;
    for (const auto& custom : custom_folders) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(custom, ec)) {
            const auto text = load(entry.path() / "build.json");
            json::Value build;
            std::string problem;
            if (text.empty() || !json::parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), build, problem))
                continue;
            const auto* resources = build.find("resources");
            if (!resources || !resources->is_array()) continue;
            for (const auto& r : resources->items) {
                const auto* k = r.find("key");
                const auto* type = k ? k->find("type") : nullptr;
                const auto* name = k ? k->find("name") : nullptr;
                const auto* file = r.find("file");
                const auto* path = file ? file->find("path") : nullptr;
                if (!type || !name || !path || !type->is_string() || !name->is_string() || !path->is_string()) continue;
                const std::uint64_t id = id64(name->string);
                out.resources.emplace(std::make_pair(type->string, id), entry.path() / fs::u8path(path->string));
                out.names.emplace(id, name->string);
                const auto* stream = r.find("stream");
                const auto* stream_name = stream ? stream->find("name") : nullptr;
                const auto* stream_file = stream ? stream->find("file") : nullptr;
                const auto* stream_path = stream_file ? stream_file->find("path") : nullptr;
                // the stream's name is the path of its file in the folder (builds before 11-13 also wrote it as "name")
                if (stream_path && stream_path->is_string())
                    out.streams.emplace(stream_name && stream_name->is_string() ? stream_name->string : stream_path->string,
                                        entry.path() / fs::u8path(stream_path->string));
            }
        }
    }
    return out;
}

std::map<std::uint64_t, fs::path> mod_unit_files(const std::vector<fs::path>& custom_folders) {
    std::map<std::uint64_t, fs::path> units;
    for (const auto& [key, file] : mod_files(custom_folders).resources)
        if (key.first == "unit") units.emplace(key.second, file);
    return units;
}

std::unordered_map<std::uint64_t, std::string> read_name_dictionary(const fs::path& file) {
    std::unordered_map<std::uint64_t, std::string> names;
    std::ifstream in(file, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) names.emplace(id64(line), line);
    }
    return names;
}

bool write_level_folder(const fs::path& level_file, const fs::path& folder, const std::vector<fs::path>& custom_folders,
                        std::string& summary, std::string& error, const std::unordered_map<std::uint64_t, std::string>& names) {
    const auto named = [&](std::uint64_t id) {
        const auto found = names.find(id);
        return found != names.end() ? found->second : "#ID[" + hex16(id) + "]";
    };
    const auto cooked = load(level_file);
    LevelDocument d;
    if (!read_level_document(cooked, d, error)) return false;
    std::string name = d.name;
    if (name.size() > 6 && name.compare(name.size() - 6, 6, ".level") == 0) name.resize(name.size() - 6);
    if (name.empty()) { error = "the level has no name"; return false; }

    // the mod's own resources: every build.json in the Custom folders, by unit name hash
    struct Source {
        fs::path dir;
        const json::Value* resources = nullptr;
        json::Value build;
    };
    std::vector<Source> sources;
    std::map<std::uint64_t, std::size_t> unit_source;
    std::set<std::string> own;  // "<type>/<name hash>" of every resource the Custom folders have
    for (const auto& custom : custom_folders) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(custom, ec)) {
            const auto build_path = entry.path() / "build.json";
            if (!fs::is_regular_file(build_path, ec)) continue;
            if (fs::equivalent(entry.path(), folder, ec)) continue;  // the folder being written (an earlier write of it)
            const auto text = load(build_path);
            Source s;
            s.dir = entry.path();
            std::string problem;
            if (!json::parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), s.build, problem)) continue;
            sources.push_back(std::move(s));
        }
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        sources[i].resources = sources[i].build.find("resources");
        if (!sources[i].resources || !sources[i].resources->is_array()) continue;
        for (const auto& r : sources[i].resources->items) {
            const auto* k = r.find("key");
            const auto* type = k ? k->find("type") : nullptr;
            const auto* rname = k ? k->find("name") : nullptr;
            if (type && rname && type->string == "unit") unit_source.emplace(id64(rname->string), i);
            if (type && rname) own.insert(type->string + "/" + hex16(id64(rname->string)));
        }
    }

    fs::create_directories(folder);
    auto resources = json::Value::array();
    auto externals = json::Value::array();
    std::set<std::string> external_keys;  // "<type>/<name hash>": each game resource listed once
    // path: the name as given, used when the dictionary doesn't know the hash
    const auto add_external = [&](const std::string& type, std::uint64_t id, const std::string& path = {}) {
        if (!id || !external_keys.insert(type + "/" + hex16(id)).second) return false;
        auto e = json::Value::object();
        e.set("key", key(type, names.count(id) || path.empty() ? named(id) : path));
        e.set("package_member", json::Value::of(false));
        externals.push(std::move(e));
        return true;
    };
    std::set<std::string> listed;
    // the level itself
    const std::string level_path = fs::u8path(name).filename().string() + ".level";
    {
        std::ofstream(folder / level_path, std::ios::binary).write(reinterpret_cast<const char*>(cooked.data()), static_cast<std::streamsize>(cooked.size()));
        auto r = json::Value::object();
        r.set("key", key("level", name));
        r.set("name_hash", json::Value::of(hex16(id64(name))));
        r.set("file", file_record(level_path, cooked.size()));
        r.set("dependencies", json::Value::array());
        resources.push(std::move(r));
        listed.insert("level/" + name);
    }
    // its scripts the game loads by name (<level>_volume_data: volume events and navigation tags; <level>_main_path: the
    // main path), from the Lua next to the level file: compiled as the game's Lua resources, the source beside them
    std::size_t scripts = 0;
    for (const char* suffix : {"_volume_data", "_main_path"}) {
        const fs::path script = level_file.parent_path() / fs::path(level_file.stem().wstring() + fs::u8path(suffix).wstring() + L".lua");
        std::error_code ec;
        if (!fs::is_regular_file(script, ec)) continue;
        const auto bytes = load(script);
        const std::string source(bytes.begin(), bytes.end());
        const std::string resource = name + suffix;
        std::vector<std::uint8_t> dump;
        std::string problem;
        if (!compile_lua_data(source, "@" + resource + ".lua", LuaDumpForm::game, dump, problem)) {
            error = script.string() + ": " + problem;
            return false;
        }
        const auto cooked = wrap_cooked_resource("lua", resource, lua_resource_body(dump));
        const std::string leaf = fs::u8path(resource).filename().string();
        const std::string relative = "data/lua/" + leaf;
        fs::create_directories(folder / "data" / "lua", ec);
        std::ofstream(folder / fs::u8path(relative), std::ios::binary).write(reinterpret_cast<const char*>(cooked.data()),
                                                                           static_cast<std::streamsize>(cooked.size()));
        std::ofstream(folder / fs::u8path(leaf + ".lua"), std::ios::binary).write(source.data(), static_cast<std::streamsize>(source.size()));
        auto r = json::Value::object();
        r.set("key", key("lua", resource));
        r.set("file", file_record(relative, cooked.size()));
        resources.push(std::move(r));
        listed.insert("lua/" + resource);
        ++scripts;
    }
    // the level's own Lua (<level file stem>_script.lua and the files of <stem>_scripts/): plain files beside its
    // resources, which Custom Levels runs from the folder (they are not resources of the package)
    {
        const std::wstring stem = level_file.stem().wstring();
        const std::string leaf = fs::u8path(name).filename().string();
        std::error_code ec;
        const fs::path script = level_file.parent_path() / fs::path(stem + L"_script.lua");
        if (fs::is_regular_file(script, ec)) {
            fs::copy_file(script, folder / fs::u8path(leaf + "_script.lua"), fs::copy_options::overwrite_existing, ec);
            if (ec) { error = "cannot copy " + script.string() + ": " + ec.message(); return false; }
        }
        const fs::path more = level_file.parent_path() / fs::path(stem + L"_scripts");
        if (fs::is_directory(more, ec))
            for (const auto& entry : fs::recursive_directory_iterator(more, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".lua") continue;
                const fs::path target = folder / "scripts" / entry.path().lexically_relative(more);
                fs::create_directories(target.parent_path(), ec);
                fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
                if (ec) { error = "cannot copy " + entry.path().string() + ": " + ec.message(); return false; }
            }
    }
    // its navigation (level_navigation.h): our files next to the level, or the game's own NavData
    std::size_t navigation = 0;
    {
        const auto names = navigation_names(level_file, name);
        const std::string nav_type = "NavData";  // the engine's type name (its hash: kNavDataType)
        for (const auto& row : d.nav_data) {
            std::uint64_t id = 0;
            std::memcpy(&id, row.data(), 8);
            const bool mesh = id == id64(names.mesh), sparse = id == id64(names.sparse);
            const fs::path file = mesh ? names.mesh_file : names.sparse_file;
            std::error_code ec;
            if ((mesh || sparse) && fs::is_regular_file(file, ec)) {
                const auto bytes = load(file);
                const std::string resource_name = mesh ? names.mesh : names.sparse;
                const std::string relative = "navigation/" + file.filename().string();
                fs::create_directories(folder / "navigation", ec);
                std::ofstream(folder / fs::u8path(relative), std::ios::binary)
                    .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                auto r = json::Value::object();
                r.set("key", key(nav_type, resource_name));
                r.set("name_hash", json::Value::of(hex16(id)));
                r.set("file", file_record(relative, bytes.size()));
                r.set("dependencies", json::Value::array());
                resources.push(std::move(r));
                ++navigation;
            } else if (mesh || sparse) {
                error = "the level lists its navigation but " + file.string() + " is not there (build it again)";
                return false;
            } else {
                add_external(nav_type, id);
            }
        }
    }
    std::set<std::uint64_t> units;
    for (const auto& u : d.units) units.insert(u.resource);
    std::size_t copied = 0, game_units = 0;
    std::set<std::size_t> done;
    for (const auto unit : units) {
        const auto found = unit_source.find(unit);
        if (found == unit_source.end()) {
            game_units += add_external("unit", unit);
            continue;
        }
        if (!done.insert(found->second).second) continue;
        const auto& source = sources[found->second];
        for (const auto& r : source.resources->items) {
            const auto* k = r.find("key");
            if (!k || !k->find("type") || !k->find("name")) continue;
            if (!listed.insert(k->find("type")->string + "/" + k->find("name")->string).second) continue;
            // the files it names, copied at the same relative paths
            for (const auto* file : {r.find("file"), r.find("stream") ? r.find("stream")->find("file") : nullptr}) {
                if (!file || !file->find("path")) continue;
                const auto relative = fs::u8path(file->find("path")->string);
                std::error_code ec;
                fs::create_directories((folder / relative).parent_path(), ec);
                fs::copy_file(source.dir / relative, folder / relative, fs::copy_options::overwrite_existing, ec);
                if (ec) { error = "cannot copy " + (source.dir / relative).string() + ": " + ec.message(); return false; }
            }
            resources.push(r);
            ++copied;
        }
        if (const auto* more = source.build.find("external_resources"); more && more->is_array())
            for (const auto& e : more->items) {
                const auto* k = e.find("key");
                const auto* type = k ? k->find("type") : nullptr;
                const auto* n = k ? k->find("name") : nullptr;
                if (!type || !type->is_string() || !n || !n->is_string() || n->string.empty()) continue;
                // a #ID[<16 hex>] name is listed by path where the dictionary knows it
                const bool hashed = n->string.size() == 21 && n->string.rfind("#ID[", 0) == 0;
                if (hashed) add_external(type->string, std::strtoull(n->string.substr(4, 16).c_str(), nullptr, 16));
                else add_external(type->string, id64(n->string), n->string);
            }
    }

    // the game's other resources the level uses
    std::size_t referenced = 0;
    const auto reference = [&](const std::string& type, std::uint64_t id, const std::string& path = {}) {
        if (!own.count(type + "/" + hex16(id))) referenced += add_external(type, id, path);
    };
    for (const auto& effect : d.effects) {
        std::uint64_t id = 0;
        std::memcpy(&id, effect.data(), 8);
        reference("particles", id);
    }
    for (const auto& nested : d.nested) {
        if (nested.record.size() <= 0x40) continue;
        const auto* text = reinterpret_cast<const char*>(nested.record.data() + 0x40);
        const auto length = strnlen(text, nested.record.size() - 0x40);
        std::string path(text, length);
        if (path.size() > 6 && path.compare(path.size() - 6, 6, ".level") == 0) path.resize(path.size() - 6);
        if (!path.empty()) reference("level", id64(path), path);
    }
    {
        std::vector<DynamicEntry> data;
        std::size_t size = 0;
        std::string problem;
        if (!d.level_data.empty() && read_dynamic_data(d.level_data, 0, data, size, problem))
            if (const auto* environment = find_dynamic(data, {"shading_environment"}); environment && environment->type == 3) {
                std::string path(environment->bytes.begin(), environment->bytes.end());
                while (!path.empty() && path.back() == '\0') path.pop_back();
                if (!path.empty()) reference("shading_environment", id64(path), path);
            }
    }
    if (!d.flow.empty()) {
        flow::Graph graph;
        std::string problem;
        if (flow::read_graph(d.flow, d.flow_data, graph, problem))
            for (const auto& node : graph.nodes) {
                if (node.empty) continue;
                const char* port = node.type == 0xb ? "effect" : node.type == 0x1c ? "resource" : node.type == 0x2 ? "unit" : nullptr;
                if (!port) continue;
                for (const auto& p : flow::node_ports(graph, node)) {
                    if (p.name != port || p.slot == flow::kNoSlot) continue;
                    const auto value = flow::value_text("string", graph.dynamic, p.slot);
                    if (!value.empty()) reference(node.type == 0xb ? "particles" : "unit", id64(value), value);
                }
            }
    }

    auto build = json::Value::object();
    build.set("schema", json::Value::of(1.0));
    // Custom Assets reads the keys, names and external resources of a build.json the compiler wrote
    auto compiler = json::Value::object();
    compiler.set("name", json::Value::of(std::string("DarktideGLBCompiler")));
    compiler.set("version", json::Value::of(std::string("0.3.0-beta")));
    build.set("compiler", std::move(compiler));
    auto target = json::Value::object();
    target.set("id", json::Value::of(std::string("darktide")));
    build.set("target", std::move(target));
    auto roots = json::Value::array();
    roots.push(key("level", name));
    build.set("roots", std::move(roots));
    // the folder's package carries the level's name: the game loads a mission's level by loading that package
    auto package = json::Value::object();
    package.set("key", key("package", name));
    build.set("package", std::move(package));
    build.set("resources", std::move(resources));
    build.set("external_resources", std::move(externals));
    std::ofstream(folder / "build.json", std::ios::binary) << json::dump(build, 2) << "\n";
    summary = name + ": " + std::to_string(d.units.size()) + " units, " + std::to_string(copied) + " resources of the mod copied in, " +
              std::to_string(game_units) + " game units and " + std::to_string(referenced) +
              " other game resources it loads first" +
              (navigation ? ", navigation" : ", no navigation") + ", " + std::to_string(scripts) + " compiled data scripts";
    return true;
}

}  // namespace dtglb::stingray::level
