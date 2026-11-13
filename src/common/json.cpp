#include "common/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mb {
namespace {

constexpr int kMaxDepth = 64;

const Json& null_json() {
    static const Json kNull;
    return kNull;
}

void skip_ws(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
}

void append_utf8(std::string* s, unsigned cp) {
    if (cp < 0x80) {
        s->push_back((char)cp);
    } else if (cp < 0x800) {
        s->push_back((char)(0xC0 | (cp >> 6)));
        s->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s->push_back((char)(0xE0 | (cp >> 12)));
        s->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        s->push_back((char)(0xF0 | (cp >> 18)));
        s->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        s->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_string(const char*& p, const char* end, std::string* out, std::string* err) {
    if (p >= end || *p != '"') {
        *err = "期望字符串";
        return false;
    }
    ++p;
    out->clear();
    while (p < end) {
        const char c = *p++;
        if (c == '"') return true;
        if (c != '\\') {
            out->push_back(c);
            continue;
        }
        if (p >= end) break;
        const char e = *p++;
        switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                if (end - p < 4) {
                    *err = "\\u 转义不完整";
                    return false;
                }
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const int v = hex_val(p[i]);
                    if (v < 0) {
                        *err = "\\u 转义里有非十六进制字符";
                        return false;
                    }
                    cp = (cp << 4) | (unsigned)v;
                }
                p += 4;
                // 代理对: 高代理后面必须跟 \uDCxx
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = 0;
                    bool ok = true;
                    for (int i = 0; i < 4; ++i) {
                        const int v = hex_val(p[2 + i]);
                        if (v < 0) { ok = false; break; }
                        lo = (lo << 4) | (unsigned)v;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                append_utf8(out, cp);
                break;
            }
            default:
                *err = std::string("未知转义 \\") + e;
                return false;
        }
    }
    *err = "字符串没有闭合";
    return false;
}

bool parse_value(const char*& p, const char* end, Json* out, std::string* err, int depth) {
    if (depth > kMaxDepth) {
        *err = "JSON 嵌套过深";
        return false;
    }
    skip_ws(p, end);
    if (p >= end) {
        *err = "JSON 提前结束";
        return false;
    }

    const char c = *p;
    if (c == '{') {
        ++p;
        Json obj = Json::object();
        skip_ws(p, end);
        if (p < end && *p == '}') {
            ++p;
            *out = obj;
            return true;
        }
        for (;;) {
            skip_ws(p, end);
            std::string key;
            if (!parse_string(p, end, &key, err)) return false;
            skip_ws(p, end);
            if (p >= end || *p != ':') {
                *err = "对象成员缺少 ':'";
                return false;
            }
            ++p;
            Json v;
            if (!parse_value(p, end, &v, err, depth + 1)) return false;
            obj.set(key, v);
            skip_ws(p, end);
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == '}') {
                ++p;
                *out = obj;
                return true;
            }
            *err = "对象缺少 ',' 或 '}'";
            return false;
        }
    }

    if (c == '[') {
        ++p;
        Json arr = Json::array();
        skip_ws(p, end);
        if (p < end && *p == ']') {
            ++p;
            *out = arr;
            return true;
        }
        for (;;) {
            Json v;
            if (!parse_value(p, end, &v, err, depth + 1)) return false;
            arr.push(v);
            skip_ws(p, end);
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == ']') {
                ++p;
                *out = arr;
                return true;
            }
            *err = "数组缺少 ',' 或 ']'";
            return false;
        }
    }

    if (c == '"') {
        std::string s;
        if (!parse_string(p, end, &s, err)) return false;
        *out = Json::string_value(std::move(s));
        return true;
    }

    if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
        p += 4;
        *out = Json::bool_value(true);
        return true;
    }
    if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
        p += 5;
        *out = Json::bool_value(false);
        return true;
    }
    if (end - p >= 4 && strncmp(p, "null", 4) == 0) {
        p += 4;
        *out = Json();
        return true;
    }

    // 数字: 交给 strtod, 它自己会吃掉合法前缀
    char* stop = nullptr;
    const double v = strtod(p, &stop);
    if (stop == p) {
        *err = std::string("无法解析的值, 起于 '") + std::string(p, (size_t)std::min<long>(8, end - p)) +
               "'";
        return false;
    }
    p = stop;
    *out = Json::number_value(v);
    return true;
}

void escape_to(std::string* out, const std::string& s) {
    out->push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)c);
                    out->append(buf);
                } else {
                    out->push_back(c);
                }
        }
    }
    out->push_back('"');
}

}  // namespace

// ------------------------------------------------------------------ 解析

