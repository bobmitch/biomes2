// ---------------------------------------------------------------------------
// Runtime HLSL compilation, shared by the render pipelines and the meshing
// kernels.
//
// Compiled from source at startup rather than built offline, because the whole
// of Track B's remaining work is shader editing and a rebuild in that loop is
// the difference between trying five ideas and trying one. Offline compilation
// is a shipping concern, not a prototype one.
// ---------------------------------------------------------------------------
#ifndef ENGINE_SHADER_COMPILE_H
#define ENGINE_SHADER_COMPILE_H

#include <windows.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <map>
#include <string>

namespace engine {

using Microsoft::WRL::ComPtr;

// Resolves #include the way a C++ compiler does: relative to the INCLUDING
// FILE first, then against the repository root.
//
// The second rule alone is not enough, and M1 found that out immediately.
// A shader can say `#include "engine/field/trivial_field.h"`, the same
// spelling the C++ side uses -- but worldgen/shaders/field.h says
// `#include "noise.h"`, because that is how a header refers to its sibling
// and the C++ build has never needed anything else. ADR-001's claim that
// "HLSL is the same text" holds; what does not come for free is the include
// SEARCH, and a handler that only knows one root cannot open a sibling.
//
// D3DCompile hands Open the parent's buffer pointer rather than its path, so
// the directory each buffer came from is remembered on the way out.
class RootInclude : public ID3DInclude {
public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID parent,
                           LPCVOID* out_data, UINT* out_bytes) override {
        std::string path;
        if (parent) {
            std::map<const void*, std::string>::const_iterator it = dirs_.find(parent);
            if (it != dirs_.end() && !it->second.empty()) {
                const std::string sibling = it->second + "/" + name;
                if (exists(sibling)) path = sibling;
            }
        }
        if (path.empty()) path = name;   // repository root

        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return E_FAIL;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        char* buf = new char[n > 0 ? n : 1];
        const size_t got = n > 0 ? std::fread(buf, 1, static_cast<size_t>(n), f) : 0;
        std::fclose(f);
        *out_data = buf;
        *out_bytes = static_cast<UINT>(got);
        dirs_[buf] = directory_of(path);
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID data) override {
        dirs_.erase(data);
        delete[] static_cast<const char*>(data);
        return S_OK;
    }

    // The top-level file's directory, so its own includes resolve as siblings
    // too. D3DCompileFromFile never calls Open for the entry file itself.
    void set_root_file(const std::string& path) { root_dir_ = directory_of(path); }

private:
    static bool exists(const std::string& p) {
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }
    static std::string directory_of(const std::string& p) {
        const size_t slash = p.find_last_of("/\\");
        return slash == std::string::npos ? std::string() : p.substr(0, slash);
    }

    std::map<const void*, std::string> dirs_;
    std::string root_dir_;
};

inline bool compile_shader(const char* path, const char* entry, const char* target,
                           ComPtr<ID3DBlob>& out, std::string& error) {
    RootInclude inc;
    inc.set_root_file(path);
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    wchar_t wpath[512];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);

    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompileFromFile(wpath, nullptr, &inc, entry, target,
                                          flags, 0, &out, &errors);
    if (FAILED(hr)) {
        error = std::string("compiling ") + path + " " + entry + " (" + target + "):\n";
        if (errors) {
            error += static_cast<const char*>(errors->GetBufferPointer());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
            error += buf;
            error += "\n(is the working directory the repository root?)";
        }
        return false;
    }
    return true;
}

}  // namespace engine

#endif  // ENGINE_SHADER_COMPILE_H
