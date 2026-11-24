#include "stingray/material/shader_compiler.h"

#include "stingray/murmur_hash.h"
#include "stingray/texture/texture_writer.h"

#include <algorithm>
#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <unknwn.h>
#include <dxcapi.h>
#include <d3d12shader.h>
#endif

namespace dtglb::stingray::material {
namespace {

std::uint32_t id32(std::string_view name) {
    // dxc names a texture / sampler pair from a combined sampler object __tex_<name> / __samp_<name>; the game's
    // records use <name>
    for (const std::string_view prefix : {"__tex_", "__samp_"})
        if (name.substr(0, prefix.size()) == prefix) name.remove_prefix(prefix.size());
    return id32_from_id64(name);
}

#ifdef _WIN32
template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// dxcompiler.dll loads dxil.dll (the validator that signs the DXIL) by name, so dxil.dll is loaded from the same
// folder first.
DxcCreateInstanceProc dxc_create(std::string& error) {
    static DxcCreateInstanceProc create = nullptr;
    if (create) return create;
    std::vector<std::filesystem::path> folders;
    std::vector<wchar_t> exe(32768);
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (length && length < exe.size()) folders.push_back(std::filesystem::path(exe.data(), exe.data() + length).parent_path());
    std::vector<wchar_t> kits(32768);
    const DWORD kits_length = GetEnvironmentVariableW(L"ProgramFiles(x86)", kits.data(), static_cast<DWORD>(kits.size()));
    if (kits_length && kits_length < kits.size()) {
        const auto root = std::filesystem::path(kits.data(), kits.data() + kits_length) / L"Windows Kits" / L"10";
        folders.push_back(root / L"Redist" / L"D3D" / L"x64");
        std::error_code ec;
        std::vector<std::filesystem::path> versions;
        for (const auto& entry : std::filesystem::directory_iterator(root / L"bin", ec))
            if (std::filesystem::exists(entry.path() / L"x64" / L"dxcompiler.dll", ec)) versions.push_back(entry.path() / L"x64");
        std::sort(versions.rbegin(), versions.rend());
        folders.insert(folders.end(), versions.begin(), versions.end());
    }
    for (const auto& folder : folders) {
        std::error_code ec;
        if (!std::filesystem::exists(folder / L"dxcompiler.dll", ec) || !std::filesystem::exists(folder / L"dxil.dll", ec)) continue;
        if (!LoadLibraryExW((folder / L"dxil.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) continue;
        HMODULE module = LoadLibraryExW((folder / L"dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) continue;
        create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
        if (create) return create;
    }
    error = "DirectX shader compiler not found: put dxcompiler.dll and dxil.dll next to DarktideGLBCompiler.exe "
            "(or install the Windows SDK)";
    return nullptr;
}
#endif

} // namespace

bool compile_hlsl(const std::string& source, const std::string& source_name, const std::string& entry,
                  const std::string& profile, std::vector<std::uint8_t>& dxbc, std::string& error) {
#ifdef _WIN32
    const auto create = dxc_create(error);
    if (!create) return false;
    Com<IDxcCompiler3> compiler;
    if (FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&compiler)))) {
        error = "cannot start the DirectX shader compiler"; return false;
    }
    const auto wname = widen(source_name), wentry = widen(entry), wprofile = widen(profile);
    std::vector<LPCWSTR> args{wname.c_str(), L"-E", wentry.c_str(), L"-T", wprofile.c_str(), L"-O3",
                              L"-validator-version", L"1.7"};
    const DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    Com<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()), nullptr,
                                 __uuidof(IDxcResult), reinterpret_cast<void**>(&result)))) {
        error = "the DirectX shader compiler failed to run"; return false;
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    Com<IDxcBlobUtf8> messages;
    result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&messages), nullptr);
    if (FAILED(status)) {
        error = messages.p && messages->GetStringLength()
            ? std::string(messages->GetStringPointer(), messages->GetStringLength()) : "shader compile failed";
        return false;
    }
    Com<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&object), nullptr)) ||
        !object.p || !object->GetBufferSize()) {
        error = "the DirectX shader compiler returned no bytecode"; return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(object->GetBufferPointer());
    dxbc.assign(bytes, bytes + object->GetBufferSize());
    return true;
#else
    (void)source; (void)source_name; (void)entry; (void)profile; (void)dxbc;
    error = "compiling shaders needs the Windows DirectX shader compiler";
    return false;
#endif
}

