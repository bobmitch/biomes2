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
#include <string>

namespace engine {

using Microsoft::WRL::ComPtr;

// Resolves #include against the repository root rather than against the
// including file. A shader says `#include "engine/field/trivial_field.h"`,
// which is the same spelling the C++ side uses -- one path convention for the
// dialect headers instead of a different relative walk from each consumer.
class RootInclude : public ID3DInclude {
public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID,
                           LPCVOID* out_data, UINT* out_bytes) override {
        FILE* f = std::fopen(name, "rb");
        if (!f) return E_FAIL;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        char* buf = new char[n > 0 ? n : 1];
        const size_t got = n > 0 ? std::fread(buf, 1, static_cast<size_t>(n), f) : 0;
        std::fclose(f);
        *out_data = buf;
        *out_bytes = static_cast<UINT>(got);
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID data) override {
        delete[] static_cast<const char*>(data);
        return S_OK;
    }
};

inline bool compile_shader(const char* path, const char* entry, const char* target,
                           ComPtr<ID3DBlob>& out, std::string& error) {
    RootInclude inc;
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
