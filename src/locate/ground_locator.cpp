#include "locate/ground_locator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mb {

namespace {

constexpr float kRad2Deg = 57.29577951308232f;
constexpr uint64_t kHealthWindowSec = 300;
constexpr size_t kMaxTrail = 600;

float dist(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

void add_extra(RuleEvent* e, const std::string& s) {
    if (!e->extra.empty()) e->extra += "; ";
    e->extra += s;
}

Json point_array(const std::vector<Point>& pts) {
    Json a = Json::array();
    for (const Point& p : pts) {
        Json q = Json::array();
        q.push(Json::number_value(std::round(p.x * 1000.0f) / 1000.0f));
        q.push(Json::number_value(std::round(p.y * 1000.0f) / 1000.0f));
        a.push(q);
    }
    return a;
}

bool parse_points(const Json& arr, const std::string& what, std::vector<Point>* out,
                  std::string* err) {
    out->clear();
    if (!arr.is_array() || arr.size() < 3) {
        if (err) *err = what + " 至少要 3 个点";
        return false;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        const Json& p = arr[i];
        if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) {
            if (err) *err = what + "[" + std::to_string(i) + "] 不是 [x,y]";
            return false;
        }
        out->push_back(Point{(float)p[0].number(0.0), (float)p[1].number(0.0)});
    }
    return true;
}

bool same_points(const std::vector<Point>& a, const std::vector<Point>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y) return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------

std::map<std::string, std::vector<std::string>> LocateParams::default_groups() {
    return {{"person", {"person"}},
            {"vehicle", {"car", "truck", "bus", "motorcycle", "bicycle"}},
            {"pet", {"dog", "cat"}}};
}

bool LocateParams::from_json(const Json& f, const std::map<std::string, int>& class_map,
                             LocateParams* out, std::string* err) {
    LocateParams p;
    p.report_interval_sec = (float)f["report_interval_sec"].number(5.0);
    p.direction_window_sec = (float)f["direction_window_sec"].number(1.5);
    p.direction_change_deg = (float)f["direction_change_deg"].number(60.0);
    p.smooth_accel = (float)std::max(0.1, f["smooth_accel"].number(3.0));
    p.foot_sigma_px = (float)std::max(0.0, f["foot_sigma_px"].number(3.0));
    p.foot_sigma_ratio = (float)std::max(0.0, f["foot_sigma_ratio"].number(0.02));
    p.trail_sec = (float)std::max(0.0, f["trail_sec"].number(30.0));
    p.zone_margin_m = (float)std::max(0.0, f["zone_margin_m"].number(0.3));
    p.zone_exit_sec = (float)std::max(0.0, f["zone_exit_sec"].number(1.0));
    p.zone_sustain_sec = (float)std::max(0.0, f["zone_sustain_sec"].number(0.5));
    const Json& g = f["groups"];
    if (g.is_null()) {
        p.groups = default_groups();
    } else {
        if (!g.is_object()) {
            if (err) *err = "locate.groups 必须是 {分组: [类别, ...]}";
            return false;
        }
        for (const auto& kv : g.members()) {
            if (!kv.second.is_array()) {
                if (err) *err = "locate.groups." + kv.first + " 必须是类别数组";
                return false;
            }
            std::vector<std::string>& labels = p.groups[kv.first];
            for (const Json& l : kv.second.items()) labels.push_back(l.str(""));
        }
    }
    for (const auto& kv : p.groups) {
        for (const std::string& l : kv.second) {
            auto it = class_map.find(l);
            if (it != class_map.end() && !p.group_of.count(it->second))
                p.group_of[it->second] = kv.first;
        }
    }
    if (out) *out = p;
    return true;
}

bool GroundZone::from_json(const std::string& id, const Json& j,
                           const std::map<std::string, std::vector<std::string>>& groups,
                           GroundZone* out, std::string* err) {
    GroundZone z;
    z.id = id;
    if (!j.is_object()) {
        if (err) *err = "ground_zones." + id + " 必须是对象";
        return false;
    }
    z.name = j["name"].str(id);
    z.enabled = j["enabled"].boolean(true);
    z.alarm_enter = j["alarm_enter"].boolean(false);
    z.dwell_sec = (float)std::max(0.0, j["dwell_sec"].number(0.0));
    const bool has_m = j.has("points_m"), has_img = j.has("points_img");
    if (has_m == has_img) {
        if (err) *err = "ground_zones." + id + " 要有且只有 points_m 或 points_img 之一";
        return false;
    }
    const std::string what = "ground_zones." + id + (has_m ? ".points_m" : ".points_img");
    std::vector<Point> pts;
    if (!parse_points(has_m ? j["points_m"] : j["points_img"], what, &pts, err)) return false;
    if (polygon_area(pts) <= 1e-9f) {
        if (err) *err = what + " 面积为 0";
        return false;
    }
    if (has_m) {
        z.source = "meters";
        z.pts_m = pts;
    } else {
        z.source = "image";
        z.base_img = pts;
        z.valid = false;  // 等标定换算
    }
    const Json& g = j["groups"];
    if (!g.is_null()) {
        if (!g.is_array()) {
            if (err) *err = "ground_zones." + id + ".groups 必须是数组";
            return false;
        }
        for (const Json& x : g.items()) {
            const std::string name = x.str("");
            if (!groups.count(name)) {
                if (err) *err = "ground_zones." + id + ".groups 里的 \"" + name + "\" 不是定位分组";
                return false;
            }
            z.groups.push_back(name);
        }
    }
    if (out) *out = z;
    return true;
}

bool GroundLocator::check_zones(const Json& zones, const Json& locate_feature, std::string* err) {
    if (zones.is_null()) return true;
    if (!zones.is_object()) {
        if (err) *err = "ground_zones 必须是对象";
        return false;
    }
    LocateParams p;
    if (!LocateParams::from_json(locate_feature, {}, &p, err)) return false;
    for (const auto& kv : zones.members()) {
        if (!GroundZone::from_json(kv.first, kv.second, p.groups, nullptr, err)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 配置 / 标定
// ---------------------------------------------------------------------------

bool GroundLocator::configure(const Json& region, const std::string& region_id,
                              const std::string& name, const LocateParams& params,
                              const Json& zones, std::string* err) {
    *this = GroundLocator();
    params_ = params;
    if (!zones.is_null()) {
        if (!zones.is_object()) {
            if (err) *err = "ground_zones 必须是对象";
            return false;
        }
        for (const auto& kv : zones.members()) {
            GroundZone z;
            if (!GroundZone::from_json(kv.first, kv.second, params_.groups, &z, err)) return false;
            zones_.push_back(z);
        }
    }
    if (region_id.empty()) return true;

    region_id_ = region_id;
    region_name_ = name;
    std::string perr;
    if (!GroundCalib::parse_region(region, &base_calib_, &base_check_, &mode_, &perr)) {
        if (!perr.empty()) {
            if (err) *err = "地面标定 " + region_id + ": " + perr;
            return false;
        }
        printf("[locate] 警告: 地面标定 %s 没有 real_size_m 或 calib_points, 定位不可用\n",
               region_id.c_str());
    }
    enabled_ = true;
    rebuild();
    return true;
}

void GroundLocator::rebuild() {
    std::vector<GroundPair> calib = base_calib_, check = base_check_;
    for (GroundPair& p : calib) p.img = view_.apply(p.img);
    for (GroundPair& p : check) p.img = view_.apply(p.img);
    std::string err;
    if (!calib.empty() && !calib_.fit(calib, check, &err))
        printf("[locate] 警告: 地面标定 %s 拟合失败 (%s), 定位不可用\n", region_id_.c_str(),
               err.c_str());
    if (calib.empty()) calib_ = GroundCalib();
    for (GroundZone& z : zones_) {
        if (z.source != "image") continue;
        z.pts_m.clear();
        z.valid = calib_.valid();
        for (const Point& p : z.base_img) {
            Point g;
            if (!calib_.to_ground(view_.apply(p), &g)) {
                z.valid = false;
                break;
            }
            z.pts_m.push_back(g);
        }
        if (!z.valid) z.pts_m.clear();
    }
}

void GroundLocator::set_view_transform(const Affine2& t) {
    view_ = t;
    if (enabled_) rebuild();
}

// ---------------------------------------------------------------------------
// 每帧
// ---------------------------------------------------------------------------

GroundLocator::HealthBucket& GroundLocator::bucket(uint64_t ts_ms) {
    const uint64_t sec = ts_ms / 1000;
    if (health_.empty() || health_.back().sec != sec) {
        HealthBucket b;
        b.sec = sec;
        health_.push_back(b);
    }
    while (!health_.empty() && health_.front().sec + kHealthWindowSec < sec) health_.pop_front();
    return health_.back();
}

void GroundLocator::step_filter(Target* t, Point z, float sigma, uint64_t ts_ms) {
    const double r = std::max(1e-4, (double)sigma * sigma);
    const double dt =
        t->has_filter && ts_ms > t->last_meas_ms ? (double)(ts_ms - t->last_meas_ms) / 1000.0 : 0.0;
    const Point pred{(float)(t->kx.x + t->kx.v * dt), (float)(t->ky.x + t->ky.v * dt)};
    // 太久没测量, 或位置跳得离谱 (ID 换人了): 不平滑, 直接从新位置重来
    if (!t->has_filter || dt > 3.0 || dist(pred, z) > std::max(3.0f, 6.0f * sigma)) {
        t->kx = Kalman1{z.x, 0.0, r, 0.0, 4.0};
        t->ky = Kalman1{z.y, 0.0, r, 0.0, 4.0};
        t->has_filter = true;
    } else {
        const double q2 = (double)params_.smooth_accel * params_.smooth_accel;
        auto step = [&](Kalman1* k, double meas) {
            k->x += k->v * dt;
            const double p00 = k->p00 + 2 * dt * k->p01 + dt * dt * k->p11 + q2 * dt * dt * dt * dt / 4;
            const double p01 = k->p01 + dt * k->p11 + q2 * dt * dt * dt / 2;
            const double p11 = k->p11 + q2 * dt * dt;
            const double s = p00 + r;
            const double k0 = p00 / s, k1 = p01 / s;
            const double y = meas - k->x;
            k->x += k0 * y;
            k->v += k1 * y;
            k->p00 = (1 - k0) * p00;
            k->p01 = (1 - k0) * p01;
            k->p11 = p11 - k1 * p01;
        };
        step(&t->kx, z.x);
        step(&t->ky, z.y);
    }
    t->pos = Point{(float)t->kx.x, (float)t->ky.x};
    t->last_meas_ms = ts_ms;
}

RuleEvent GroundLocator::make_event(const char* kind, const std::string& rule_id,
                                    const std::string& name, int id, const Target& t,
                                    uint64_t ts_ms, Severity sev) const {
    RuleEvent e;
    e.feature = params_.feature_key;
    e.kind = kind;
    e.rule_id = rule_id;
    e.name = name;
    e.track_id = id;
    e.cls_id = t.cls_id;
    e.label = t.label;
    e.ts_ms = ts_ms;
    e.severity = sev;
    e.x = t.last_foot.x;
    e.y = t.last_foot.y;
    e.gx = t.pos.x;
    e.gy = t.pos.y;
    e.has_ground = true;
    e.heading_deg = t.has_heading ? t.heading : 0.0f;
    return e;
}

void GroundLocator::observe(int id, int cls_id, const std::string& label, Point foot, float box_h,
                            int img_w, int img_h, uint64_t ts_ms, std::vector<RuleEvent>* out) {
    if (!enabled_ || !calib_.valid() || img_w <= 0 || img_h <= 0) return;
    last_frame_ms_ = ts_ms;
    auto found = targets_.find(id);
    if (found == targets_.end()) {
        Target t;
        t.first_ms = ts_ms;
        found = targets_.emplace(id, t).first;
    }
    Target& t = found->second;
    t.cls_id = cls_id;
    t.label = label;
    auto g = params_.group_of.find(cls_id);
    t.group = g != params_.group_of.end() ? g->second : "other";
    t.donated = false;
    t.last_ms = ts_ms;

    // 规则每个采集帧都跑, 但轨迹只在推理结果回来时变; 同一个脚点重复喂给滤波器会把速度压成 0
    const bool fresh = foot.x != t.last_foot.x || foot.y != t.last_foot.y || box_h != t.last_box_h;
    if (fresh) {
        t.last_foot = foot;
        t.last_box_h = box_h;
        HealthBucket& b = bucket(ts_ms);
        ++b.obs;
        const Point norm{foot.x / (float)img_w, foot.y / (float)img_h};
        Point raw;
        if (!calib_.to_ground(norm, &raw)) {
            ++b.unmapped;
        } else {
            const float sigma_px = std::max(params_.foot_sigma_px, box_h * params_.foot_sigma_ratio);
            float sigma = calib_.sigma_m(norm, sigma_px, img_w, img_h);
            if (sigma < 0.0f) sigma = 1.0f;
            const float fit_err = calib_.quality().error_estimate_m();
            if (fit_err > 0.0f) sigma = std::sqrt(sigma * sigma + fit_err * fit_err);
            t.raw = raw;
            t.sigma_m = sigma;
            t.in_calib = calib_.in_hull(raw);
            if (!t.in_calib) ++b.out_of_calib;
            b.sigma_sum += sigma;
            ++b.sigma_n;
            step_filter(&t, raw, sigma, ts_ms);
            const float speed = (float)std::hypot(t.kx.v, t.ky.v);
            if (speed < 0.15f && ts_ms - t.first_ms > 1000) {
                const float d = dist(t.raw, t.pos);
                b.jitter_sq += (double)d * d;
                ++b.jitter_n;
            }

            // 轨迹: 至少隔 0.2s 或 0.1m 记一个点
            if (t.trail.empty() || ts_ms - t.trail.back().first >= 200 ||
                dist(t.trail.back().second, t.pos) >= 0.1f) {
                if (!t.trail.empty()) t.distance_m += dist(t.trail.back().second, t.pos);
                t.trail.emplace_back(ts_ms, t.pos);
            }
            const uint64_t keep = (uint64_t)(params_.trail_sec * 1000.0f);
            while (t.trail.size() > kMaxTrail ||
                   (!t.trail.empty() && ts_ms - t.trail.front().first > keep && t.trail.size() > 1))
                t.trail.pop_front();

            // ---- 方向: 窗口内首尾位移 >= 0.3m 才判, 位移太小 atan2 的噪声能把方向甩出 180 度 ----
            const uint64_t window = (uint64_t)(params_.direction_window_sec * 1000.0f);
            const std::pair<uint64_t, Point>* first = nullptr;
            for (const auto& p : t.trail) {
                if (ts_ms - p.first <= window) {
                    first = &p;
                    break;
                }
            }
            if (first) {
                const float ddx = t.pos.x - first->second.x;
                const float ddy = t.pos.y - first->second.y;
                if (ddx * ddx + ddy * ddy > 0.09f) {
                    float deg = std::atan2(ddy, ddx) * kRad2Deg;
                    if (deg < 0.0f) deg += 360.0f;
                    bool changed = false;
                    if (t.has_heading) {
                        float diff = std::fabs(deg - t.heading);
                        if (diff > 180.0f) diff = 360.0f - diff;
                        changed = diff >= params_.direction_change_deg;
                    }
                    if (!t.has_heading || changed ||
                        (t.last_direction_ms < ts_ms && ts_ms - t.last_direction_ms >= window)) {
                        t.heading = deg;
                        t.has_heading = true;
                        t.last_direction_ms = ts_ms;
                        RuleEvent e = make_event("direction", region_id_, region_name_, id, t, ts_ms,
                                                 Severity::Info);
                        std::string dir = ddx >= 0.0f ? "向右" : "向左";
                        if (ddy > 0.0f) dir += "、远离镜头";
                        else if (ddy < 0.0f) dir += "、靠近镜头";
                        char buf[128];
                        snprintf(buf, sizeof(buf), "%s (%.0f°), 位置 (%.2f, %.2f)m", dir.c_str(),
                                 deg, t.pos.x, t.pos.y);
                        add_extra(&e, buf);
                        if (out) out->push_back(e);
                    }
                }
            }
        }
    }
    if (!t.has_filter) return;

    update_zones(id, &t, ts_ms, out);

    // ---- 周期位置上报 ----
    const uint64_t interval = (uint64_t)(params_.report_interval_sec * 1000.0f);
    const bool due = t.last_report_ms == 0 ||
                     (ts_ms >= t.last_report_ms && ts_ms - t.last_report_ms >= interval);
    if (!due) return;
    t.last_report_ms = ts_ms;
    RuleEvent e = make_event("position", region_id_, region_name_, id, t, ts_ms, Severity::Info);
    char buf[128];
    if (t.in_calib) {
        snprintf(buf, sizeof(buf), "位置 (%.2f, %.2f)m ±%.2fm", t.pos.x, t.pos.y, t.sigma_m);
        add_extra(&e, buf);
    } else {
        snprintf(buf, sizeof(buf), "位置 (%.2f, %.2f)m", t.pos.x, t.pos.y);
        add_extra(&e, buf);
        add_extra(&e, "位置超出标定地面, 结果仅供参考");
    }
    if (out) out->push_back(e);
}

bool GroundLocator::zone_applies(const GroundZone& z, const std::string& group) const {
    if (!z.enabled || !z.valid || z.pts_m.size() < 3) return false;
    return z.groups.empty() || std::find(z.groups.begin(), z.groups.end(), group) != z.groups.end();
}

void GroundLocator::update_zones(int id, Target* t, uint64_t ts_ms, std::vector<RuleEvent>* out) {
    for (const GroundZone& z : zones_) {
        if (!zone_applies(z, t->group)) {
            t->zones.erase(z.id);
            continue;
        }
        const bool inside = point_in_polygon(z.pts_m, t->pos);
        auto it = t->zones.find(z.id);
        if (it == t->zones.end()) {
            if (!inside) continue;
            ZoneOcc occ;
            occ.inside = true;
            occ.since = ts_ms;
            it = t->zones.emplace(z.id, occ).first;
        }
        ZoneOcc& occ = it->second;
        if (inside) {
            occ.out_since = 0;
        } else {
            // 出界先进滞回带: 离边界近且时间短当作定位抖动
            if (occ.out_since == 0) occ.out_since = ts_ms;
            const float d = distance_to_polygon_edge(z.pts_m, t->pos);
            const float out_for = (float)(ts_ms - occ.out_since) / 1000.0f;
            if (!(d <= params_.zone_margin_m && out_for < params_.zone_exit_sec)) {
                close_zone(id, t, z, &occ, occ.out_since, ts_ms, "离开", out);
                t->zones.erase(it);
                continue;
            }
        }
        if (!occ.confirmed && ts_ms - occ.since >= (uint64_t)(params_.zone_sustain_sec * 1000.0f)) {
            occ.confirmed = true;
            ZoneStats& s = stats_[z.id];
            ++s.visits;
            ++s.by_group[t->group];
            RuleEvent e = make_event("zone_enter", z.id, z.name, id, *t, ts_ms,
                                     z.alarm_enter ? Severity::Alarm : Severity::Info);
            char buf[160];
            snprintf(buf, sizeof(buf), "进入地面区域 %s, 位置 (%.2f, %.2f)m", z.name.c_str(),
                     t->pos.x, t->pos.y);
            add_extra(&e, buf);
            if (out) out->push_back(e);
        }
        if (occ.confirmed && z.dwell_sec > 0.0f && !occ.dwell_reported &&
            ts_ms - occ.since >= (uint64_t)(z.dwell_sec * 1000.0f)) {
            occ.dwell_reported = true;
            RuleEvent e = make_event("dwell", z.id, z.name, id, *t, ts_ms, Severity::Alarm);
            e.duration_sec = (float)(ts_ms - occ.since) / 1000.0f;
            char buf[160];
            snprintf(buf, sizeof(buf), "在地面区域 %s 停留 %.0fs", z.name.c_str(), e.duration_sec);
            add_extra(&e, buf);
            if (out) out->push_back(e);
        }
    }
}

void GroundLocator::close_zone(int id, Target* t, const GroundZone& z, ZoneOcc* occ,
                               uint64_t end_ms, uint64_t ts_ms, const char* why,
                               std::vector<RuleEvent>* out) {
    if (!occ->confirmed) return;
    const double dur = end_ms > occ->since ? (double)(end_ms - occ->since) / 1000.0 : 0.0;
    ZoneStats& s = stats_[z.id];
    s.total_dwell_sec += dur;
    s.max_dwell_sec = std::max(s.max_dwell_sec, dur);
    ++s.completed;
    RuleEvent e = make_event("zone_exit", z.id, z.name, id, *t, ts_ms, Severity::Info);
    e.duration_sec = (float)dur;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s地面区域 %s, 停留 %.0fs", why, z.name.c_str(), dur);
    add_extra(&e, buf);
    if (out) out->push_back(e);
}

void GroundLocator::end_frame(uint64_t ts_ms, float max_lost_sec, std::vector<RuleEvent>* out) {
    if (ts_ms > last_frame_ms_) last_frame_ms_ = ts_ms;
    const uint64_t limit = (uint64_t)(std::max(0.0f, max_lost_sec) * 1000.0f);
    for (auto it = targets_.begin(); it != targets_.end();) {
        Target& t = it->second;
        if (ts_ms > t.last_ms && ts_ms - t.last_ms > limit) {
            if (!t.donated) {
                for (auto& kv : t.zones) {
                    for (const GroundZone& z : zones_) {
                        if (z.id == kv.first) {
                            close_zone(it->first, &t, z, &kv.second, t.last_ms, ts_ms, "目标丢失, 离开",
                                       out);
                            break;
                        }
                    }
                }
            }
            it = targets_.erase(it);
        } else {
            ++it;
        }
    }
}

void GroundLocator::relink(int old_id, int new_id) {
    auto it = targets_.find(old_id);
    if (it == targets_.end() || targets_.count(new_id)) return;
    it->second.donated = true;
    Target inherited = it->second;
    inherited.donated = false;
    targets_[new_id] = inherited;
}

int GroundLocator::zone_liveness(int track_id, const std::string& zone_id, uint64_t now_ms) const {
    auto it = targets_.find(track_id);
    if (it == targets_.end()) return -1;
    auto z = it->second.zones.find(zone_id);
    if (z == it->second.zones.end() || !z->second.inside) return -1;
    if (it->second.last_ms != now_ms || z->second.out_since != 0) return 0;
    return 1;
}

void GroundLocator::adopt_state(const GroundLocator& old) {
    if (!enabled_ || !old.enabled_) return;
    const bool same_calib = calib_.valid() && old.calib_.valid() &&
                            std::equal(calib_.h().m, calib_.h().m + 9, old.calib_.h().m);
    auto same_zone = [&](const std::string& id) {
        const GroundZone *a = nullptr, *b = nullptr;
        for (const GroundZone& z : zones_) if (z.id == id) a = &z;
        for (const GroundZone& z : old.zones_) if (z.id == id) b = &z;
        return a && b && a->enabled && a->valid && same_points(a->pts_m, b->pts_m) &&
               a->groups == b->groups;
    };
    targets_ = old.targets_;
    for (auto& kv : targets_) {
        Target& t = kv.second;
        if (!same_calib) {
            // 标定变了: 旧坐标系下的位置和轨迹没意义, 下一次测量重新开始
            t.has_filter = false;
            t.trail.clear();
            t.has_heading = false;
            t.distance_m = 0.0f;
            t.last_foot = Point{-1.0f, -1.0f};
            t.zones.clear();
            continue;
        }
        for (auto z = t.zones.begin(); z != t.zones.end();) {
            if (same_zone(z->first)) ++z;
            else z = t.zones.erase(z);
        }
    }
    for (const auto& kv : old.stats_)
        if (same_zone(kv.first)) stats_[kv.first] = kv.second;
    if (same_calib) health_ = old.health_;
    last_frame_ms_ = old.last_frame_ms_;
}

void GroundLocator::reset_stats() {
    stats_.clear();
    // 正在区内的目标算作重新开始的一次到访
    for (auto& kv : targets_) {
        if (kv.second.donated) continue;
        for (auto& z : kv.second.zones) {
            if (!z.second.confirmed) continue;
            ZoneStats& s = stats_[z.first];
            ++s.visits;
            ++s.by_group[kv.second.group];
            z.second.since = last_frame_ms_;
        }
    }
}

// ---------------------------------------------------------------------------
// 输出
// ---------------------------------------------------------------------------

BevScene GroundLocator::scene() const {
    BevScene s;
    if (!enabled_ || !calib_.valid()) return s;
    s.valid = true;
    s.hull_m = calib_.hull_m();
    for (const GroundZone& z : zones_) {
        if (!z.enabled || !z.valid) continue;
        BevScene::Zone bz;
        bz.pts = z.pts_m;
        bz.name = z.name;
        bz.alarm = z.alarm_enter || z.dwell_sec > 0.0f;
        for (const auto& kv : targets_) {
            auto o = kv.second.zones.find(z.id);
            if (!kv.second.donated && o != kv.second.zones.end() && o->second.confirmed)
                bz.occupied = true;
        }
        s.zones.push_back(bz);
    }
    for (const auto& kv : targets_) {
        const Target& t = kv.second;
        if (t.donated || !t.has_filter) continue;
        BevScene::Target bt;
        bt.id = kv.first;
        bt.group = t.group;
        bt.pos = t.pos;
        bt.heading_deg = t.heading;
        bt.has_heading = t.has_heading;
        bt.in_calib = t.in_calib;
        bt.visible = t.last_ms == last_frame_ms_;
        for (const auto& p : t.trail) bt.trail.push_back(p.second);
        s.targets.push_back(bt);
    }
    return s;
}

Json GroundLocator::runtime_json(uint64_t now_ms) const {
    const uint64_t now_sec = now_ms / 1000;
    int obs = 0, out_of_calib = 0, unmapped = 0, sigma_n = 0, jitter_n = 0;
    double sigma_sum = 0.0, jitter_sq = 0.0;
    for (const HealthBucket& b : health_) {
        if (b.sec + kHealthWindowSec < now_sec) continue;
        obs += b.obs;
        out_of_calib += b.out_of_calib;
        unmapped += b.unmapped;
        sigma_sum += b.sigma_sum;
        sigma_n += b.sigma_n;
        jitter_sq += b.jitter_sq;
        jitter_n += b.jitter_n;
    }
    const double out_ratio = sigma_n > 0 ? (double)out_of_calib / sigma_n : 0.0;
    const double unmapped_ratio = obs > 0 ? (double)unmapped / obs : 0.0;
    const double mean_sigma = sigma_n > 0 ? sigma_sum / sigma_n : 0.0;
    const double jitter = jitter_n > 0 ? std::sqrt(jitter_sq / jitter_n) : 0.0;

    Json reasons = Json::array();
    std::string health = "ok";
    char buf[160];
    if (!calib_.valid()) {
        health = "invalid";
        reasons.push(Json::string_value("地面标定无效, 请重新标定"));
    } else {
        if (calib_.quality().grade == "poor") {
            health = "degraded";
            reasons.push(Json::string_value("标定质量差, 建议增加标定点或检查点"));
        }
        if (!view_.is_identity(1e-4f)) {
            health = "degraded";
            reasons.push(Json::string_value("摄像头画面有偏移, 标定在自动跟随; 偏移大时建议重新标定"));
        }
        if (obs >= 20) {
            if (unmapped_ratio > 0.05) {
                health = unmapped_ratio > 0.3 ? "invalid" : "degraded";
                snprintf(buf, sizeof(buf), "%.0f%% 的脚点在地平线以上无法换算, 标定可能已失效",
                         unmapped_ratio * 100.0);
                reasons.push(Json::string_value(buf));
            }
            if (out_ratio > 0.3) {
                if (health == "ok") health = "degraded";
                snprintf(buf, sizeof(buf), "%.0f%% 的位置在标定范围外, 建议扩大标定范围",
                         out_ratio * 100.0);
                reasons.push(Json::string_value(buf));
            }
            if (mean_sigma > 0.5) {
                if (health == "ok") health = "degraded";
                snprintf(buf, sizeof(buf), "平均定位不确定度 %.2fm, 目标多在远处", mean_sigma);
                reasons.push(Json::string_value(buf));
            }
            if (jitter_n >= 20 && jitter > 0.3) {
                if (health == "ok") health = "degraded";
                snprintf(buf, sizeof(buf), "静止目标位置抖动 %.2fm", jitter);
                reasons.push(Json::string_value(buf));
            }
        }
        if (obs == 0 && health == "ok") health = "idle";
    }
    Json j = Json::object();
    j.set("health", Json::string_value(health));
    j.set("observations", Json::number_value(obs));
    j.set("out_of_calib_ratio", Json::number_value(out_ratio));
    j.set("unmapped_ratio", Json::number_value(unmapped_ratio));
    j.set("mean_sigma_m", Json::number_value(mean_sigma));
    j.set("jitter_m", Json::number_value(jitter));
    j.set("reasons", reasons);
    return j;
}

Json GroundLocator::to_json(uint64_t now_ms) const {
    Json j = Json::object();
    j.set("enabled", Json::bool_value(enabled_ && calib_.valid()));
    j.set("ts_ms", Json::number_value((double)now_ms));
    if (enabled_) {
        Json c = calib_.to_json();
        c.set("region_id", Json::string_value(region_id_));
        c.set("name", Json::string_value(region_name_));
        c.set("mode", Json::string_value(mode_));
        c.set("view_corrected", Json::bool_value(!view_.is_identity(1e-4f)));
        j.set("calib", c);
    } else {
        j.set("calib", Json());
    }
    j.set("runtime", runtime_json(now_ms));

    Json zones = Json::array();
    for (const GroundZone& z : zones_) {
        Json zj = Json::object();
        zj.set("id", Json::string_value(z.id));
        zj.set("name", Json::string_value(z.name));
        zj.set("source", Json::string_value(z.source));
        zj.set("valid", Json::bool_value(z.valid));
        zj.set("enabled", Json::bool_value(z.enabled));
        zj.set("points_m", point_array(z.pts_m));
        zj.set("alarm_enter", Json::bool_value(z.alarm_enter));
        zj.set("dwell_sec", Json::number_value(z.dwell_sec));
        Json groups = Json::array();
        for (const std::string& g : z.groups) groups.push(Json::string_value(g));
        zj.set("groups", groups);

        Json st = Json::object();
        Json occupants = Json::array();
        for (const auto& kv : targets_) {
            auto o = kv.second.zones.find(z.id);
            if (!kv.second.donated && o != kv.second.zones.end() && o->second.confirmed)
                occupants.push(Json::number_value(kv.first));
        }
        auto si = stats_.find(z.id);
        const ZoneStats s = si != stats_.end() ? si->second : ZoneStats();
        st.set("count_now", Json::number_value((double)occupants.size()));
        st.set("occupants", occupants);
        st.set("visits", Json::number_value(s.visits));
        st.set("total_dwell_sec", Json::number_value(s.total_dwell_sec));
        st.set("avg_dwell_sec",
               Json::number_value(s.completed > 0 ? s.total_dwell_sec / s.completed : 0.0));
        st.set("max_dwell_sec", Json::number_value(s.max_dwell_sec));
        Json by = Json::object();
        for (const auto& g : s.by_group) by.set(g.first, Json::number_value(g.second));
        st.set("by_group", by);
        zj.set("stats", st);
        zones.push(zj);
    }
    j.set("zones", zones);

    Json targets = Json::array();
    for (const auto& kv : targets_) {
        const Target& t = kv.second;
        if (t.donated || !t.has_filter) continue;
        Json tj = Json::object();
        tj.set("id", Json::number_value(kv.first));
        tj.set("label", Json::string_value(t.label));
        tj.set("group", Json::string_value(t.group));
        tj.set("x", Json::number_value(t.pos.x));
        tj.set("y", Json::number_value(t.pos.y));
        tj.set("vx", Json::number_value(t.kx.v));
        tj.set("vy", Json::number_value(t.ky.v));
        tj.set("speed", Json::number_value(std::hypot(t.kx.v, t.ky.v)));
        tj.set("heading_deg", Json::number_value(t.heading));
        tj.set("has_heading", Json::bool_value(t.has_heading));
        tj.set("sigma_m", Json::number_value(t.sigma_m));
        tj.set("in_calib", Json::bool_value(t.in_calib));
        tj.set("visible", Json::bool_value(t.last_ms == last_frame_ms_));
        tj.set("age_sec", Json::number_value((double)(t.last_ms - t.first_ms) / 1000.0));
        tj.set("distance_m", Json::number_value(t.distance_m));
        Json in_zones = Json::array();
        for (const auto& z : t.zones)
            if (z.second.confirmed) in_zones.push(Json::string_value(z.first));
        tj.set("zones", in_zones);
        std::vector<Point> trail;
        for (const auto& p : t.trail) trail.push_back(p.second);
        tj.set("trail", point_array(trail));
        targets.push(tj);
    }
    j.set("targets", targets);
    return j;
}

}  // namespace mb
