#include "rules/rule_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mb {
namespace {


float dist(const Point& a, const Point& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

Point foot_of(const Track& t) { return Point{t.foot_x(), t.foot_y()}; }

// 自动清零按"本地日内的第几个周期"判断: 1 小时就是每个整点换一个 bucket。
// 用本地日 0 点做基准而不是拿 epoch 直接除, 有的时区不是整小时偏移。
int64_t period_bucket(time_t t, int period_sec) {
    tm lt;
    localtime_r(&t, &lt);
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    const time_t day = mktime(&lt);
    return (int64_t)(t - day) / period_sec;
}

void set_box(float b[4], const Track& t) {
    b[0] = t.x1;
    b[1] = t.y1;
    b[2] = t.x2;
    b[3] = t.y2;
}

float box_iou(const float a[4], const float b[4]) {
    return iou_boxes(a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
}

Point box_center(const float b[4]) { return Point{(b[0] + b[2]) * 0.5f, (b[1] + b[3]) * 0.5f}; }

bool in_box(Point p, const float b[4]) {
    return p.x >= b[0] && p.x <= b[2] && p.y >= b[1] && p.y <= b[3];
}

// a 被 b 盖住的面积占 a 的比例
float covered_ratio(const float a[4], const float b[4]) {
    const float w = std::min(a[2], b[2]) - std::max(a[0], b[0]);
    const float h = std::min(a[3], b[3]) - std::max(a[1], b[1]);
    const float area = (a[2] - a[0]) * (a[3] - a[1]);
    if (w <= 0.0f || h <= 0.0f || area <= 0.0f) return 0.0f;
    return w * h / area;
}

float dist_to_box(Point p, const float b[4]) {
    const float dx = std::max({b[0] - p.x, 0.0f, p.x - b[2]});
    const float dy = std::max({b[1] - p.y, 0.0f, p.y - b[3]});
    return std::sqrt(dx * dx + dy * dy);
}

void add_extra(RuleEvent* e, const std::string& s) {
    if (e->extra.empty()) {
        e->extra = s;
    } else {
        e->extra += " / ";
        e->extra += s;
    }
}

// "HH:MM" -> 当天分钟数; allow_24 时接受 "24:00" (=1440, 只用于 end)。
bool parse_hhmm(const std::string& s, bool allow_24, int* out) {
    int h = 0, m = 0;
    char tail = 0;
    if (sscanf(s.c_str(), "%d:%d%c", &h, &m, &tail) != 2) return false;
    if (h < 0 || m < 0 || m > 59) return false;
    if (h > 23 && !(allow_24 && h == 24 && m == 0)) return false;
    *out = h * 60 + m;
    return true;
}

bool parse_arm(const Json& arm, RuleEngine::Feature* feat, std::string* err) {
    feat->arm_mode = arm["mode"].str("always");
    feat->arm_windows.clear();
    if (feat->arm_mode != "always" && feat->arm_mode != "off" && feat->arm_mode != "schedule") {
        *err = feat->id + ".arm.mode 只能是 always / off / schedule: " + feat->arm_mode;
        return false;
    }
    const Json& sch = arm["schedule"];
    for (size_t i = 0; sch.is_array() && i < sch.size(); ++i) {
        const Json& w = sch[i];
        const std::string where = feat->id + ".arm.schedule[" + std::to_string(i) + "]";
        RuleEngine::Feature::ArmWindow aw;
        if (!parse_hhmm(w["start"].str(""), false, &aw.start_min) ||
            !parse_hhmm(w["end"].str(""), true, &aw.end_min)) {
            *err = where + " 的 start/end 要写成 \"HH:MM\"";
            return false;
        }
        if (aw.end_min == 1440) aw.end_min = 0;
        const Json& days = w["days"];
        if (days.is_array()) {
            aw.days = 0;
            for (size_t k = 0; k < days.size(); ++k) {
                const int d = days[k].integer(0);
                if (d < 1 || d > 7) {
                    *err = where + " 的 days 只能是 1(周一)..7(周日)";
                    return false;
                }
                aw.days |= 1u << (d - 1);
            }
        }
        feat->arm_windows.push_back(aw);
    }
    if (feat->arm_mode == "schedule" && feat->arm_windows.empty()) {
        *err = feat->id + ".arm.mode=schedule 但 schedule 为空";
        return false;
    }
    return true;
}

}  // namespace

bool RuleEngine::armed_at(const Feature& f, time_t t) {
    if (f.arm_mode == "off") return false;
    if (f.arm_mode != "schedule") return true;
    tm lt;
    localtime_r(&t, &lt);
    const int today = lt.tm_wday == 0 ? 6 : lt.tm_wday - 1;  // 0 = 周一
    const int yesterday = (today + 6) % 7;
    const int m = lt.tm_hour * 60 + lt.tm_min;
    for (const Feature::ArmWindow& w : f.arm_windows) {
        const bool on_today = w.days & (1u << today);
        if (w.start_min == w.end_min) {
            if (on_today) return true;
        } else if (w.start_min < w.end_min) {
            if (on_today && m >= w.start_min && m < w.end_min) return true;
        } else {
            // 跨零点: 当天 start 之后, 或前一天开始的时段延续到今天 end 之前
            if (on_today && m >= w.start_min) return true;
            if ((w.days & (1u << yesterday)) && m < w.end_min) return true;
        }
    }
    return false;
}

void RuleEngine::refresh_armed() {
    const time_t now = wall_override_ ? wall_override_ : time(nullptr);
    for (const Feature& f : features_) {
        if (!armable(f)) continue;
        const bool on = f.enabled && armed_at(f, now);
        auto it = armed_.find(f.id);
        if (it != armed_.end() && it->second != on && f.enabled)
            printf("[rules] %s %s\n", f.id.c_str(), on ? "布防" : "撤防");
        armed_[f.id] = on;
    }
}

void RuleEngine::set_wall_time(time_t t) {
    // 只换时间基准, 不动 reset_bucket_: 下一次 update() 会自己按新时间算出周期序号,
    // 跨格就清零 (测试靠这个观察整点清零)。第一次 update 时计数本来就是空的。
    wall_override_ = t;
}

void RuleEngine::maybe_reset_counts() {
    if (reset_sec_ <= 0) return;
    const time_t now = wall_override_ ? wall_override_ : time(nullptr);
    const long long bucket = period_bucket(now, reset_sec_);
    if (bucket == reset_bucket_) return;
    {
        std::lock_guard<std::mutex> lk(line_mu_);
        for (auto& kv : line_counts_) kv.second = LineCount{};
    }
    reset_bucket_ = bucket;
    printf("[rules] 越线计数已按 %d 小时周期清零\n", reset_sec_ / 3600);
}

bool RuleEngine::armed_instance(const Feature& f) const {
    if (!f.enabled) return false;
    if (!armable(f)) return true;
    auto it = armed_.find(f.id);
    return it != armed_.end() && it->second;
}

bool RuleEngine::armed(const std::string& key) const {
    for (const Feature& f : features_)
        if (f.key == key && armed_instance(f)) return true;
    return false;
}

bool RuleEngine::armed(const std::string& key, const std::string& target) const {
    const Feature* f = find_feature(key, target);
    return f && armed_instance(*f);
}

bool RuleEngine::load(const Json& cfg, const std::map<std::string, int>& class_map,
                      std::string* err) {
    features_.clear();
    regions_.clear();
    lines_.clear();
    locator_ = GroundLocator();
    states_.clear();
    stay_.clear();
    {
        std::lock_guard<std::mutex> lk(line_mu_);
        line_counts_.clear();
    }
    class_names_.clear();
    aliases_.clear();
    sites_.clear();
    next_site_ = 0;
    person_cls_ = -1;
    last_update_ms_ = 0;
    next_session_ = 0;
    max_lost_sec_ = 3.0f;
    reset_sec_ = 0;
    reset_bucket_ = -1;
    people_in_frame_.store(0);
    relink_sec_ = (float)std::max(0.0, cfg["track"]["relink_sec"].number(2.0));
    relink_dist_ = (float)std::max(0.0, cfg["track"]["relink_dist"].number(0.6));

    for (const auto& kv : class_map) class_names_[kv.second] = kv.first;
    {
        auto it = class_map.find("person");
        if (it != class_map.end()) person_cls_ = it->second;
    }

    // ---- regions ----
    const Json& regions = cfg["regions"];
    if ((int)regions.size() > kMaxRegions) {
        if (err)
            *err = "区域最多只能有 " + std::to_string(kMaxRegions) + " 个 (当前 " +
                   std::to_string(regions.size()) + " 个)";
        return false;
    }
    for (const auto& kv : regions.members()) {
        const Json& r = kv.second;
        NamedPolygon poly;
        poly.id = kv.first;
        poly.name = r["name"].str(poly.id);
        poly.role = r["role"].str("");
        if (!valid_role(poly.role)) {
            if (err)
                *err = "region " + poly.id + " 的 role 只能是 private / public / ignore / ground: " +
                       poly.role;
            return false;
        }
        const Json& pts = r["points"];
        if (!pts.is_array() || pts.size() < 3) {
            if (err) *err = "region " + poly.id + " 的 points 少于 3 个";
            return false;
        }
        for (size_t i = 0; i < pts.size(); ++i) {
            if (!pts[i].is_array() || pts[i].size() < 2) {
                if (err) *err = "region " + poly.id + " 的 points[" + std::to_string(i) + "] 不是 [x,y]";
                return false;
            }
            poly.points.push_back(Point{(float)pts[i][0].number(0.0), (float)pts[i][1].number(0.0)});
        }
        const Json& real = r["real_size_m"];
        if (real.is_array() && real.size() >= 2 && poly.points.size() == 4) {
            poly.has_real_size = true;
            poly.real_w_m = (float)real[0].number(0.0);
            poly.real_h_m = (float)real[1].number(0.0);
        }
        if (poly.role == "ground") {
            // 地面标定写错了 (calib_points 不是 4 个数、矩形缺尺寸) 不让落盘, 不管定位开没开
            std::vector<GroundPair> calib, check;
            std::string cerr;
            if (!GroundCalib::parse_region(r, &calib, &check, nullptr, &cerr) && !cerr.empty()) {
                if (err) *err = "地面标定 " + poly.id + ": " + cerr;
                return false;
            }
        }
        regions_.push_back(poly);
    }

    // ---- lines ----
    const Json& lines = cfg["lines"];
    for (const auto& kv : lines.members()) {
        const Json& l = kv.second;
        NamedLine line;
        line.id = kv.first;
        line.name = l["name"].str(line.id);
        const Json& p1 = l["p1"];
        const Json& p2 = l["p2"];
        if (!p1.is_array() || p1.size() < 2 || !p2.is_array() || p2.size() < 2) {
            if (err) *err = "line " + line.id + " 缺少 p1/p2";
            return false;
        }
        line.p1 = Point{(float)p1[0].number(0.0), (float)p1[1].number(0.0)};
        line.p2 = Point{(float)p2[0].number(0.0), (float)p2[1].number(0.0)};
        lines_.push_back(line);
        {
            std::lock_guard<std::mutex> lk(line_mu_);
            line_counts_[line.id] = LineCount{};
        }
    }

    // ---- 地面标定 ----
    base_region_points_.clear();
    for (const NamedPolygon& r : regions_) base_region_points_.push_back(r.points);
    base_line_points_.clear();
    for (const NamedLine& l : lines_) base_line_points_.emplace_back(l.p1, l.p2);
    view_ = Affine2();

    // ---- features ----
    // 每个功能按挂载点展开成实例 (rules/rule_bindings.h): 参数 = features.<key> 叠加挂载点覆盖。
    if (!check_rule_targets(cfg, err)) return false;
    const Json& feats = cfg["features"];
    for (const auto& kv : feats.members()) {
        const BindKind kind = binding_kind(cfg, kv.first);
        const std::vector<RuleBinding> binds = feature_bindings(cfg, kv.first);
        if (binds.empty()) {
            Feature feat;
            if (!parse_feature(kv.first, "", kind, kv.second, class_map, &feat, err)) return false;
            features_.push_back(feat);
            continue;
        }
        for (const RuleBinding& b : binds) {
            Feature feat;
            if (!parse_feature(kv.first, b.target, kind, b.params, class_map, &feat, err))
                return false;
            features_.push_back(feat);
        }
    }
    // 定位的路径/上报节拍按轨迹记, 不分地面标定区, 所以只能挂一个。
    const Feature* locate = nullptr;
    {
        int n = 0;
        for (const Feature& f : features_) {
            if (f.enabled && f.rule == "homography" && !f.region_id.empty()) {
                ++n;
                locate = &f;
            }
        }
        if (n > 1) {
            if (err) *err = "定位系统只能使用一个地面标定区";
            return false;
        }
    }
    // 地面区域 (ground_zones) 定位关着也要校验, 免得打开定位时才发现写错
    if (!GroundLocator::check_zones(cfg["ground_zones"], feats["locate"], err)) return false;
    {
        const NamedPolygon* g = locate ? find_region(locate->region_id) : nullptr;
        const LocateParams lp = locate ? locate->locate : LocateParams();
        if (!locator_.configure(g ? regions[g->id] : Json(), g ? g->id : "", g ? g->name : "", lp,
                                cfg["ground_zones"], err))
            return false;
    }

    // 人数统计的自动清零周期 (reset_hours, 0 = 不清零)。只有启用的 count 才算数。
    // 计数是按线各自维护、清却是整体清一次的, 所以挂多个计数功能时取最长周期。
    for (const Feature& f : features_) {
        if (f.enabled && f.rule == "line" && f.reset_hours > 0)
            reset_sec_ = std::max(reset_sec_, f.reset_hours * 3600);
    }
    reset_bucket_ = reset_sec_ > 0 ? (long long)period_bucket(wall_override_ ? wall_override_
                                                                          : time(nullptr),
                                                             reset_sec_)
                                   : -1;

    armed_.clear();
    refresh_armed();
    return true;
}

bool RuleEngine::parse_feature(const std::string& key, const std::string& target, BindKind kind,
                               const Json& f, const std::map<std::string, int>& class_map,
                               Feature* out, std::string* err) {
    Feature feat;
    feat.key = key;
    feat.id = target.empty() ? key : key + "@" + target;
    feat.enabled = f["enabled"].boolean(false);
    feat.rule = f["rule"].str("");
    feat.trigger = f["trigger"].str("");
    if (kind == BindKind::Region) feat.region_id = target;
    if (kind == BindKind::Line && !target.empty()) feat.line_ids.push_back(target);
    feat.sustain_sec = (float)f["sustain_sec"].number(0.5);
    feat.zone_margin = (float)std::max(0.0, f["zone_margin"].number(0.02));
    feat.exit_sec = (float)std::max(0.0, f["exit_sec"].number(1.0));
    feat.dwell_sec = (float)f["dwell_sec"].number(60.0);
    feat.scope = f["scope"].str("track");
    feat.escalate_sec = (float)std::max(0.0, f["escalate_sec"].number(0.0));
    feat.move_threshold_px = (float)f["move_threshold_px"].number(20.0);
    feat.line_margin = (float)f["line_margin"].number(0.03);
    feat.max_lost_sec = (float)f["max_lost_sec"].number(3.0);
    feat.reset_hours = (int)std::max(0.0, f["reset_hours"].number(0.0));
    feat.static_sec = (float)f["static_sec"].number(20.0);
    feat.move_confirm_sec = (float)std::max(0.0, f["move_confirm_sec"].number(1.0));
    feat.unattended_sec = (float)std::max(0.0, f["unattended_sec"].number(5.0));
    feat.unattended_radius = (float)std::max(0.0, f["unattended_radius"].number(0.5));
    feat.remove_sec = (float)std::max(0.0, f["remove_sec"].number(5.0));
    if (feat.rule == "homography" && !LocateParams::from_json(f, class_map, &feat.locate, err))
        return false;
    feat.locate.feature_key = key;

    const Json& cls = f["classes"];
    // 跌倒/火焰等功能的类别属于它们自己的模型 (pose/fire), 不在检测器的标签表里
    feat.model = f["model"].str("");
    if (cls.is_array() && feat.uses_detector()) {
        for (size_t i = 0; i < cls.size(); ++i) {
            const std::string name = cls[i].str("");
            auto it = class_map.find(name);
            if (it == class_map.end()) {
                printf("[rules] %s: 未知类别 \"%s\", 忽略\n", feat.id.c_str(), name.c_str());
                continue;
            }
            feat.classes.push_back(it->second);
        }
    }
    if (feat.enabled && feat.uses_detector() && cls.is_array() && cls.size() > 0 &&
        feat.classes.empty()) {
        if (err) *err = feat.id + ".classes 没有一项存在于检测模型标签中";
        return false;
    }
    if (feat.enabled && feat.key == "dwell") {
        if (feat.rule != "zone" || feat.trigger != "stay") {
            if (err) *err = feat.id + " 必须配置 rule=zone 和 trigger=stay";
            return false;
        }
        if (person_cls_ < 0 || feat.classes.size() != 1 || feat.classes[0] != person_cls_) {
            if (err)
                *err = feat.id + ".classes 必须且只能包含 person; 空类别会匹配所有物体, "
                       "不得用于人员滞留";
            return false;
        }
    }
    {
        std::string arm_err;
        if (!parse_arm(f["arm"], &feat, &arm_err)) {
            // 关着的功能配错了不影响别的功能启动, 等它被打开时再报
            if (feat.enabled) {
                if (err) *err = arm_err;
                return false;
            }
            printf("[rules] %s (功能未启用, 先忽略)\n", arm_err.c_str());
            feat.arm_mode = "always";
            feat.arm_windows.clear();
        }
    }

    if (feat.enabled) {
        // 启用了但指向不存在的区域/线, 属于配置错误, 直接报出来比运行后没反应好。
        if (!feat.region_id.empty() && !find_region(feat.region_id)) {
            if (err) *err = feat.key + " 指向不存在的 region: " + feat.region_id;
            return false;
        }
        if (!feat.region_id.empty() && find_region(feat.region_id)->role == "ignore") {
            if (err) *err = feat.key + " 不能使用忽略区: " + feat.region_id;
            return false;
        }
        for (const std::string& lid : feat.line_ids) {
            if (!find_line(lid)) {
                if (err) *err = feat.key + " 指向不存在的 line: " + lid;
                return false;
            }
        }
        if (feat.key == "locate" && feat.region_id.empty()) {
            if (err) *err = "locate 需要 region_id 指向 ground 标定区";
            return false;
        }
        max_lost_sec_ = std::max(max_lost_sec_, feat.max_lost_sec);
    }
    *out = feat;
    return true;
}

void RuleEngine::set_view_transform(const Affine2& t) {
    view_ = t;
    for (size_t i = 0; i < regions_.size() && i < base_region_points_.size(); ++i) {
        std::vector<Point>& pts = regions_[i].points;
        const std::vector<Point>& base = base_region_points_[i];
        for (size_t k = 0; k < pts.size() && k < base.size(); ++k) pts[k] = t.apply(base[k]);
    }
    for (size_t i = 0; i < lines_.size() && i < base_line_points_.size(); ++i) {
        lines_[i].p1 = t.apply(base_line_points_[i].first);
        lines_[i].p2 = t.apply(base_line_points_[i].second);
    }
    locator_.set_view_transform(t);
}

bool RuleEngine::same_geometry(const RuleEngine& o) const {
    auto same_pt = [](Point a, Point b) { return a.x == b.x && a.y == b.y; };
    if (regions_.size() != o.regions_.size() || lines_.size() != o.lines_.size() ||
        base_region_points_.size() != o.base_region_points_.size() ||
        base_line_points_.size() != o.base_line_points_.size())
        return false;
    for (size_t i = 0; i < regions_.size(); ++i) {
        if (regions_[i].id != o.regions_[i].id) return false;
        const std::vector<Point>& a = base_region_points_[i];
        const std::vector<Point>& b = o.base_region_points_[i];
        if (a.size() != b.size()) return false;
        for (size_t k = 0; k < a.size(); ++k)
            if (!same_pt(a[k], b[k])) return false;
    }
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].id != o.lines_[i].id ||
            !same_pt(base_line_points_[i].first, o.base_line_points_[i].first) ||
            !same_pt(base_line_points_[i].second, o.base_line_points_[i].second))
            return false;
    }
    return true;
}

