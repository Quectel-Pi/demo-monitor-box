#ifndef MONITOR_BOX_VIDEO_ALARM_OSD_H_
#define MONITOR_BOX_VIDEO_ALARM_OSD_H_

#include <algorithm>
#include <cstdint>
#include <vector>

#include "image_drawing.h"
#include "infer/types.h"
#include "rules/rule_types.h"

namespace mb {

class AlarmHighlights {
public:
    void clear() { sources_.clear(); }

    void update(const std::vector<RuleEvent>& sources, uint64_t now_ms) {
        sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                     [now_ms](const Source& s) { return now_ms >= s.until; }),
                       sources_.end());
        for (const RuleEvent& ev : sources) {
            auto it = std::find_if(sources_.begin(), sources_.end(), [&ev](const Source& s) {
                return s.ev.feature == ev.feature && s.ev.kind == ev.kind &&
                       s.ev.rule_id == ev.rule_id && s.ev.track_id == ev.track_id;
            });
            if (it == sources_.end()) sources_.push_back({ev, now_ms + 3000});
            else {
                it->ev = ev;
                it->until = now_ms + 3000;
            }
        }
    }

    bool track(int id) const {
        return id >= 0 && std::any_of(sources_.begin(), sources_.end(), [id](const Source& s) {
            return s.ev.feature != "fire" && s.ev.track_id == id;
        });
    }

    bool region(const std::string& id) const {
        return std::any_of(sources_.begin(), sources_.end(), [&id](const Source& s) {
            return s.ev.track_id < 0 && s.ev.feature != "fire" && s.ev.rule_id == id;
        });
    }

    bool fire(size_t index, const std::vector<Detection>& detections) const {
        // 火焰模型没有 track id, 只高亮包含事件位置且中心最近的同类框。
        for (const Source& s : sources_) {
            if (s.ev.feature != "fire") continue;
            size_t best = detections.size();
            float best_dist = 0;
            for (size_t i = 0; i < detections.size(); ++i) {
                const Detection& d = detections[i];
                if (d.cls_id != s.ev.cls_id || s.ev.x < d.x1 || s.ev.x > d.x2 ||
                    s.ev.y < d.y1 || s.ev.y > d.y2) continue;
                const float dx = (d.x1 + d.x2) * 0.5f - s.ev.x;
                const float dy = (d.y1 + d.y2) * 0.5f - s.ev.y;
                const float dist = dx * dx + dy * dy;
                if (best == detections.size() || dist < best_dist) {
                    best = i;
                    best_dist = dist;
                }
            }
            if (best == index && best < detections.size()) return true;
        }
        return false;
    }

private:
    struct Source {
        RuleEvent ev;
        uint64_t until;
    };
    std::vector<Source> sources_;
};

inline int alarm_thickness(const image_buffer_t* image) {
    return std::clamp(std::min(image->width, image->height) / 180, 4, 8);
}

inline void draw_alarm_box(image_buffer_t* image, int x, int y, int w, int h) {
    const int thickness = alarm_thickness(image);
    draw_rectangle(image, x, y, w, h, COLOR_BLACK, thickness + 8);
    draw_rectangle(image, x, y, w, h, COLOR_WHITE, thickness + 4);
    draw_rectangle(image, x, y, w, h, COLOR_RED, thickness);
}

}  // namespace mb

#endif  // MONITOR_BOX_VIDEO_ALARM_OSD_H_
