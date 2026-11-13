#ifndef MONITOR_BOX_RULES_RULE_BINDINGS_H_
#define MONITOR_BOX_RULES_RULE_BINDINGS_H_

#include <string>
#include <vector>

#include "common/json.h"

namespace mb {

// 功能挂在哪些区域 / 进出线上。
//
//   新写法 (推荐): regions.<id>.rules.<功能> = {覆盖参数}
//                 lines.<id>.rules.<功能>   = {覆盖参数}
//   旧写法 (兼容): features.<功能>.region_id (单个) / region_ids (数组) / line_ids (数组)
//
// 两种写法取并集, 同一个目标只算一次。每个挂载点的有效配置 = features.<功能> 深合并
// 该挂载点的覆盖参数 (对象逐层合并, 数组/数值/字符串整体替换), 所以 features.<功能>
// 是总开关 + 默认参数, 挂载点只写和默认不同的部分。
// 覆盖参数里写 "enabled": false 表示这一处暂时不用; 写 true 不能绕过总开关。
// rule / model 和引用字段 (region_id / region_ids / line_ids) 不能在挂载点覆盖。
enum class BindKind { None, Region, Line };

struct RuleBinding {
    std::string target;  // 区域 / 线 id
    Json params;         // 合并后的功能配置
};

// 功能按什么挂: rule=line 挂线; rule=zone/homography 挂区域; rule 为空的旧配置看引用字段。
BindKind binding_kind(const Json& cfg, const std::string& key);

// 功能的全部挂载点 (按 旧写法 -> regions/lines 的配置顺序)。BindKind::None 返回空。
std::vector<RuleBinding> feature_bindings(const Json& cfg, const std::string& key);

// 对象逐层合并, 其余类型 over 整体替换 base。
Json merge_json(const Json& base, const Json& over);

// regions / lines 里的 rules 是否都指向存在、且挂载类型对得上的功能。
bool check_rule_targets(const Json& cfg, std::string* err);

}  // namespace mb

#endif  // MONITOR_BOX_RULES_RULE_BINDINGS_H_