bool make_shader_variant(const std::vector<std::uint8_t>& dxbc, RootTable& roots, ShaderVariant& out,
                         std::string& error) {
#ifdef _WIN32
    const auto create = dxc_create(error);
    if (!create) return false;
    Com<IDxcUtils> utils;
    Com<ID3D12ShaderReflection> reflection;
    const DxcBuffer buffer{dxbc.data(), dxbc.size(), 0};
    if (FAILED(create(CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(&utils))) ||
        FAILED(utils->CreateReflection(&buffer, __uuidof(ID3D12ShaderReflection), reinterpret_cast<void**>(&reflection)))) {
        error = "cannot read the compiled shader's reflection"; return false;
    }
    D3D12_SHADER_DESC desc{};
    reflection->GetDesc(&desc);
    ShaderVariant v;
    const auto root = [&](std::uint32_t name) {
        const auto found = std::find(roots.begin(), roots.end(), name);
        if (found != roots.end()) return static_cast<std::uint32_t>(found - roots.begin());
        roots.push_back(name);
        return static_cast<std::uint32_t>(roots.size() - 1);
    };
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC b{};
        reflection->GetResourceBindingDesc(i, &b);
        const auto name = id32(b.Name);
        const bool unbounded = b.BindCount == 0;
        const std::uint32_t count = unbounded ? 0xffffffffu : b.BindCount;
        switch (b.Type) {
        case D3D_SIT_CBUFFER: {
            D3D12_SHADER_BUFFER_DESC cb{};
            reflection->GetConstantBufferByName(b.Name)->GetDesc(&cb);
            v.constant_buffers[0].push_back({name, root(name), cb.Size, b.BindPoint, count, b.Space});
            break;
        }
        case D3D_SIT_SAMPLER:
            if (unbounded) v.sampler_arrays.push_back({name, b.BindPoint, 0, b.Space});
            else v.static_samplers.push_back({name, b.BindPoint, count, b.Space});
            break;
        default: {
            const bool uav = b.Type == D3D_SIT_UAV_RWTYPED || b.Type == D3D_SIT_UAV_RWSTRUCTURED ||
                b.Type == D3D_SIT_UAV_RWBYTEADDRESS || b.Type == D3D_SIT_UAV_APPEND_STRUCTURED ||
                b.Type == D3D_SIT_UAV_CONSUME_STRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
            const bool raw = b.Type == D3D_SIT_BYTEADDRESS || b.Type == D3D_SIT_UAV_RWBYTEADDRESS;
            const bool structured = b.Type == D3D_SIT_STRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED ||
                b.Type == D3D_SIT_UAV_APPEND_STRUCTURED || b.Type == D3D_SIT_UAV_CONSUME_STRUCTURED ||
                b.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
            const std::uint32_t stride = raw ? 0u : structured ? b.NumSamples : 0xffffffffu;
            v.resources[(uav ? 3 : 1) + (unbounded ? 1 : 0)].push_back(
                {name, root(name), b.BindPoint, count, b.Space, stride, 0});
            break;
        }
        }
    }
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC p{};
        reflection->GetInputParameterDesc(i, &p);
        v.inputs.push_back({id32_from_id64(p.SemanticName), p.SemanticIndex, p.Register});
    }
    if (!texture::oodle_compress(dxbc, v.bytecode, error)) return false;
    v.compression = 5;
    v.size = static_cast<std::uint32_t>(dxbc.size());
    v.hash = murmur64(v.bytecode.data(), v.bytecode.size());
    out = std::move(v);
    return true;
#else
    (void)dxbc; (void)roots; (void)out;
    error = "reading shader reflection needs the Windows DirectX shader compiler";
    return false;
#endif
}

