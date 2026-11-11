// ---------------------------------------------------------------------------
// wgslgen -- emits WGSL from the field-function dialect (ADR-001).
//
// The dialect is a restricted C subset that compiles unmodified as C++ and as
// HLSL. WGSL is the one target whose grammar differs, so it is generated. The
// restrictions (one statement per line, functional casts, no brace
// initialisers, no structs) exist so this stays a line-oriented transform
// rather than a compiler. See worldgen/shaders/noise.h for the full list.
//
//   wgslgen <input.h> <output.wgsl>
// ---------------------------------------------------------------------------
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {

enum Kind { K_SPACE = 0, K_IDENT = 1, K_NUM = 2, K_PUNCT = 3, K_COMMENT = 4 };

struct Tok {
    std::string s;
    int kind;
};

std::vector<Tok> lex(const std::string& line) {
    std::vector<Tok> out;
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
            out.push_back(Tok{line.substr(i, j - i), K_SPACE});
            i = j;
        } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            out.push_back(Tok{line.substr(i), K_COMMENT});
            break;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
            out.push_back(Tok{line.substr(i, j - i), K_IDENT});
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            if (c == '0' && i + 1 < line.size() && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
                j = i + 2;
                while (j < line.size() && std::isxdigit(static_cast<unsigned char>(line[j]))) ++j;
            } else {
                while (j < line.size() &&
                       (std::isdigit(static_cast<unsigned char>(line[j])) || line[j] == '.')) ++j;
                if (j < line.size() && (line[j] == 'e' || line[j] == 'E')) {
                    ++j;
                    if (j < line.size() && (line[j] == '+' || line[j] == '-')) ++j;
                    while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
                }
            }
            while (j < line.size() && (line[j] == 'u' || line[j] == 'U' ||
                                       line[j] == 'f' || line[j] == 'F')) ++j;
            out.push_back(Tok{line.substr(i, j - i), K_NUM});
            i = j;
        } else {
            out.push_back(Tok{std::string(1, c), K_PUNCT});
            ++i;
        }
    }
    return out;
}

// Hex keeps its u suffix; a decimal float drops the C f suffix WGSL rejects.
std::string num(const std::string& t) {
    if (t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) return t;
    if (!t.empty() && (t.back() == 'f' || t.back() == 'F')) return t.substr(0, t.size() - 1);
    return t;
}

const char* type_of(const std::string& t) {
    if (t == "float")  return "f32";
    if (t == "float2") return "vec2<f32>";
    if (t == "float3") return "vec3<f32>";
    if (t == "float4") return "vec4<f32>";
    if (t == "int")    return "i32";
    if (t == "uint")   return "u32";
    if (t == "bool")   return "bool";
    return nullptr;
}

// Indices of tokens that carry meaning -- not whitespace, not comments.
std::vector<size_t> meaningful(const std::vector<Tok>& t) {
    std::vector<size_t> m;
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].kind != K_SPACE && t[i].kind != K_COMMENT) m.push_back(i);
    return m;
}

bool is_punct(const std::vector<Tok>& t, size_t i, const char* p) {
    return t[i].kind == K_PUNCT && t[i].s == p;
}

// The only expression-level rewrites the dialect needs: numeric suffixes,
// functional casts and constructors, and the two bit reinterpretations.
std::string expr(const std::vector<Tok>& t, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to && i < t.size(); ++i) {
        if (t[i].kind == K_NUM) { out += num(t[i].s); continue; }
        if (t[i].kind == K_IDENT) {
            size_t n = i + 1;
            while (n < t.size() && t[n].kind == K_SPACE) ++n;
            const bool called = n < t.size() && is_punct(t, n, "(");
            if (called) {
                const char* ty = type_of(t[i].s);
                if (ty) { out += ty; continue; }
                if (t[i].s == "asuint") { out += "bitcast<u32>"; continue; }
                if (t[i].s == "asint")  { out += "bitcast<i32>"; continue; }
            }
        }
        out += t[i].s;
    }
    return out;
}

std::string indent_of(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.substr(0, i);
}

// TYPE name(TYPE a, TYPE b) {   ->   fn name(a: T, b: T) -> T {
bool function_def(const std::vector<Tok>& t, const std::vector<size_t>& m,
                  const std::string& ind, std::string& out) {
    if (m.size() < 5) return false;
    if (!is_punct(t, m.back(), "{")) return false;
    const char* ret = type_of(t[m[0]].s);
    if (!ret || t[m[1]].kind != K_IDENT || !is_punct(t, m[2], "(")) return false;

    size_t close = 0;
    for (size_t k = m.size(); k-- > 0;) {
        if (is_punct(t, m[k], ")")) { close = k; break; }
    }
    if (close < 2) return false;

    std::string params;
    std::vector<size_t> cur;
    for (size_t k = 3; k < close; ++k) {
        if (is_punct(t, m[k], ",")) {
            if (cur.size() >= 2) {
                const char* pt = type_of(t[cur[0]].s);
                if (!pt) return false;
                if (!params.empty()) params += ", ";
                params += t[cur[1]].s + std::string(": ") + pt;
            }
            cur.clear();
        } else {
            cur.push_back(m[k]);
        }
    }
    if (cur.size() >= 2) {
        const char* pt = type_of(t[cur[0]].s);
        if (!pt) return false;
        if (!params.empty()) params += ", ";
        params += t[cur[1]].s + std::string(": ") + pt;
    }

    out = ind + "fn " + t[m[1]].s + "(" + params + ") -> " + ret + " {";
    return true;
}

