#include "rules/fire_detector.h"

#include <algorithm>
#include <cstdio>

namespace mb {

FireDetector::FireDetector(const FireParams& p, const std::vector<std::string>& class_names)
    : params_(p) {
    kinds_[0].name = "fire";
    kinds_[0].zh = "火焰";
    kinds_[0].p = p.fire;
    kinds_[1].name = "smoke";
    kinds_[1].zh = "烟雾";
    kinds_[1].p = p.smoke;
    for (size_t i = 0; i < class_names.size(); ++i) {
        for (KindState& k : kinds_) {
            if (class_names[i] == k.name) k.cls_id = (int)i;
        }
    }
}

void FireDetector::reset() {
    for (KindState& k : kinds_) {
        k.history.clear();
        k.start_ms = k.last_hit_ms = 0;
        k.has_start = k.alarmed = k.seen = false;
    }
    visible_.clear();
}

std::vector<RuleEvent> FireDetector::update(const std::vector<Detection>& dets, int img_w,
                                            int img_h, uint64_t ts_ms) {
    std::vector<RuleEvent> out;
    visible_.clear();
    const float img_area = (float)img_w * (float)img_h;
    const Detection* best[2] = {nullptr, nullptr};
    for (const Detection& d : dets) {
        for (int i = 0; i < 2; ++i) {
            const KindState& k = kinds_[i];
            if (k.cls_id < 0 || d.cls_id != k.cls_id || !k.p.enabled) continue;
            if (d.score < k.p.conf) continue;
            const float area = (d.x2 - d.x1) * (d.y2 - d.y1);
            if (img_area > 0.f && area < params_.min_area_ratio * img_area) continue;
            visible_.push_back(d);
            if (!best[i] || d.score > best[i]->score) best[i] = &d;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (kinds_[i].cls_id < 0 || !kinds_[i].p.enabled) continue;
        step(&kinds_[i], best[i], img_w, img_h, ts_ms, &out);
    }
    return out;
}

void FireDetector::step(KindState* k, const Detection* best, int img_w, int img_h, uint64_t ts_ms,
                        std::vector<RuleEvent>* out) {
    (void)img_w;
    (void)img_h;
    const bool hit = best != nullptr;
    k->seen = hit;
    const uint64_t confirm_ms = (uint64_t)(k->p.confirm_sec * 1000.0f);
    const uint64_t gap_ms = (uint64_t)(params_.gap_sec * 1000.0f);
    const uint64_t clear_ms = (uint64_t)(params_.clear_sec * 1000.0f);

    k->history.emplace_back(ts_ms, hit);
    while (!k->history.empty() && k->history.front().first + confirm_ms < ts_ms) {
        k->history.pop_front();
    }

    if (hit) {
        if (!k->has_start || ts_ms > k->last_hit_ms + gap_ms) {
            k->start_ms = ts_ms;
            k->has_start = true;
        }
        k->last_hit_ms = ts_ms;
    } else if (k->has_start && ts_ms > k->last_hit_ms + gap_ms) {
        k->has_start = false;
    }

    if (k->alarmed) {
        if (!hit && ts_ms >= k->last_hit_ms + clear_ms) {
            k->alarmed = false;
            k->has_start = false;
            RuleEvent ev;
            ev.feature = "fire";
            ev.kind = "clear";
            ev.rule_id = k->name;
            ev.name = k->zh + "解除";
            ev.label = k->name;
            ev.cls_id = k->cls_id;
            ev.ts_ms = ts_ms;
            ev.severity = Severity::Info;
            char b[96];
            snprintf(b, sizeof(b), "%.0fs 未再检出", (ts_ms - k->last_hit_ms) / 1000.0);
            ev.extra = b;
            out->push_back(ev);
        }
        return;
    }
    if (!hit || !k->has_start || ts_ms < k->start_ms + confirm_ms) return;

    int hits = 0;
    for (const auto& h : k->history) hits += h.second ? 1 : 0;
    const float ratio = k->history.empty() ? 0.f : (float)hits / (float)k->history.size();
    // confirm_sec 很短时窗口里可能只有一两帧, min_hits 不能超过窗口能装下的帧数
    const int need = std::min<int>(params_.min_hits, (int)k->history.size());
    if (hits < need || ratio < params_.hit_ratio) return;

    k->alarmed = true;
    RuleEvent ev;
    ev.feature = "fire";
    ev.kind = k->name;
    ev.rule_id = k->name;
    ev.name = k->zh;
    ev.label = k->name;
    ev.cls_id = k->cls_id;
    ev.ts_ms = ts_ms;
    ev.severity = Severity::Alarm;
    ev.x = (best->x1 + best->x2) * 0.5f;
    ev.y = (best->y1 + best->y2) * 0.5f;
    char b[128];
    snprintf(b, sizeof(b), "置信度 %.2f, 持续 %.1fs, 命中 %d/%zu", best->score,
             (ts_ms - k->start_ms) / 1000.0, hits, k->history.size());
    ev.extra = b;
    out->push_back(ev);
}

}  // namespace mb