#ifdef _WIN32
namespace {

// shader_data variable class of a constant buffer member and its element stride: float 0, float2..4 1..3 (uint
// vectors too), float4x4 4, uint 5, float4x4 array 10 - every class the game's shaders use
bool variable_class(const D3D12_SHADER_TYPE_DESC& type, std::uint32_t& klass, std::uint32_t& stride) {
    const bool array = type.Elements != 0;
    const bool number = type.Type == D3D_SVT_FLOAT || type.Type == D3D_SVT_UINT;
    if (type.Class == D3D_SVC_SCALAR && !array && number) {
        klass = type.Type == D3D_SVT_FLOAT ? 0 : 5;
        stride = 4;
    } else if (type.Class == D3D_SVC_VECTOR && !array && number && type.Columns >= 2 && type.Columns <= 4) {
        klass = type.Columns - 1;
        stride = 4 * type.Columns;
    } else if ((type.Class == D3D_SVC_MATRIX_ROWS || type.Class == D3D_SVC_MATRIX_COLUMNS) && type.Type == D3D_SVT_FLOAT &&
               type.Rows == 4 && type.Columns == 4) {
        klass = array ? 10 : 4;
        stride = 64;
    } else {
        return false;
    }
    return true;
}

struct BindlessKind { std::string_view prefix; std::uint32_t kind; };
// c_per_object members the engine fills with a material texture's bindless indices: __BINDLESS_<kind>_<texture>
constexpr BindlessKind bindless_kinds[] = {
    {"__BINDLESS_TEX2D_", 0}, {"__BINDLESS_SAMPLER_", 5}, {"__BINDLESS_MINLOD_", 8},
};

} // namespace
#endif

