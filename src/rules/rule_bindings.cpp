#include "rules/rule_bindings.h"

namespace mb {

namespace {

bool fixed_key(const std::string& k) {
    return k == "rule" || k == "model" || k == "region_id" || k == "region_ids" || k == "line_ids";
}

bool any_item_uses(const Json& items, const std::string& key) {
    for (const auto& kv : items.members())
        if (kv.second["rules"].has(key)) return true;
    return false;
}

void add_target(std::vector<std::string>* out, const std::string& id) {
    if (id.empty()) return;
    for (const std::string& x : *out)
        if (x == id) return;
    out->push_back(id);
}

}  // namespace

Json merge_json(const Json& base, const Json& over) {
    if (!base.is_object() || !over.is_object()) return over;
    Json out = base;
    for (const auto& kv : over.members()) {
        const Json& cur = base[kv.first];
        out.set(kv.first, cur.is_object() && kv.second.is_object() ? merge_json(cur, kv.second)
                                                                   : kv.second);
    }
    return out;
}

BindKind binding_kind(const Json& cfg, const std::string& key) {
    const Json& f = cfg["features"][key];
    const std::string rule = f["rule"].str("");
    if (rule == "line") return BindKind::Line;
    if (rule == "zone" || rule == "homography") return BindKind::Region;
    if (!rule.empty()) return BindKind::None;
    if (!f["region_id"].str("").empty() || f["region_ids"].size() > 0 ||
        any_item_uses(cfg["regions"], key))
        return BindKind::Region;
    if (f["line_ids"].size() > 0 || any_item_uses(cfg["lines"], key)) return BindKind::Line;
    return BindKind::None;
}

std::vector<RuleBinding> feature_bindings(const Json& cfg, const std::string& key) {
    std::vector<RuleBinding> out;
    const BindKind kind = binding_kind(cfg, key);
    if (kind == BindKind::None) return out;
    const Json& f = cfg["features"][key];
    const Json& items = cfg[kind == BindKind::Line ? "lines" : "regions"];

    std::vector<std::string> ids;
    if (kind == BindKind::Region) {
        add_target(&ids, f["region_id"].str(""));
        const Json& rids = f["region_ids"];
        for (size_t i = 0; rids.is_array() && i < rids.size(); ++i) add_target(&ids, rids[i].str(""));
    } else {
        const Json& lids = f["line_ids"];
        for (size_t i = 0; lids.is_array() && i < lids.size(); ++i) add_target(&ids, lids[i].str(""));
    }
    for (const auto& kv : items.members())
        if (kv.second["rules"].has(key)) add_target(&ids, kv.first);

    const bool global_on = f["enabled"].boolean(false);
    for (const std::string& id : ids) {
        const Json& over = items[id]["rules"][key];
        if (over.is_object() && !over["enabled"].boolean(true)) continue;
        Json clean = Json::object();
        for (const auto& kv : over.members())
            if (!fixed_key(kv.first) && kv.first != "enabled") clean.set(kv.first, kv.second);
        RuleBinding b;
        b.target = id;
        b.params = merge_json(f.is_object() ? f : Json::object(), clean);
        b.params.set("enabled", Json::bool_value(global_on));
        out.push_back(b);
    }
    return out;
}

bool check_rule_targets(const Json& cfg, std::string* err) {
    struct Group {
        const char* section;
        const char* what;
        BindKind kind;
    };
    for (const Group& g : {Group{"regions", "区域", BindKind::Region},
                           Group{"lines", "进出线", BindKind::Line}}) {
        for (const auto& item : cfg[g.section].members()) {
            const Json& rules = item.second["rules"];
            if (rules.is_null()) continue;
            const std::string where = std::string(g.section) + "." + item.first + ".rules";
            if (!rules.is_object()) {
                if (err) *err = where + " 要写成 {功能: {参数}}";
                return false;
            }
            for (const auto& r : rules.members()) {
                if (!r.second.is_object() && !r.second.is_null()) {
                    if (err) *err = where + "." + r.first + " 要写成对象 (可以是 {})";
                    return false;
                }
                if (!cfg["features"].has(r.first)) {
                    if (err) *err = where + " 引用了不存在的功能: " + r.first;
                    return false;
                }
                if (binding_kind(cfg, r.first) != g.kind) {
                    if (err) *err = r.first + " 不能挂在" + g.what + "上 (" + where + ")";
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace mb
