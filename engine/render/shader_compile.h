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
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace engine {

using Microsoft::WRL::ComPtr;

// Where startup time goes, phase by phase: every shader compile and every
// init step appends (what, milliseconds). main writes it to build/startup.txt.
inline std::vector<std::pair<std::string, double> >& startup_log() {
    static std::vector<std::pair<std::string, double> > log;
    return log;
}
inline double startup_now_ms() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * double(t.QuadPart) / double(freq.QuadPart);
}

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

// The shader cache. Startup was 45 s, 40 of it FXC compiling one kernel, and
// almost every run compiles text identical to the last run's. So each shader
// is PREPROCESSED with its includes (milliseconds), the expanded text is
// hashed with the entry point, target, flags and compiler, and the compiled
// blob is kept under build/shader_cache/<hash>.cso. Editing a shader -- or
// anything it includes, field.h among them -- changes the hash, so a stale
// blob cannot be served. On by default; --no-shader-cache turns it off, and
// --check-dialect always compiles, because compiling is the thing it checks.
inline bool& shader_cache_enabled() {
    static bool on = true;
    return on;
}

inline unsigned long long fnv1a64(const void* data, size_t n, unsigned long long h) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline bool read_whole_file(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    const size_t got = n > 0 ? std::fread(&out[0], 1, size_t(n), f) : 0;
    std::fclose(f);
    out.resize(got);
    return true;
}

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
    const double t0 = startup_now_ms();

    std::string src;
    if (!read_whole_file(path, src)) {
        error = std::string("compiling ") + path + " " + entry + " (" + target + "):\n" +
                "could not read the file (is the working directory the repository root?)";
        return false;
    }

    // Preprocess: the expanded text is what the hash must cover, and it is
    // also what gets compiled, so the include files are read once.
    ComPtr<ID3DBlob> pre, errors;
    HRESULT hr = D3DPreprocess(src.data(), src.size(), path, nullptr, &inc, &pre, &errors);
    if (FAILED(hr)) {
        error = std::string("preprocessing ") + path + " " + entry + ":\n";
        error += errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)";
        return false;
    }

    unsigned long long h = 1469598103934665603ull;
    h = fnv1a64(pre->GetBufferPointer(), pre->GetBufferSize(), h);
    h = fnv1a64(entry, std::strlen(entry), h);
    h = fnv1a64(target, std::strlen(target), h);
    h = fnv1a64(&flags, sizeof(flags), h);
    const UINT compiler = D3D_COMPILER_VERSION;
    h = fnv1a64(&compiler, sizeof(compiler), h);
    char cache_path[96];
    std::snprintf(cache_path, sizeof(cache_path), "build/shader_cache/%016llx.cso", h);

    if (shader_cache_enabled()) {
        std::string blob;
        if (read_whole_file(cache_path, blob) && !blob.empty() &&
            SUCCEEDED(D3DCreateBlob(blob.size(), &out))) {
            std::memcpy(out->GetBufferPointer(), blob.data(), blob.size());
            startup_log().push_back(std::make_pair(std::string("cached  ") + path + " " + entry,
                                                   startup_now_ms() - t0));
            return true;
        }
    }

    errors.Reset();
    hr = D3DCompile(pre->GetBufferPointer(), pre->GetBufferSize(), path, nullptr, nullptr,
                    entry, target, flags, 0, &out, &errors);
    // FAILED, not "compile": a failed compile is never cached, so logging it
    // as a compile made the next run's recompile look like a cache miss.
    startup_log().push_back(std::make_pair(std::string(FAILED(hr) ? "FAILED  " : "compile ") +
                                               path + " " + entry,
                                           startup_now_ms() - t0));
    if (FAILED(hr)) {
        error = std::string("compiling ") + path + " " + entry + " (" + target + "):\n";
        if (errors) {
            error += static_cast<const char*>(errors->GetBufferPointer());
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
            error += buf;
        }
        return false;
    }
    if (shader_cache_enabled()) {
        CreateDirectoryA("build", nullptr);
        CreateDirectoryA("build/shader_cache", nullptr);
        if (FILE* f = std::fopen(cache_path, "wb")) {
            std::fwrite(out->GetBufferPointer(), 1, out->GetBufferSize(), f);
            std::fclose(f);
        }
    }
    return true;
}

}  // namespace engine

#endif  // ENGINE_SHADER_COMPILE_H
