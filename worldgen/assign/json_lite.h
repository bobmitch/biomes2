// Minimal JSON reader. Deliberately dependency-free: worldgen/ must not acquire
// host deps (CLAUDE.md invariant 1). Covers exactly what archetypes.json needs.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>

namespace jsonlite {

struct Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

struct Value {
    enum Type { Null, Bool, Number, String, Arr, Obj };
    Type        type = Null;
    bool        b    = false;
    double      num  = 0.0;
    std::string str;
    Array       arr;
    Object      obj;

    bool has(const std::string& k) const { return type == Obj && obj.count(k) != 0; }

    const Value& at(const std::string& k) const {
        auto it = obj.find(k);
        if (it == obj.end()) throw std::runtime_error("json: missing key '" + k + "'");
        return it->second;
    }
    double number(const std::string& k) const {
        const Value& v = at(k);
        if (v.type != Number) throw std::runtime_error("json: '" + k + "' is not a number");
        return v.num;
    }
    const std::string& string(const std::string& k) const {
        const Value& v = at(k);
        if (v.type != String) throw std::runtime_error("json: '" + k + "' is not a string");
        return v.str;
    }
    bool boolean(const std::string& k) const {
        const Value& v = at(k);
        if (v.type != Bool) throw std::runtime_error("json: '" + k + "' is not a bool");
        return v.b;
    }
    const Array& array(const std::string& k) const {
        const Value& v = at(k);
        if (v.type != Arr) throw std::runtime_error("json: '" + k + "' is not an array");
        return v.arr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text), i_(0) {}

    Value parse() {
        skip();
        Value v = value();
        skip();
        if (i_ != s_.size()) fail("trailing characters after top-level value");
        return v;
    }

private:
    const std::string& s_;
    size_t i_;

    [[noreturn]] void fail(const std::string& why) const {
        size_t line = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); ++k) if (s_[k] == '\n') ++line;
        throw std::runtime_error("json: " + why + " at line " + std::to_string(line));
    }

    void skip() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i_; continue; }
            break;
        }
    }

    bool lit(const char* word) {
        size_t n = 0; while (word[n]) ++n;
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n; return true;
    }

    Value value() {
        if (i_ >= s_.size()) fail("unexpected end of input");
        char c = s_[i_];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') { Value v; v.type = Value::String; v.str = str(); return v; }
        if (lit("true"))  { Value v; v.type = Value::Bool; v.b = true;  return v; }
        if (lit("false")) { Value v; v.type = Value::Bool; v.b = false; return v; }
        if (lit("null"))  { return Value{}; }
        return number();
    }

    Value object() {
        Value v; v.type = Value::Obj;
        ++i_; skip();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
        for (;;) {
            skip();
            if (i_ >= s_.size() || s_[i_] != '"') fail("expected a key string");
            std::string key = str();
            skip();
            if (i_ >= s_.size() || s_[i_] != ':') fail("expected ':' after key");
            ++i_; skip();
            v.obj[key] = value();
            skip();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
            fail("expected ',' or '}' in object");
        }
    }

    Value array() {
        Value v; v.type = Value::Arr;
        ++i_; skip();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
        for (;;) {
            skip();
            v.arr.push_back(value());
            skip();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
            fail("expected ',' or ']' in array");
        }
    }

    std::string str() {
        ++i_;  // opening quote
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_];
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) fail("unterminated escape");
                char e = s_[i_];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '/': out += '/';  break;
                    case '"': out += '"';  break;
                    case '\\': out += '\\'; break;
                    default: fail("unsupported escape sequence");
                }
                ++i_;
            } else {
                out += c; ++i_;
            }
        }
        if (i_ >= s_.size()) fail("unterminated string");
        ++i_;  // closing quote
        return out;
    }

    Value number() {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        bool any = false;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' ||
                                  s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) {
            if (s_[i_] >= '0' && s_[i_] <= '9') any = true;
            ++i_;
        }
        if (!any) fail("expected a value");
        Value v; v.type = Value::Number;
        v.num = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return v;
    }
};

inline Value parse_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("json: cannot open '" + path + "'");
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    Parser p(text);
    return p.parse();
}

}  // namespace jsonlite