std::vector<std::vector<Point>> RuleEngine::base_ignore_polygons() const {
    std::vector<std::vector<Point>> out;
    for (size_t i = 0; i < regions_.size() && i < base_region_points_.size(); ++i)
        if (regions_[i].role == "ignore") out.push_back(base_region_points_[i]);
    return out;
}

std::vector<Point> RuleEngine::region_pixels(const NamedPolygon& r, int w, int h) const {
    std::vector<Point> out;
    out.reserve(r.points.size());
    for (const Point& p : r.points) out.push_back(denormalize(p, w, h));
    return out;
}

void RuleEngine::line_pixels(const NamedLine& l, int w, int h, Point* a, Point* b) const {
    if (a) *a = denormalize(l.p1, w, h);
    if (b) *b = denormalize(l.p2, w, h);
}

bool RuleEngine::valid_role(const std::string& role) {
    return role.empty() || role == "private" || role == "public" || role == "ignore" ||
           role == "ground";
}

bool RuleEngine::region_active(const NamedPolygon& r) const {
    if (r.role == "ignore") return true;
    for (const Feature& f : features_) {
        if (f.enabled && f.region_id == r.id) return true;
    }
    return false;
}

uint32_t RuleEngine::region_color(const NamedPolygon& r) const {
    // Keep in sync with app/mb_regions.py::REGION_COLORS.
    static constexpr uint32_t colors[] = {
        0x30D158, 0xFFB020, 0x00A5FF, 0xAF7AFF,
        0xFF6B6B, 0x00D5C8, 0xD4E157, 0xFF9ED2,
        0xBCAAA4, 0x90CAF9, 0xFF8A50, 0xB2DF8A,
    };
    size_t index = 0;
    for (const NamedPolygon& other : regions_)
        if (other.id < r.id) ++index;
    return 0xFF000000u | colors[index % (sizeof(colors) / sizeof(colors[0]))];
}