// TYPE name[N];   ->   var name: array<T, N>;
bool array_decl(const std::vector<Tok>& t, const std::vector<size_t>& m,
                const std::string& ind, std::string& out) {
    if (m.size() != 6) return false;
    const char* ty = type_of(t[m[0]].s);
    if (!ty || t[m[1]].kind != K_IDENT) return false;
    if (!is_punct(t, m[2], "[") || t[m[3]].kind != K_NUM) return false;
    if (!is_punct(t, m[4], "]") || !is_punct(t, m[5], ";")) return false;
    out = ind + "var " + t[m[1]].s + ": array<" + ty + ", " + num(t[m[3]].s) + ">;";
    return true;
}

// for (TYPE i = a; cond; step) {   ->   for (var i: T = a; cond; step) {
bool for_header(const std::vector<Tok>& t, const std::vector<size_t>& m,
                const std::string& ind, std::string& out) {
    if (m.size() < 8 || t[m[0]].s != "for" || !is_punct(t, m[1], "(")) return false;
    const char* ty = type_of(t[m[2]].s);
    if (!ty || t[m[3]].kind != K_IDENT || !is_punct(t, m[4], "=")) return false;
    size_t close = 0;
    for (size_t k = m.size(); k-- > 0;) {
        if (is_punct(t, m[k], ")")) { close = k; break; }
    }
    if (close < 5) return false;
    const std::string rest = expr(t, m[5], m[close]);
    out = ind + "for (var " + t[m[3]].s + ": " + ty + " = " + rest + ") {";
    return true;
}

// [const] TYPE name = expr;   ->   let|var name: T = expr;
bool var_decl(const std::vector<Tok>& t, const std::vector<size_t>& m,
              const std::string& ind, std::string& out) {
    size_t k = 0;
    bool immutable = false;
    if (m.size() > 0 && t[m[0]].kind == K_IDENT && t[m[0]].s == "const") { immutable = true; k = 1; }
    if (m.size() < k + 4) return false;
    const char* ty = type_of(t[m[k]].s);
    if (!ty || t[m[k + 1]].kind != K_IDENT || !is_punct(t, m[k + 2], "=")) return false;
    if (!is_punct(t, m.back(), ";")) return false;
    const std::string rhs = expr(t, m[k + 3], m.back());
    out = ind + (immutable ? "let " : "var ") + t[m[k + 1]].s + ": " + ty + " = " + rhs + ";";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: wgslgen <input.h> <output.wgsl>\n");
        return 2;
    }
    std::FILE* in = std::fopen(argv[1], "rb");
    if (!in) { std::printf("wgslgen: cannot open %s\n", argv[1]); return 1; }
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) text.append(buf, n);
    std::fclose(in);

    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') { lines.push_back(cur); cur.clear(); }
        else if (text[i] != '\r') cur += text[i];
    }
    if (!cur.empty()) lines.push_back(cur);

    std::string out =
        "// GENERATED by tools/wgslgen from " + std::string(argv[1]) + " -- do not edit.\n"
        "// The dialect source is canonical; this file is a build product.\n\n";

    bool seen_code = false;
    int emitted = 0, passthrough = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        const std::vector<Tok> t = lex(line);
        const std::vector<size_t> m = meaningful(t);

        // Preprocessor has no WGSL equivalent; the generated file needs none.
        std::string trimmed = line;
        size_t a = trimmed.find_first_not_of(" \t");
        if (a != std::string::npos && trimmed[a] == '#') continue;

        if (m.empty()) {
            // Drop the dialect file's own header block; keep later comments.
            if (seen_code) out += line + "\n";
            continue;
        }
        seen_code = true;

        const std::string ind = indent_of(line);
        std::string rendered;
        if (function_def(t, m, ind, rendered) ||
            array_decl(t, m, ind, rendered) ||
            for_header(t, m, ind, rendered) ||
            var_decl(t, m, ind, rendered)) {
            ++emitted;
        } else {
            rendered = ind + expr(t, m.front(), t.size());
            ++passthrough;
        }
        out += rendered + "\n";
    }

    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::printf("wgslgen: cannot write %s\n", argv[2]); return 1; }
    std::fwrite(out.data(), 1, out.size(), o);
    std::fclose(o);
    std::printf("wgslgen: %s -> %s (%d declarations rewritten, %d lines passed through)\n",
                argv[1], argv[2], emitted, passthrough);
    return 0;
}
