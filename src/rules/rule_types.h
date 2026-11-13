#ifndef MONITOR_BOX_RULES_RULE_TYPES_H_
#define MONITOR_BOX_RULES_RULE_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "common/geometry.h"

namespace mb {

// 事件等级。Info 只进日志和网页事件列表; Alarm 还要走去抖/冷却/抓拍。
enum class Severity { Info, Alarm };

// 规则引擎吐出的一条事件。九个功能都走这一个结构, 只是 feature/kind 不同。
struct RuleEvent {
    std::string feature;   // "intrusion" / "fence" / "dwell" / "count" / "locate" / "fall" ...
    std::string kind;      // "enter" / "exit" / "cross" / "stay" / "position" / "direction"
    std::string rule_id;   // region_id 或 line_id
    std::string name;      // region/line 的中文名, 给网页直接用
    int track_id = -1;
    int cls_id = -1;
    std::string label;
    uint64_t ts_ms = 0;
    Severity severity = Severity::Alarm;

    float x = 0.0f;  // 事件位置的图像坐标 (脚点)
    float y = 0.0f;
    float gx = 0.0f;  // 地面坐标 (米), 只有 locate 有
    float gy = 0.0f;
    bool has_ground = false;
    float heading_deg = 0.0f;  // 运动方向 (度, 0=画面右, 顺时针), 只有 locate 有
    std::string extra;         // 人话补充, 如 "由左向右"

    // 滞留会话号 (按目标滞留才有, >0): 同一个人从进区到离开为一个会话, ID 切换时沿用。
    // 事件引擎/滞留联动靠它把 stay / stay_long / stay_end 和录像串到一起。
    int session = 0;
    float duration_sec = 0.0f;  // 滞留已持续 (stay_end 为总时长)
};

// 带名字的多边形 (归一化 0..1 坐标, 使用时按帧尺寸还原)。
struct NamedPolygon {
    std::string id;
    std::string name;
    // 区域用途: "private" 自家区域 (花园/车位) / "public" 公共区域 (马路/人行道) /
    // "ignore" 忽略区 (树、邻居窗户: 脚点落在里面的目标不参与任何规则) /
    // "ground" 地面标定 / "" 未标注 (旧配置)。只有 ignore 改变规则行为, 其余用于显示和 APP 推荐。
    std::string role;
    std::vector<Point> points;  // 归一化
    bool has_real_size = false;
    float real_w_m = 0.0f;  // 对应 points[0..3] 组成的地面矩形宽 (米)
    float real_h_m = 0.0f;
};

// 带名字的线段 (归一化)。
struct NamedLine {
    std::string id;
    std::string name;
    Point p1;
    Point p2;
};

}  // namespace mb

#endif  // MONITOR_BOX_RULES_RULE_TYPES_H_
