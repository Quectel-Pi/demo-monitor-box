#ifndef MONITOR_BOX_COMMON_JSON_H_
#define MONITOR_BOX_COMMON_JSON_H_

#include <string>
#include <utility>
#include <vector>

namespace mb {

// 极简 JSON。sysroot 里没有 nlohmann/json, 为几 KB 的配置引一个库不划算,
// 所以自己写一个够用的递归下降解析器 (只认标准 JSON, 不支持注释)。
// 限制: 嵌套深度上限 64; 数字统一存 double。
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_number() const { return type_ == Type::Number; }

    // ---- 解析 ----
    static bool parse(const std::string& text, Json* out, std::string* err);
    static bool parse_file(const std::string& path, Json* out, std::string* err);

    // ---- 读取 (带缺省值, 配置项都用这个) ----
    bool has(const std::string& key) const;
    double number(double def) const;
    int integer(int def) const;
    bool boolean(bool def) const;
    std::string str(const std::string& def) const;

    // 只读访问: 不存在时返回 Null 单例, 可以安全地长链兜底 cfg["a"]["b"]["c"]
    const Json& operator[](const std::string& key) const;
    const Json& operator[](size_t i) const;
    size_t size() const;

    // "video.width" 这种点号路径, 少写几层嵌套
    const Json& path(const std::string& dotted) const;

    // 遍历: object 保插入序, array 保原序。遍历 regions / features 这类
    // 键名事先不知道的配置段时用。
    const std::vector<std::pair<std::string, Json>>& members() const { return kv_; }
    const std::vector<Json>& items() const { return items_; }

    // ---- 构造 (拼事件 JSON 用) ----
    static Json object();
    static Json array();
    static Json number_value(double v);
    static Json string_value(std::string v);
    static Json bool_value(bool v);

    Json& set(const std::string& key, Json v);
    void push(Json v);

    std::string dump(int indent = 0) const;

private:
    // 序列化。json.cpp 里实现, 递归下去, level 控制缩进层级。
    void dump_impl(std::string* out, int indent, int level) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> items_;                       // array 元素
    std::vector<std::pair<std::string, Json>> kv_;  // object 成员 (保插入序)
};

}  // namespace mb

#endif  // MONITOR_BOX_COMMON_JSON_H_