bool RuleEngine::line_active(const NamedLine& l) const {
    for (const Feature& f : features_) {
        if (f.enabled && std::find(f.line_ids.begin(), f.line_ids.end(), l.id) != f.line_ids.end())
            return true;
    }
    return false;
}

bool RuleEngine::ignored(Point foot_px, int w, int h) const {
    if (w <= 0 || h <= 0) return false;
    const Point pn{foot_px.x / (float)w, foot_px.y / (float)h};
    for (const NamedPolygon& r : regions_) {
        if (r.role == "ignore" && point_in_polygon(r.points, pn)) return true;
    }
    return false;
}

const NamedPolygon* RuleEngine::find_region(const std::string& id) const {
    for (const NamedPolygon& r : regions_) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

const NamedLine* RuleEngine::find_line(const std::string& id) const {
    for (const NamedLine& l : lines_) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

const RuleEngine::Feature* RuleEngine::find_feature(const std::string& key) const {
    for (const Feature& f : features_) {
        if (f.key == key) return &f;
    }
    return nullptr;
}

const RuleEngine::Feature* RuleEngine::find_feature(const std::string& key,
                                                    const std::string& target) const {
    for (const Feature& f : features_) {
        if (f.key != key) continue;
        if (f.region_id == target ||
            std::find(f.line_ids.begin(), f.line_ids.end(), target) != f.line_ids.end())
            return &f;
    }
    return nullptr;
}

int RuleEngine::resolve_id(int id) const {
    // 链长有限 (同一个人连续换几次 ID), 加上限防环
    for (int i = 0; i < 16; ++i) {
        auto it = aliases_.find(id);
        if (it == aliases_.end()) break;
        id = it->second.to;
    }
    return id;
}

void RuleEngine::relink(const Track& t, const std::vector<int>& live_ids, uint64_t ts_ms) {
    if (relink_sec_ <= 0.0f || relink_dist_ <= 0.0f) return;
    const uint64_t window = (uint64_t)(relink_sec_ * 1000.0f);
    const Point foot = foot_of(t);
    const Point center{t.cx(), t.cy()};
    std::string best_key;
    float best_score = 1.0f;  // 越小越像; >= 1 不接
    float best_d = 0.0f, best_iou = 0.0f;
    for (const auto& kv : states_) {
        const TrackState& s = kv.second;
        if (s.donated || s.cls_id != t.cls_id || s.last_seen_ms >= ts_ms ||
            ts_ms - s.last_seen_ms > window)
            continue;
        const int old_id = atoi(kv.first.c_str());
        // 还活着的轨迹不是"刚丢失"的, 不能被抢
        if (std::find(live_ids.begin(), live_ids.end(), old_id) != live_ids.end()) continue;
        // 半身被挡时框底 (脚点) 会大幅跳动, 所以同时看框中心距离和框 IoU
        const float gate = std::max(1.0f, std::max(t.height(), s.last_h)) * relink_dist_;
        const Point oc{(s.box[0] + s.box[2]) * 0.5f, (s.box[1] + s.box[3]) * 0.5f};
        const float d = std::min(dist(s.last_foot, foot), dist(oc, center));
        const float ov = iou_boxes(s.box[0], s.box[1], s.box[2], s.box[3], t.x1, t.y1, t.x2, t.y2);
        if (d > gate && ov < 0.3f) continue;
        const float score = std::min(d / gate, 1.0f) * (1.0f - ov);
        if (score < best_score) {
            best_score = score;
            best_d = d;
            best_iou = ov;
            best_key = kv.first;
        }
    }
    if (best_key.empty()) return;
    const int old_id = atoi(best_key.c_str());
    // 复制而不是搬走: 旧 id 若又被跟回来 (其实是跟踪器短暂分裂), 它自己的状态还在,
    // 不会当成新人再报一次
    TrackState& donor = states_[best_key];
    donor.donated = true;
    TrackState inherited = donor;
    inherited.donated = false;
    states_[std::to_string(t.id)] = inherited;
    aliases_[old_id] = Alias{t.id, ts_ms};
    // 地面轨迹 / 地面区域停留也接着算
    locator_.relink(old_id, t.id);
    printf("[rules] 轨迹接续: #%d -> #%d (距离 %.0fpx, IoU %.2f)\n", old_id, t.id, best_d,
           best_iou);
}

RuleEngine::Liveness RuleEngine::liveness(const RuleEvent& ev) const {
    const Feature* f = find_feature(ev.feature);
    if (!f) return Liveness::Unknown;
    if (f->rule == "object") {
        if (!f->enabled) return Liveness::Gone;
        // removed 本身就是"连续 remove_sec 看不到"确认出来的, 不能再按轨迹存活去抖
        if (ev.kind == "removed") return Liveness::Active;
        if (ev.kind != "abandoned") return Liveness::Unknown;
        auto it = sites_.find(f->key);
        if (it == sites_.end()) return Liveness::Gone;
        for (const ObjectSite& s : it->second) {
            if ("pkg-" + std::to_string(s.id) != ev.rule_id) continue;
            return s.last_seen == last_update_ms_ ? Liveness::Active : Liveness::Occluded;
        }
        return Liveness::Gone;
    }
    if (f->rule == "homography") {
        // 地面区域的越界 / 停留告警: 看目标是否还在那个地面区域里
        if (ev.kind != "zone_enter" && ev.kind != "dwell") return Liveness::Unknown;
        if (!f->enabled) return Liveness::Gone;
        const int s = locator_.zone_liveness(resolve_id(ev.track_id), ev.rule_id, last_update_ms_);
        return s > 0 ? Liveness::Active : s == 0 ? Liveness::Occluded : Liveness::Gone;
    }
    if (!zone_rule(*f)) return Liveness::Unknown;
    // 热加载把这个区域上的功能去掉了: 挂起的旧告警作废
    f = find_feature(ev.feature, ev.rule_id);
    if (!f) return Liveness::Gone;
    if (f->trigger == "stay") {
        if (!stay_by_track(*f) || (ev.kind != "stay" && ev.kind != "stay_long"))
            return Liveness::Unknown;
    } else if (ev.kind != "enter") {
        return Liveness::Unknown;
    }
    // 热加载把功能关了 / 撤防了: 挂起的旧告警作废
    if (!f->enabled) return Liveness::Gone;
    if (armable(*f) && !armed_instance(*f)) return Liveness::Gone;

    auto it = states_.find(std::to_string(resolve_id(ev.track_id)));
    if (it == states_.end()) return Liveness::Gone;
    const TrackState& st = it->second;
    auto z = st.zones.find(zone_key(*f, ev.rule_id));
    if (z == st.zones.end() || !z->second.inside) return Liveness::Gone;
    if (st.last_seen_ms != last_update_ms_) return Liveness::Occluded;
    // 抖动带里 (出界但还没确认离开) 不算命中, 也不算丢失
    if (z->second.out_since != 0) return Liveness::Occluded;
    return Liveness::Active;
}

void RuleEngine::adopt_state(const RuleEngine& old) {
    auto same_region = [&](const std::string& id) {
        const NamedPolygon* a = find_region(id);
        const NamedPolygon* b = old.find_region(id);
        if (!a || !b || a->points.size() != b->points.size()) return false;
        for (size_t i = 0; i < a->points.size(); ++i) {
            if (a->points[i].x != b->points[i].x || a->points[i].y != b->points[i].y) return false;
        }
        return true;
    };
    auto same_line = [&](const std::string& id) {
        const NamedLine* a = find_line(id);
        const NamedLine* b = old.find_line(id);
        return a && b && a->p1.x == b->p1.x && a->p1.y == b->p1.y && a->p2.x == b->p2.x &&
               a->p2.y == b->p2.y;
    };

    states_ = old.states_;
    for (auto& kv : states_) {
        TrackState& st = kv.second;
        for (auto z = st.zones.begin(); z != st.zones.end();) {
            const size_t bar = z->first.find('|');
            const std::string fkey = z->first.substr(0, bar);
            const std::string rid = bar == std::string::npos ? "" : z->first.substr(bar + 1);
            const Feature* f = find_feature(fkey, rid);
            if (f && f->enabled && zone_rule(*f) && same_region(rid)) ++z;
            else z = st.zones.erase(z);
        }
        for (auto l = st.line_side.begin(); l != st.line_side.end();) {
            if (same_line(l->first)) ++l;
            else l = st.line_side.erase(l);
        }
    }
    for (const auto& kv : old.stay_) {
        const size_t bar = kv.first.find('|');
        if (bar == std::string::npos) continue;
        const Feature* f = find_feature(kv.first.substr(0, bar), kv.first.substr(bar + 1));
        if (f && f->enabled && zone_rule(*f) && same_region(f->region_id))
            stay_[kv.first] = kv.second;
    }
    {
        // Web 线程可能同时在 /counts 上清零, 读旧引擎的计数也要进它的锁
        std::lock_guard<std::mutex> ok(old.line_mu_);
        std::lock_guard<std::mutex> lk(line_mu_);
        for (const auto& kv : old.line_counts_) {
            if (line_counts_.count(kv.first) && same_line(kv.first)) line_counts_[kv.first] = kv.second;
        }
    }
    aliases_ = old.aliases_;
    // 包裹位置跟区域无关; 只要功能还开着就搬过来 (APP 改 static_sec 会热加载, 不能因此重报)
    for (const auto& kv : old.sites_) {
        const Feature* f = find_feature(kv.first);
        if (f && f->enabled && f->rule == "object") sites_[kv.first] = kv.second;
    }
    next_site_ = old.next_site_;
    last_update_ms_ = old.last_update_ms_;
    next_session_ = old.next_session_;
    // 清零周期没变就沿用旧的周期序号, 免得改个无关配置就把当前这一格当成新周期;
    // 周期变了 (或本来就没开) 用 load() 按当前时间重算的值。
    if (reset_sec_ > 0 && reset_sec_ == old.reset_sec_) reset_bucket_ = old.reset_bucket_;
    locator_.adopt_state(old.locator_);
}

void RuleEngine::copy_from(const RuleEngine& o) {
    features_ = o.features_;
    regions_ = o.regions_;
    lines_ = o.lines_;
    locator_ = o.locator_;
    base_region_points_ = o.base_region_points_;
    base_line_points_ = o.base_line_points_;
    view_ = o.view_;
    states_ = o.states_;
    stay_ = o.stay_;
    {
        // Web 线程可能正在 this 上 GET/DELETE /counts, 也可能在 o 上读, 两边都要锁
        std::lock_guard<std::mutex> lk1(o.line_mu_);
        std::lock_guard<std::mutex> lk2(line_mu_);
        line_counts_ = o.line_counts_;
    }
    class_names_ = o.class_names_;
    aliases_ = o.aliases_;
    sites_ = o.sites_;
    next_site_ = o.next_site_;
    person_cls_ = o.person_cls_;
    max_lost_sec_ = o.max_lost_sec_;
    relink_sec_ = o.relink_sec_;
    relink_dist_ = o.relink_dist_;
    last_update_ms_ = o.last_update_ms_;
    next_session_ = o.next_session_;
    reset_sec_ = o.reset_sec_;
    reset_bucket_ = o.reset_bucket_;
    people_in_frame_.store(o.people_in_frame_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
    armed_ = o.armed_;
    wall_override_ = o.wall_override_;
}

RuleEngine::LineCounts RuleEngine::line_counts() const {
    std::lock_guard<std::mutex> lk(line_mu_);
    return line_counts_;
}

bool RuleEngine::reset_line_counts(const std::string& line_id) {
    std::lock_guard<std::mutex> lk(line_mu_);
    if (line_id.empty()) {
        for (auto& kv : line_counts_) kv.second = LineCount{};
        return true;
    }
    auto it = line_counts_.find(line_id);
    if (it == line_counts_.end()) return false;
    it->second = LineCount{};
    return true;
}

bool RuleEngine::needs_detector() const {
    for (const Feature& f : features_) {
        if (f.enabled && f.uses_detector()) return true;
    }
    return false;
}

bool RuleEngine::shows_class(int cls_id) const {
    for (const Feature& f : features_) {
        if (!f.enabled || !f.uses_detector()) continue;
        if (f.classes.empty() ||
            std::find(f.classes.begin(), f.classes.end(), cls_id) != f.classes.end())
            return true;
    }
    return false;
}

bool RuleEngine::matches(const Feature& f, const Track& t) const {
    if (f.classes.empty()) return true;
    return std::find(f.classes.begin(), f.classes.end(), t.cls_id) != f.classes.end();
}

void RuleEngine::expire(uint64_t ts_ms, std::vector<RuleEvent>* out) {
    const uint64_t limit = (uint64_t)(max_lost_sec_ * 1000.0f);
    for (auto it = states_.begin(); it != states_.end();) {
        if (ts_ms > it->second.last_seen_ms && ts_ms - it->second.last_seen_ms > limit) {
            const TrackState& st = it->second;
            // 滞留中的人丢了: 会话到此结束。已被新 id 接续的 (donated) 由接续者继续计时, 不算结束。
            if (out && !st.donated) {
                for (const auto& z : st.zones) {
                    if (z.second.stay_level <= 0 || !z.second.inside) continue;
                    if (session_level(z.first, z.second.session, &st) >= 0) continue;
                    const size_t bar = z.first.find('|');
                    const Feature* f = bar == std::string::npos
                                           ? nullptr
                                           : find_feature(z.first.substr(0, bar), z.first.substr(bar + 1));
                    const NamedPolygon* r = f ? find_region(f->region_id) : nullptr;
                    if (!f || !r || !stay_by_track(*f)) continue;
                    RuleEvent e = make_event(*f, "stay_end", r->id, r->name, atoi(it->first.c_str()),
                                             st.cls_id, ts_ms, Severity::Info, st.last_foot);
                    e.session = z.second.session;
                    e.duration_sec = (float)(st.last_seen_ms - z.second.since) / 1000.0f;
                    char buf[128];
                    snprintf(buf, sizeof(buf), "目标丢失, 在 %s 共停留 %.0fs", r->name.c_str(),
                             e.duration_sec);
                    add_extra(&e, buf);
                    out->push_back(e);
                }
            }
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
    // 区域级滞留计时: 区域里已经没人了, 但 update 里会在下次有人时重置;
    // 这里只兜底长时间完全没人的情况。
    for (auto it = stay_.begin(); it != stay_.end();) {
        if (it->second.active && ts_ms > it->second.last_update_ms &&
            ts_ms - it->second.last_update_ms > limit) {
            it = stay_.erase(it);
        } else {
            ++it;
        }
    }
    // 别名只在去抖/冷却的窗口内有用, 留一分钟足够
    for (auto it = aliases_.begin(); it != aliases_.end();) {
        if (ts_ms > it->second.ts_ms && ts_ms - it->second.ts_ms > 60000) {
            it = aliases_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<RuleEvent> RuleEngine::update(const std::vector<Track>& all_tracks, int img_w,
                                          int img_h, uint64_t ts_ms) {
    std::vector<RuleEvent> events;
    if (img_w <= 0 || img_h <= 0) return events;
    last_update_ms_ = ts_ms;
    refresh_armed();
    maybe_reset_counts();

    // 忽略区里的目标 (树影、邻居家) 对所有规则都当作不存在
    bool any_ignore = false;
    for (const NamedPolygon& r : regions_) any_ignore = any_ignore || r.role == "ignore";
    std::vector<Track> kept;
    if (any_ignore) {
        kept.reserve(all_tracks.size());
        for (const Track& t : all_tracks) {
            if (!ignored(foot_of(t), img_w, img_h)) kept.push_back(t);
        }
    }
    const std::vector<Track>& tracks = any_ignore ? kept : all_tracks;

    std::vector<int> live_ids;
    for (const Track& t : tracks) {
        if (t.confirmed && t.lost == 0) live_ids.push_back(t.id);
    }

    // 画面内人数 (OSD 的 NOW / Web 的 people): 已确认、本帧仍在的轨迹。
    // 优先按启用的越线计数功能配置的类别算 (通常是 person), 没有就按 person 类。
    // 同一目标挂多条计数线也只算一个人。
    {
        int people = 0;
        const Feature* line_feature = nullptr;
        for (const Feature& f : features_) {
            if (f.enabled && f.rule == "line") {
                line_feature = &f;
                break;
            }
        }
        for (const Track& t : tracks) {
            if (!t.confirmed || t.lost != 0) continue;
            const bool hit = line_feature ? matches(*line_feature, t)
                                          : (person_cls_ < 0 || t.cls_id == person_cls_);
            if (hit) ++people;
        }
        people_in_frame_.store(people, std::memory_order_relaxed);
    }

    // 需要"当前帧还活着"的轨迹: 用 lost==0 作为判据, 丢失中的轨迹不再产生新事件。
    for (const Track& t : tracks) {
        if (!t.confirmed || t.lost != 0) continue;
        const std::string key = std::to_string(t.id);
        if (!states_.count(key)) relink(t, live_ids, ts_ms);
        // 旧 id 自己又回来了: 它就是它自己, 不再指向接续者
        aliases_.erase(t.id);
        TrackState& st = states_[key];
        st.donated = false;
        st.last_seen_ms = ts_ms;
        st.cls_id = t.cls_id;
        st.last_foot = foot_of(t);
        st.last_h = t.height();
        st.box[0] = t.x1;
        st.box[1] = t.y1;
        st.box[2] = t.x2;
        st.box[3] = t.y2;
        const Point foot = foot_of(t);

        for (const Feature& f : features_) {
            if (!f.enabled) continue;
            if (!matches(f, t)) continue;

            if (zone_rule(f)) {
                const NamedPolygon* r = find_region(f.region_id);
                if (!r) continue;
                if (armable(f) && !armed_[f.id]) {
                    st.zones.erase(zone_key(f, r->id));
                    continue;
                }
                if (f.trigger == "stay") {
                    // 区域级滞留单独在下面按 feature 处理; 按目标滞留在这里逐个轨迹计时。
                    if (stay_by_track(f))
                        write_dwell_events(f, *r, &st, t, foot, img_w, img_h, ts_ms, &events);
                    continue;
                }
                write_zone_events(f, *r, &st, t, foot, img_w, img_h, ts_ms, &events);
            } else if (f.rule == "line") {
                for (const std::string& lid : f.line_ids) {
                    const NamedLine* l = find_line(lid);
                    if (!l) continue;
                    write_line_events(f, *l, &st, t, foot, img_w, img_h, ts_ms, &events);
                }
            } else if (f.rule == "homography") {
                auto it = class_names_.find(t.cls_id);
                locator_.observe(t.id, t.cls_id, it != class_names_.end() ? it->second : "", foot,
                                 t.height(), img_w, img_h, ts_ms, &events);
            }
        }
    }

    // 包裹: 按位置记账, 每个 object feature 每帧整体处理一次。
    for (const Feature& f : features_) {
        if (f.enabled && f.rule == "object") write_object_events(f, tracks, ts_ms, &events);
    }

    // 区域级滞留: 每个 stay feature 每帧只判断一次。
    for (const Feature& f : features_) {
        if (!f.enabled || f.trigger != "stay" || stay_by_track(f)) continue;
        const NamedPolygon* r = find_region(f.region_id);
        if (!r) continue;
        write_stay_events(f, *r, tracks, img_w, img_h, ts_ms, &events);
    }

    expire(ts_ms, &events);
    locator_.end_frame(ts_ms, max_lost_sec_, &events);
    return events;
}

int RuleEngine::zone_step(const Feature& f, const NamedPolygon& r, ZoneState* z, Point foot,
                          int w, int h, uint64_t ts_ms) const {
    const bool inside = point_in_polygon(region_pixels(r, w, h), foot);
    if (!z->inside) {
        if (!inside) return 0;
        z->inside = true;
        z->since = ts_ms;
        z->out_since = 0;
        z->reported = false;
        return 1;
    }
    if (inside) {
        z->out_since = 0;
        return 0;
    }
    // 出界先进抖动带: 离边界近且时间短就当作脚点抖动, 不清计时, 也不报离开。
    if (z->out_since == 0) z->out_since = ts_ms;
    const Point pn{foot.x / (float)w, foot.y / (float)h};
    const float d = distance_to_polygon_edge(r.points, pn);
    const float out_for = (float)(ts_ms - z->out_since) / 1000.0f;
    if (d <= f.zone_margin && out_for < f.exit_sec) return 0;

    z->inside = false;
    z->out_since = 0;
    z->reported = false;
    return -1;
}

RuleEvent RuleEngine::make_event(const Feature& f, const char* kind, const std::string& rule_id,
                                 const std::string& name, int track_id, int cls_id,
                                 uint64_t ts_ms, Severity sev, Point at) const {
    RuleEvent e;
    e.feature = f.key;
    e.kind = kind;
    e.rule_id = rule_id;
    e.name = name;
    e.track_id = track_id;
    e.cls_id = cls_id;
    auto it = class_names_.find(cls_id);
    e.label = it != class_names_.end() ? it->second : "";
    e.ts_ms = ts_ms;
    e.severity = sev;
    e.x = at.x;
    e.y = at.y;
    return e;
}

// 同一会话在其它轨迹状态里 (跟踪器分裂/旧 id 回来) 已报到的级别; -1 = 别处没有这个会话。
int RuleEngine::session_level(const std::string& zk, int session, const TrackState* self) const {
    int best = -1;
    for (const auto& kv : states_) {
        if (&kv.second == self) continue;
        auto z = kv.second.zones.find(zk);
        if (z == kv.second.zones.end() || !z->second.inside || z->second.session != session)
            continue;
        best = std::max(best, z->second.stay_level);
    }
    return best;
}

std::vector<RuleEngine::Dweller> RuleEngine::dwellers() const {
    std::vector<Dweller> out;
    for (const auto& kv : states_) {
        const TrackState& st = kv.second;
        if (st.donated) continue;  // 已被新 id 接续, 由接续者代表
        for (const auto& z : st.zones) {
            if (!z.second.inside || z.second.session <= 0) continue;
            const size_t bar = z.first.find('|');
            if (bar == std::string::npos) continue;
            const Feature* f = find_feature(z.first.substr(0, bar), z.first.substr(bar + 1));
            if (!f || !f->enabled || !stay_by_track(*f)) continue;
            const NamedPolygon* r = find_region(f->region_id);
            if (!r) continue;
            Dweller d;
            d.session = z.second.session;
            d.track_id = atoi(kv.first.c_str());
            d.cls_id = st.cls_id;
            auto it = class_names_.find(st.cls_id);
            d.label = it != class_names_.end() ? it->second : "";
            d.feature = f->key;
            d.region_id = r->id;
            d.region_name = r->name;
            d.since_ms = z.second.since;
            const uint64_t now = std::max(last_update_ms_, z.second.since);
            d.seconds = (float)(now - z.second.since) / 1000.0f;
            d.dwell_sec = f->dwell_sec;
            d.level = z.second.stay_level;
            d.visible = st.last_seen_ms == last_update_ms_;
            d.foot = st.last_foot;
            // 旧 id 又被跟回来时两份状态同一个会话: 只留看得见/计时更久的那份
            bool dup = false;
            for (Dweller& o : out) {
                if (o.session != d.session) continue;
                dup = true;
                if ((d.visible && !o.visible) || (d.visible == o.visible && d.seconds > o.seconds))
                    o = d;
                break;
            }
            if (!dup) out.push_back(d);
        }
    }
    return out;
}

void RuleEngine::write_dwell_events(const Feature& f, const NamedPolygon& r, TrackState* st,
                                    const Track& t, Point foot, int w, int h, uint64_t ts_ms,
                                    std::vector<RuleEvent>* out) {
    const std::string zk = zone_key(f, r.id);
    ZoneState& z = st->zones[zk];
    const uint64_t left_at = z.out_since ? z.out_since : ts_ms;
    const int step = zone_step(f, r, &z, foot, w, h, ts_ms);
    if (step > 0) {
        z.session = ++next_session_;
        z.stay_level = 0;
    }
    if (step < 0) {
        // 同一会话还有别的轨迹在区内 (分裂出来的那份走了), 会话没结束
        if (z.stay_level > 0 && session_level(zk, z.session, st) < 0) {
            RuleEvent e = make_event(f, "stay_end", r.id, r.name, t.id, t.cls_id, ts_ms,
                                     Severity::Info, foot);
            e.session = z.session;
            e.duration_sec = (float)(left_at - z.since) / 1000.0f;
            char buf[128];
            snprintf(buf, sizeof(buf), "离开 %s, 共停留 %.0fs", r.name.c_str(), e.duration_sec);
            add_extra(&e, buf);
            out->push_back(e);
        }
        z.stay_level = 0;
        return;
    }
    // 抖动带里不升级告警, 但计时照走: 站在边界附近徘徊也是滞留。
    if (!z.inside || z.out_since != 0) return;

    const float held = (float)(ts_ms - z.since) / 1000.0f;
    int want = held >= f.dwell_sec ? 1 : 0;
    if (want && f.escalate_sec > f.dwell_sec && held >= f.escalate_sec) want = 2;
    // 跟踪器分裂时旧 id 带着同一个会话回来: 另一份状态已经报过的级别不再报
    if (z.stay_level < want) z.stay_level = std::max(z.stay_level, session_level(zk, z.session, st));
    while (z.stay_level < want) {
        ++z.stay_level;
        const bool longer = z.stay_level >= 2;
        RuleEvent e = make_event(f, longer ? "stay_long" : "stay", r.id, r.name, t.id, t.cls_id,
                                 ts_ms, Severity::Alarm, foot);
        e.session = z.session;
        e.duration_sec = held;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %s 已停留 %.0fs", longer ? "超长滞留:" : "滞留:",
                 r.name.c_str(), held);
        add_extra(&e, buf);
        out->push_back(e);
    }
}

void RuleEngine::write_zone_events(const Feature& f, const NamedPolygon& r, TrackState* st,
                                   const Track& t, Point foot, int w, int h, uint64_t ts_ms,
                                   std::vector<RuleEvent>* out) {
    ZoneState& z = st->zones[zone_key(f, r.id)];
    if (zone_step(f, r, &z, foot, w, h, ts_ms) < 0) {
        if (f.trigger == "cross") {
            RuleEvent e = make_event(f, "exit", r.id, r.name, t.id, t.cls_id, ts_ms,
                                     Severity::Info, foot);
            add_extra(&e, "离开 " + r.name);
            out->push_back(e);
        }
        return;
    }
    if (!z.inside || z.out_since != 0) return;

    if (z.reported) return;
    const float held = (float)(ts_ms - z.since) / 1000.0f;
    if (held < f.sustain_sec) return;

    z.reported = true;
    RuleEvent e;
    e.feature = f.key;
    e.kind = "enter";
    e.rule_id = r.id;
    e.name = r.name;
    e.track_id = t.id;
    e.cls_id = t.cls_id;
    auto it = class_names_.find(t.cls_id);
    e.label = it != class_names_.end() ? it->second : "";
    e.ts_ms = ts_ms;
    e.severity = Severity::Alarm;
    e.x = foot.x;
    e.y = foot.y;
    char buf[96];
    snprintf(buf, sizeof(buf), "进入 %s, 已持续 %.1fs", r.name.c_str(), held);
    add_extra(&e, buf);
    out->push_back(e);
}

void RuleEngine::write_line_events(const Feature& f, const NamedLine& l, TrackState* st,
                                   const Track& t, Point foot, int w, int h, uint64_t ts_ms,
                                   std::vector<RuleEvent>* out) {
    // 用归一化坐标算带符号距离, 这样 margin 的含义和分辨率无关。
    const Point p{foot.x / (float)w, foot.y / (float)h};
    const float dx = l.p2.x - l.p1.x;
    const float dy = l.p2.y - l.p1.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) return;
    const float side = (dx * (p.y - l.p1.y) - dy * (p.x - l.p1.x)) / len;

    float& last = st->line_side[l.id];
    if (std::fabs(side) <= f.line_margin) return;  // 死区: 人站在线上抖来抖去不计数

    const float sign = side > 0.0f ? 1.0f : -1.0f;
    if (last == 0.0f) {
        last = sign;  // 第一次见到这条线, 只记方向, 不算过线
        return;
    }
    if (sign == last) return;

    last = sign;
    LineCount c;
    {
        std::lock_guard<std::mutex> lk(line_mu_);
        LineCount& ref = line_counts_[l.id];
        const bool forward = sign > 0.0f;
        if (forward) {
            ref.in += 1;
        } else {
            ref.out += 1;
        }
        c = ref;
    }

    RuleEvent e;
    e.feature = f.key;
    e.kind = "cross";
    e.rule_id = l.id;
    e.name = l.name;
    e.track_id = t.id;
    e.cls_id = t.cls_id;
    auto it = class_names_.find(t.cls_id);
    e.label = it != class_names_.end() ? it->second : "";
    e.ts_ms = ts_ms;
    // 越线计数是低优先级: 一整天可能过几百次, 全当告警会刷爆事件列表。
    e.severity = Severity::Info;
    e.x = foot.x;
    e.y = foot.y;
    char buf[128];
    snprintf(buf, sizeof(buf), "穿过 %s, 进 %d 出 %d", l.name.c_str(), c.in, c.out);
    add_extra(&e, buf);
    out->push_back(e);
}

void RuleEngine::write_object_events(const Feature& f, const std::vector<Track>& tracks,
                                     uint64_t ts_ms, std::vector<RuleEvent>* out) {
    std::vector<ObjectSite>& sites = sites_[f.key];
    std::vector<const Track*> live, cand;
    for (const Track& t : tracks) {
        if (!t.confirmed || t.lost != 0) continue;
        live.push_back(&t);
        if (matches(f, t)) cand.push_back(&t);
    }

    // 1) 轨迹认领位置: 先认原来的 track id, 再按框重叠认 (换 ID / 轨迹过期后重新出现)。
    //    不比类别: 背包/手提包在检测器里来回跳类别很常见, 同一个位置就是同一件东西。
    std::vector<int> bound(sites.size(), -1);
    std::vector<bool> used(cand.size(), false);
    for (size_t s = 0; s < sites.size(); ++s) {
        for (size_t c = 0; c < cand.size(); ++c) {
            if (used[c] || cand[c]->id != sites[s].track_id) continue;
            bound[s] = (int)c;
            used[c] = true;
            break;
        }
    }
    for (size_t s = 0; s < sites.size(); ++s) {
        if (bound[s] >= 0) continue;
        float best = -1.0f;
        int pick = -1;
        for (size_t c = 0; c < cand.size(); ++c) {
            if (used[c]) continue;
            float tb[4];
            set_box(tb, *cand[c]);
            const float ov = std::max(box_iou(sites[s].anchor, tb), box_iou(sites[s].box, tb));
            // 半遮挡时框会缩掉一大块, IoU 不够; 中心还落在原框里也算同一件
            if (ov < 0.3f && !in_box(box_center(tb), sites[s].anchor)) continue;
            if (ov > best) {
                best = ov;
                pick = (int)c;
            }
        }
        if (pick >= 0) {
            bound[s] = pick;
            used[pick] = true;
        }
    }
    for (size_t c = 0; c < cand.size(); ++c) {
        if (used[c]) continue;
        ObjectSite s;
        s.id = ++next_site_;
        s.cls_id = cand[c]->cls_id;
        s.track_id = cand[c]->id;
        set_box(s.anchor, *cand[c]);
        set_box(s.box, *cand[c]);
        s.since = ts_ms;
        s.last_seen = ts_ms;
        sites.push_back(s);
        bound.push_back((int)c);
    }

    const uint64_t static_ms = (uint64_t)(f.static_sec * 1000.0f);
    const uint64_t confirm_ms = (uint64_t)(f.move_confirm_sec * 1000.0f);
    const uint64_t unattended_ms = (uint64_t)(f.unattended_sec * 1000.0f);
    const uint64_t remove_ms = (uint64_t)(f.remove_sec * 1000.0f);
    const uint64_t lost_ms = (uint64_t)(f.max_lost_sec * 1000.0f);

    for (size_t i = 0; i < sites.size();) {
        ObjectSite& s = sites[i];
        const Track* own = bound[i] >= 0 ? cand[bound[i]] : nullptr;

        // 2) 遮挡 (人/猫/狗等别的目标盖住原位置) 与周边有没有人
        bool occluded = false;
        const Point center = box_center(s.anchor);
        for (const Track* t : live) {
            if (t == own) continue;
            float tb[4];
            set_box(tb, *t);
            if (!matches(f, *t) && covered_ratio(s.anchor, tb) >= 0.2f) occluded = true;
            if (unattended_ms > 0 && t->cls_id == person_cls_ &&
                dist_to_box(center, tb) <= t->height() * f.unattended_radius)
                s.last_person = ts_ms;
        }

        // 3) 移动判定: 被挡着时冻结; 偏移要持续 move_confirm_sec 才算真被挪了
        if (own) {
            set_box(s.box, *own);
            s.cls_id = own->cls_id;
            s.track_id = own->id;
            s.last_seen = ts_ms;
            s.absent_since = 0;
            const bool moved = !occluded &&
                               dist(box_center(s.anchor), box_center(s.box)) > f.move_threshold_px &&
                               box_iou(s.anchor, s.box) < 0.6f;
            if (!moved) {
                s.moved_since = 0;
            } else {
                if (s.moved_since == 0) s.moved_since = ts_ms;
                if (ts_ms - s.moved_since >= confirm_ms) {
                    // 换锚点。还没报的从开始偏移那一刻重新计时; 已报的是同一件东西, 不重报
                    std::copy(s.box, s.box + 4, s.anchor);
                    if (!s.reported) s.since = s.moved_since;
                    s.moved_since = 0;
                }
            }
        } else {
            s.moved_since = 0;
            if (occluded)
                s.absent_since = 0;
            else if (s.absent_since == 0)
                s.absent_since = ts_ms;
        }

        // 4) 到点报 abandoned (每个位置一次)
        if (own && !s.reported && s.moved_since == 0 && ts_ms - s.since >= static_ms &&
            (unattended_ms == 0 || ts_ms - s.last_person >= unattended_ms)) {
            s.reported = true;
            RuleEvent e = make_event(f, "abandoned", "pkg-" + std::to_string(s.id), "", own->id,
                                     own->cls_id, ts_ms, Severity::Alarm, foot_of(*own));
            char buf[96];
            snprintf(buf, sizeof(buf), "物体静止 %.0fs", (float)(ts_ms - s.since) / 1000.0f);
            add_extra(&e, buf);
            if (unattended_ms > 0 && s.last_person > 0) {
                snprintf(buf, sizeof(buf), "周边无人 %.0fs",
                         (float)(ts_ms - s.last_person) / 1000.0f);
                add_extra(&e, buf);
            }
            out->push_back(e);
        }

        // 5) 消失: 已报的无遮挡消失满 remove_sec 报 removed; 没报的过期就丢
        bool drop = false;
        if (!own) {
            const uint64_t absent = s.absent_since ? ts_ms - s.absent_since : 0;
            if (s.reported) {
                if (remove_ms > 0 && s.absent_since && absent >= remove_ms) {
                    const Point at{(s.box[0] + s.box[2]) * 0.5f, s.box[3]};
                    // 时刻取消失的那一刻, 录像的预录段才能覆盖"被拿走"
                    RuleEvent e = make_event(f, "removed", "pkg-" + std::to_string(s.id), "",
                                             s.track_id, s.cls_id, s.absent_since,
                                             Severity::Alarm, at);
                    char buf[96];
                    snprintf(buf, sizeof(buf), "物体已消失 %.0fs (此前静止 %.0fs)",
                             (float)absent / 1000.0f, (float)(s.absent_since - s.since) / 1000.0f);
                    add_extra(&e, buf);
                    out->push_back(e);
                    drop = true;
                } else if (remove_ms == 0 && s.absent_since && absent >= std::max(lost_ms, (uint64_t)60000)) {
                    drop = true;
                }
            } else if ((s.absent_since && absent >= lost_ms) || ts_ms - s.last_seen > 60000) {
                drop = true;
            }
        }
        if (drop) {
            sites.erase(sites.begin() + (long)i);
            bound.erase(bound.begin() + (long)i);
        } else {
            ++i;
        }
    }

    // 6) 两个位置重合 (一个被挪到另一个上 / 检测器重复框): 合并, 已报状态取并
    for (size_t i = 0; i < sites.size(); ++i) {
        for (size_t j = i + 1; j < sites.size();) {
            if (box_iou(sites[i].anchor, sites[j].anchor) < 0.5f) {
                ++j;
                continue;
            }
            ObjectSite& a = sites[i];
            const ObjectSite& b = sites[j];
            if (b.last_seen > a.last_seen) {
                a.track_id = b.track_id;
                std::copy(b.box, b.box + 4, a.box);
                a.last_seen = b.last_seen;
                a.absent_since = b.absent_since;
            }
            a.reported = a.reported || b.reported;
            a.since = std::min(a.since, b.since);
            a.last_person = std::max(a.last_person, b.last_person);
            sites.erase(sites.begin() + (long)j);
        }
    }
}

void RuleEngine::write_stay_events(const Feature& f, const NamedPolygon& r,
                                   const std::vector<Track>& tracks, int w, int h, uint64_t ts_ms,
                                   std::vector<RuleEvent>* out) {
    const std::vector<Point> poly = region_pixels(r, w, h);

    const Track* first = nullptr;
    Point sum{0.0f, 0.0f};
    int n = 0;
    for (const Track& t : tracks) {
        if (!t.confirmed || t.lost != 0) continue;
        if (!matches(f, t)) continue;
        const Point foot = foot_of(t);
        if (!point_in_polygon(poly, foot)) continue;
        sum.x += foot.x;
        sum.y += foot.y;
        ++n;
        if (!first) first = &t;
    }

    StayState& s = stay_[zone_key(f, f.region_id)];
    if (n == 0) {
        // 人走空了才清零。按区域占位计时, 中间换 ID 不影响 (DESIGN 2.3.2 的低成本方案)。
        s.active = false;
        s.reported = false;
        s.since_ms = 0;
        return;
    }
    const Point centroid{sum.x / (float)n, sum.y / (float)n};
    s.last_update_ms = ts_ms;
    if (!s.active) {
        s.active = true;
        s.since_ms = ts_ms;
        s.last_centroid = centroid;
        s.reported = false;
        return;
    }
    // 区域内的"人堆"整体挪了位置, 说明不是同一批人在原地待着, 重新计时。
    if (dist(s.last_centroid, centroid) > f.move_threshold_px) {
        s.last_centroid = centroid;
        s.since_ms = ts_ms;
        s.reported = false;
        return;
    }
    s.last_centroid = centroid;
    if (s.reported) return;
    const float held = (float)(ts_ms - s.since_ms) / 1000.0f;
    if (held < f.dwell_sec) return;

    s.reported = true;
    RuleEvent e;
    e.feature = f.key;
    e.kind = "stay";
    e.rule_id = r.id;
    e.name = r.name;
    e.track_id = first ? first->id : -1;
    e.cls_id = first ? first->cls_id : -1;
    if (first) {
        auto it = class_names_.find(first->cls_id);
        e.label = it != class_names_.end() ? it->second : "";
    }
    e.ts_ms = ts_ms;
    e.severity = Severity::Info;  // 滞留是低优先, 只记录
    e.x = centroid.x;
    e.y = centroid.y;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s 内 %d 人已停留 %.0fs", r.name.c_str(), n, held);
    add_extra(&e, buf);
    out->push_back(e);
}

}  // namespace mb