bool make_shader_data(const std::vector<std::array<std::vector<std::uint8_t>, 6>>& passes, ShaderData& out,
                      std::string& error) {
#ifdef _WIN32
    const auto create = dxc_create(error);
    if (!create) return false;
    Com<IDxcUtils> utils;
    if (FAILED(create(CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(&utils)))) {
        error = "cannot start the DirectX shader compiler"; return false;
    }
    out.resources.clear();
    out.constant_buffers.clear();
    out.records.clear();
    std::vector<std::uint32_t> cbuffer_of;           // per resource: its constant buffer reflection or ~0
    std::vector<std::uint32_t> first_pass;           // per resource: the pass that binds it first
    std::vector<std::vector<std::string>> members;   // per constant buffer reflection: member names as written
    for (std::size_t p = 0; p < passes.size(); ++p) {
        for (const auto& dxbc : passes[p]) {
            if (dxbc.empty()) continue;
            Com<ID3D12ShaderReflection> reflection;
            const DxcBuffer buffer{dxbc.data(), dxbc.size(), 0};
            if (FAILED(utils->CreateReflection(&buffer, __uuidof(ID3D12ShaderReflection), reinterpret_cast<void**>(&reflection)))) {
                error = "cannot read a compiled shader's reflection"; return false;
            }
            D3D12_SHADER_DESC desc{};
            reflection->GetDesc(&desc);
            for (UINT i = 0; i < desc.BoundResources; ++i) {
                D3D12_SHADER_INPUT_BIND_DESC b{};
                reflection->GetResourceBindingDesc(i, &b);
                if (b.Type == D3D_SIT_SAMPLER) continue;
                const std::string_view name(b.Name);
                const bool uav = b.Type == D3D_SIT_UAV_RWTYPED || b.Type == D3D_SIT_UAV_RWSTRUCTURED ||
                    b.Type == D3D_SIT_UAV_RWBYTEADDRESS || b.Type == D3D_SIT_UAV_APPEND_STRUCTURED ||
                    b.Type == D3D_SIT_UAV_CONSUME_STRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
                // type: kind (0 constant buffer, 1 global constant buffer, 3 texture, 4 structured buffer, 5 RW
                // buffer, 6 byte address buffer, 7 RW byte address buffer) | 0x100 for the engine's global_ ones |
                // the texture's dimension or the buffer's element stride << 16
                const bool global = name.substr(0, 7) == "global_";
                std::uint32_t type = 0;
                const auto unknown = [&] {
                    error = std::string("shader resource ") + b.Name + " is of a kind the game's shaders don't use (type " +
                            std::to_string(b.Type) + ", dimension " + std::to_string(b.Dimension) + ")";
                    return false;
                };
                switch (b.Type) {
                case D3D_SIT_CBUFFER: type = global ? 1 : 0; break;
                case D3D_SIT_TEXTURE:
                    switch (b.Dimension) {
                    case D3D_SRV_DIMENSION_TEXTURE2D: type = 3; break;
                    case D3D_SRV_DIMENSION_TEXTURE3D: type = 3 | 1u << 16; break;
                    case D3D_SRV_DIMENSION_TEXTURECUBE: type = 3 | 2u << 16; break;
                    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: type = 3 | 5u << 16; break;
                    default: return unknown();
                    }
                    break;
                case D3D_SIT_STRUCTURED: type = 4 | b.NumSamples << 16; break;
                case D3D_SIT_BYTEADDRESS: type = 6; break;
                case D3D_SIT_UAV_RWTYPED:
                    if (b.Dimension != D3D_SRV_DIMENSION_BUFFER) return unknown();
                    type = 5;
                    break;
                case D3D_SIT_UAV_RWSTRUCTURED: type = 5 | b.NumSamples << 16; break;
                case D3D_SIT_UAV_RWBYTEADDRESS: type = 7; break;
                default: return unknown();
                }
                if (global) type |= 0x100;
                const auto id = id32(name);
                auto found = std::find_if(out.resources.begin(), out.resources.end(),
                                          [&](const auto& r) { return r[0] == id && r[1] == type; });
                if (found == out.resources.end()) {
                    out.resources.push_back({id, type, 0, 0});
                    cbuffer_of.push_back(~0u);
                    first_pass.push_back(static_cast<std::uint32_t>(p));
                    found = out.resources.end() - 1;
                    if (b.Type == D3D_SIT_CBUFFER) {
                        auto* cb = reflection->GetConstantBufferByName(b.Name);
                        D3D12_SHADER_BUFFER_DESC cb_desc{};
                        cb->GetDesc(&cb_desc);
                        ConstantBufferReflection cbr;
                        cbr.size = (cb_desc.Size + 15) & ~15u;
                        std::vector<std::string> written;
                        for (UINT v = 0; v < cb_desc.Variables; ++v) {
                            auto* var = cb->GetVariableByIndex(v);
                            D3D12_SHADER_VARIABLE_DESC var_desc{};
                            var->GetDesc(&var_desc);
                            D3D12_SHADER_TYPE_DESC type_desc{};
                            var->GetType()->GetDesc(&type_desc);
                            std::uint32_t klass = 0, stride = 0;
                            if (!variable_class(type_desc, klass, stride)) {
                                error = std::string("constant buffer ") + b.Name + " member " + var_desc.Name +
                                        " is of a type material variables can't be: use float, float2, float3, float4, uint, uint2..4, float4x4 or float4x4 arrays";
                                return false;
                            }
                            std::string_view member(var_desc.Name);
                            for (const auto& bindless : bindless_kinds)
                                if (member.substr(0, bindless.prefix.size()) == bindless.prefix) member.remove_prefix(bindless.prefix.size());
                            cbr.variables.push_back({klass, type_desc.Elements, id32_from_id64(member), var_desc.StartOffset, stride});
                            written.emplace_back(var_desc.Name);
                        }
                        cbuffer_of.back() = static_cast<std::uint32_t>(out.constant_buffers.size());
                        out.constant_buffers.push_back(std::move(cbr));
                        members.push_back(std::move(written));
                    }
                }
                if (b.Type != D3D_SIT_CBUFFER)
                    (*found)[3] |= (uav ? 2u : 1u) << (2 * p);
            }
        }
    }
    // the per-object resource block: one slot per resource (24 bytes for a constant buffer, 8 otherwise), then the
    // data of the material's own constant buffers, each 16-aligned; global constant buffers keep offset 0
    std::uint32_t at = 0;
    for (auto& r : out.resources) {
        r[2] = at;
        at += (r[1] & 0xff) == 0 || (r[1] & 0xff) == 1 ? 24 : 8;
    }
    for (std::size_t i = 0; i < out.resources.size(); ++i) {
        if (cbuffer_of[i] == ~0u) continue;
        auto& cbr = out.constant_buffers[cbuffer_of[i]];
        if (out.resources[i][1] == 0) {
            at = (at + 15) & ~15u;
            cbr.offset = at;
            at += cbr.size;
            const auto cb_name = out.resources[i][0];
            const auto& written = members[cbuffer_of[i]];
            for (std::size_t v = 0; v < written.size(); ++v)
                for (const auto& bindless : bindless_kinds)
                    if (std::string_view(written[v]).substr(0, bindless.prefix.size()) == bindless.prefix) {
                        const auto& var = cbr.variables[v];
                        out.records.push_back({var[2], bindless.kind, first_pass[i], var[3], var[0], cb_name, cbuffer_of[i]});
                    }
        } else {
            cbr.offset = 0;
        }
    }
    out.resource_size = at;
    return true;
#else
    (void)passes; (void)out;
    error = "reading shader reflection needs the Windows DirectX shader compiler";
    return false;
#endif
}

}
