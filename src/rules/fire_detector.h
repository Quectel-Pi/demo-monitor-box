#ifndef MONITOR_BOX_RULES_FIRE_DETECTOR_H_
#define MONITOR_BOX_RULES_FIRE_DETECTOR_H_

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "infer/types.h"
#include "rules/rule_types.h"

namespace mb {

// 火焰/烟雾告警参数。阈值思路来自 z11hzy/yolov8-fire-smoke-detection:
// 火焰和烟雾分开设置信度与确认时长 (烟雾更容易和雾气/蒸汽/灰墙混, 要求更高更久)。
struct FireKindParams {
    bool enabled = true;
    float conf = 0.5f;          // 单框置信度门槛
    float confirm_sec = 1.0f;   // 从首次命中起持续这么久才报
};

struct FireParams {
    FireKindParams fire{true, 0.50f, 1.0f};
    FireKindParams smoke{true, 0.60f, 3.0f};
    float min_area_ratio = 0.0002f;  // 框面积 / 画面面积, 更小的当噪声 (远处路灯/反光)
    float hit_ratio = 0.6f;          // 确认窗口内命中推理帧的占比下限 (时序投票)
    int min_hits = 2;                // 确认窗口内至少命中几次推理
    float gap_sec = 1.5f;            // 连续漏检超过这个时长, 持续计时从头算
    float clear_sec = 10.0f;         // 告警后连续这么久没再命中, 解除并重新布防
};

// 火焰烟雾时序确认。模型本身只看单帧, 单帧误报 (夕阳、红衣服、车灯) 很常见,
// 这里按推理帧做 "持续 + 投票" 两道过滤:
//
//   IDLE   --命中--> 记 start
//          持续 >= confirm_sec 且窗口 [now-confirm_sec, now] 内命中占比 >= hit_ratio
//          且命中次数 >= min_hits --> 报一条 Alarm (feature=fire, kind=fire|smoke), 进 ALARM
//          中途漏检超过 gap_sec --> start 清零
//   ALARM  --clear_sec 内没再命中--> 报一条 Info (kind=clear), 回 IDLE
//
// fire / smoke 两路状态独立。只在拿到新的火焰模型结果时 update (不是每采集帧),
// 所以 confirm_sec 是按推理结果的时间戳算的, 和 models.fire.fps 无关。
class FireDetector {
public:
    FireDetector() = default;
    // class_names: 火焰模型的类别名 (按类号), 按名字找 "fire"/"smoke", 不依赖类号顺序。
    FireDetector(const FireParams& p, const std::vector<std::string>& class_names);

    void reset();

    // dets: 本次火焰模型结果 (原图坐标); 返回本次产生的事件。
    std::vector<RuleEvent> update(const std::vector<Detection>& dets, int img_w, int img_h,
                                  uint64_t ts_ms);

    // 通过了置信度/面积过滤的框, 给 OSD 画。
    const std::vector<Detection>& visible() const { return visible_; }
    bool fire_active() const { return kinds_[0].alarmed; }
    bool smoke_active() const { return kinds_[1].alarmed; }
    // 本次检测里是否看到了 (未必已确认)
    bool fire_seen() const { return kinds_[0].seen; }
    bool smoke_seen() const { return kinds_[1].seen; }
    int fire_cls() const { return kinds_[0].cls_id; }
    int smoke_cls() const { return kinds_[1].cls_id; }
    const FireParams& params() const { return params_; }

private:
    struct KindState {
        std::string name;  // "fire" / "smoke"
        std::string zh;
        int cls_id = -1;
        FireKindParams p;
        std::deque<std::pair<uint64_t, bool>> history;  // (ts, 是否命中)
        uint64_t start_ms = 0;
        uint64_t last_hit_ms = 0;
        bool has_start = false;
        bool alarmed = false;
        bool seen = false;
    };

    void step(KindState* k, const Detection* best, int img_w, int img_h, uint64_t ts_ms,
              std::vector<RuleEvent>* out);

    FireParams params_;
    KindState kinds_[2];
    std::vector<Detection> visible_;
};

}  // namespace mb

#endif  // MONITOR_BOX_RULES_FIRE_DETECTOR_H_