bool Json::parse(const std::string& text, Json* out, std::string* err) {
    std::string e;
    const char* p = text.data();
    const char* end = text.data() + text.size();
    Json v;
    if (!parse_value(p, end, &v, &e, 0)) {
        if (err) *err = e;
        return false;
    }
    skip_ws(p, end);
    if (p != end) {
        if (err) *err = "JSON 尾部有多余内容";
        return false;
    }
    *out = std::move(v);
    return true;
}

bool Json::parse_file(const std::string& path, Json* out, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "打开失败: " + path;
        return false;
    }
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return parse(text, out, err);
}

// ------------------------------------------------------------------ 读取

bool Json::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& kv : kv_) {
        if (kv.first == key) return true;
    }
    return false;
}

double Json::number(double def) const { return type_ == Type::Number ? num_ : def; }

int Json::integer(int def) const { return type_ == Type::Number ? (int)std::lround(num_) : def; }

bool Json::boolean(bool def) const {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return num_ != 0.0;
    return def;
}

std::string Json::str(const std::string& def) const {
    return type_ == Type::String ? str_ : def;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ == Type::Object) {
        for (const auto& kv : kv_) {
            if (kv.first == key) return kv.second;
        }
    }
    return null_json();
}

const Json& Json::operator[](size_t i) const {
    if (type_ == Type::Array && i < items_.size()) return items_[i];
    return null_json();
}

size_t Json::size() const {
    if (type_ == Type::Array) return items_.size();
    if (type_ == Type::Object) return kv_.size();
    return 0;
}

const Json& Json::path(const std::string& dotted) const {
    const Json* cur = this;
    size_t start = 0;
    while (start <= dotted.size()) {
        const size_t dot = dotted.find('.', start);
        const std::string seg =
            dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        cur = &(*cur)[seg];
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return *cur;
}

// ------------------------------------------------------------------ 构造

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

Json Json::number_value(double v) {
    Json j;
    j.type_ = Type::Number;
    j.num_ = v;
    return j;
}

Json Json::string_value(std::string v) {
    Json j;
    j.type_ = Type::String;
    j.str_ = std::move(v);
    return j;
}

Json Json::bool_value(bool v) {
    Json j;
    j.type_ = Type::Bool;
    j.bool_ = v;
    return j;
}

Json& Json::set(const std::string& key, Json v) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        items_.clear();
        str_.clear();
    }
    for (auto& kv : kv_) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return *this;
        }
    }
    kv_.emplace_back(key, std::move(v));
    return *this;
}

void Json::push(Json v) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        kv_.clear();
    }
    items_.push_back(std::move(v));
}

// ------------------------------------------------------------------ 输出

void Json::dump_impl(std::string* out, int indent, int level) const {
    const std::string pad(indent > 0 ? (size_t)(indent * level) : 0, ' ');
    const std::string pad_in(indent > 0 ? (size_t)(indent * (level + 1)) : 0, ' ');
    const char* nl = indent > 0 ? "\n" : "";

    switch (type_) {
        case Type::Null: out->append("null"); break;
        case Type::Bool: out->append(bool_ ? "true" : "false"); break;
        case Type::Number: {
            char buf[40];
            // 整数按整数打, 免得配置回写时冒出 "0.0000000001" 这种噪声
            if (num_ == (double)(long long)num_ && std::fabs(num_) < 1e15) {
                snprintf(buf, sizeof(buf), "%lld", (long long)num_);
            } else {
                snprintf(buf, sizeof(buf), "%.10g", num_);
            }
            out->append(buf);
            break;
        }
        case Type::String: escape_to(out, str_); break;
        case Type::Array: {
            if (items_.empty()) {
                out->append("[]");
                break;
            }
            out->append("[");
            out->append(nl);
            for (size_t i = 0; i < items_.size(); ++i) {
                out->append(pad_in);
                items_[i].dump_impl(out, indent, level + 1);
                if (i + 1 < items_.size()) out->append(",");
                out->append(nl);
            }
            out->append(pad);
            out->append("]");
            break;
        }
        case Type::Object: {
            if (kv_.empty()) {
                out->append("{}");
                break;
            }
            out->append("{");
            out->append(nl);
            for (size_t i = 0; i < kv_.size(); ++i) {
                out->append(pad_in);
                escape_to(out, kv_[i].first);
                out->append(indent > 0 ? ": " : ":");
                kv_[i].second.dump_impl(out, indent, level + 1);
                if (i + 1 < kv_.size()) out->append(",");
                out->append(nl);
            }
            out->append(pad);
            out->append("}");
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_impl(&out, indent, 0);
    return out;
}

}  // namespace mb
