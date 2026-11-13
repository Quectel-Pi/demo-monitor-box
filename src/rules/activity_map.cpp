#include "rules/activity_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "rules/rule_engine.h"

namespace mb {

namespace {

// 轨迹多久没出现就忘掉它上次所在的格子 (再出现按新目标计一次)
constexpr uint64_t kForgetMs = 10000;
constexpr int kMaxCells = 256 * 256;

Json counts_json(const std::vector<uint32_t>& v) {
    Json a = Json::array();
    for (uint32_t n : v) a.push(Json::number_value((double)n));
    return a;
}

bool read_counts(const Json& a, size_t n, std::vector<uint32_t>* out) {
    if (!a.is_array() || a.size() != n) return false;
    out->assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const double v = a[i].number(0.0);
        (*out)[i] = v > 0.0 ? (uint32_t)std::min(v, 4.0e9) : 0;
    }
    return true;
}

// 方向统计大多是 0, 只写有数据的格子: [[cell, d0..d7], ...]
Json flow_json(const std::vector<uint32_t>& flow, int dirs) {
    Json a = Json::array();
    for (size_t cell = 0; cell * dirs < flow.size(); ++cell) {
        bool any = false;
        for (int d = 0; d < dirs; ++d) any = any || flow[cell * dirs + d];
        if (!any) continue;
        Json row = Json::array();
        row.push(Json::number_value((double)cell));
        for (int d = 0; d < dirs; ++d) row.push(Json::number_value((double)flow[cell * dirs + d]));
        a.push(row);
    }
    return a;
}

void read_flow(const Json& a, size_t cells, int dirs, std::vector<uint32_t>* out) {
    out->assign(cells * dirs, 0);
    for (size_t i = 0; a.is_array() && i < a.size(); ++i) {
        const Json& row = a[i];
        if (!row.is_array() || row.size() != (size_t)dirs + 1) continue;
        const double cell = row[0].number(-1.0);
        if (cell < 0 || cell >= (double)cells) continue;
        for (int d = 0; d < dirs; ++d) {
            const double v = row[(size_t)d + 1].number(0.0);
            (*out)[(size_t)cell * dirs + d] = v > 0.0 ? (uint32_t)std::min(v, 4.0e9) : 0;
        }
    }
}

// 从格子 a 走到格子 b 的方向 (0 = 右, 顺时针每 45 度一格)
int dir_bin(int cols, int a, int b) {
    const int dx = b % cols - a % cols;
    const int dy = b / cols - a / cols;
    if (dx == 0 && dy == 0) return -1;
    float deg = std::atan2((float)dy, (float)dx) * 57.29578f;
    if (deg < 0.0f) deg += 360.0f;
    return (int)std::lround(deg / 45.0f) % ActivityMap::kDirs;
}

}  // namespace

ActivityMap::Params ActivityMap::Params::from_json(const Json& cfg) {
    Params p;
    const Json& a = cfg["activity"];
    p.enabled = a["enabled"].boolean(true);
    p.learning = a["learning"].boolean(false);
    p.cols = std::max(4, std::min(256, a["cols"].integer(64)));
    p.rows = std::max(4, std::min(256, a["rows"].integer(36)));
    p.path = a["path"].str("/userdata/monitor_box/activity.json");
    p.save_interval_sec = (float)std::max(10.0, a["save_interval_sec"].number(300.0));
    p.night_start = std::max(0, std::min(23, a["night_start"].integer(20)));
    p.night_end = std::max(0, std::min(23, a["night_end"].integer(6)));
    return p;
}

void ActivityMap::configure(const Params& p, const std::map<std::string, int>& class_map) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool resized = p.cols != p_.cols || p.rows != p_.rows ||
                         person_.pass.size() != (size_t)p.cols * (size_t)p.rows;
    p_ = p;
    groups_.clear();
    for (const auto& kv : class_map) {
        if (kv.first == "person") {
            groups_[kv.second] = kPerson;
        } else if (kv.first == "car" || kv.first == "truck" || kv.first == "bus" ||
                   kv.first == "motorcycle" || kv.first == "bicycle") {
            groups_[kv.second] = kVehicle;
        }
    }
    if (resized) clear_locked();
}

ActivityMap::Group ActivityMap::group_of(int cls_id) const {
    auto it = groups_.find(cls_id);
    return it == groups_.end() ? kNone : it->second;
}

bool ActivityMap::is_night(time_t t) const {
    if (p_.night_start == p_.night_end) return false;
    tm lt;
    localtime_r(&t, &lt);
    const int h = lt.tm_hour;
    if (p_.night_start < p_.night_end) return h >= p_.night_start && h < p_.night_end;
    return h >= p_.night_start || h < p_.night_end;
}

void ActivityMap::clear_locked() {
    const size_t n = std::min((size_t)std::max(0, p_.cols) * (size_t)std::max(0, p_.rows),
                              (size_t)kMaxCells);
    for (Layer* l : {&person_, &vehicle_}) {
        l->pass.assign(n, 0);
        l->night.assign(n, 0);
        l->births.assign(n, 0);
        l->deaths.assign(n, 0);
        l->flow.assign(n * kDirs, 0);
        l->tracks = 0;
    }
    since_ = wall_now();
    updated_ = 0;
    seen_.clear();
    dirty_ = true;
}

void ActivityMap::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    clear_locked();
}

void ActivityMap::warp(const Affine2& delta) {
    Affine2 inv;
    if (!invert_affine(delta, &inv)) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (person_.pass.empty()) return;
    const int cols = p_.cols, rows = p_.rows;
    // 只搬格子不转方向: 自动跟随只接受几度以内的旋转, 方向统计是 45 度一格, 影响不到
    auto move = [&](std::vector<uint32_t>* v, int per_cell) {
        std::vector<uint32_t> out(v->size(), 0);
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                const Point src = inv.apply(Point{(x + 0.5f) / cols, (y + 0.5f) / rows});
                const int sx = (int)std::floor(src.x * cols);
                const int sy = (int)std::floor(src.y * rows);
                if (sx < 0 || sy < 0 || sx >= cols || sy >= rows) continue;
                const size_t to = ((size_t)y * cols + x) * per_cell;
                const size_t from = ((size_t)sy * cols + sx) * per_cell;
                for (int k = 0; k < per_cell; ++k) out[to + k] = (*v)[from + k];
            }
        }
        v->swap(out);
    };
    for (Layer* l : {&person_, &vehicle_}) {
        move(&l->pass, 1);
        move(&l->night, 1);
        move(&l->births, 1);
        move(&l->deaths, 1);
        move(&l->flow, kDirs);
    }
    seen_.clear();
    dirty_ = true;
}

void ActivityMap::update(const std::vector<Track>& tracks, int img_w, int img_h, uint64_t ts_ms,
                         const RuleEngine* rules) {
    if (!p_.enabled || img_w <= 0 || img_h <= 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (person_.pass.empty()) clear_locked();
    const time_t now = wall_now();
    const bool night = is_night(now);
    for (const Track& t : tracks) {
        if (!t.confirmed || t.lost != 0) continue;
        const Group g = group_of(t.cls_id);
        if (g == kNone) continue;
        const Point foot{t.foot_x(), t.foot_y()};
        if (rules && rules->ignored(foot, img_w, img_h)) {
            auto it = seen_.find(t.id);
            if (it != seen_.end() && ts_ms <= it->second.last_ms + kForgetMs) {
                it->second.last_ms = ts_ms;
                it->second.hidden = true;
            }
            continue;
        }
        const int cx = std::max(0, std::min(p_.cols - 1, (int)(foot.x * p_.cols / img_w)));
        const int cy = std::max(0, std::min(p_.rows - 1, (int)(foot.y * p_.rows / img_h)));
        const int cell = cy * p_.cols + cx;
        if (cell < 0 || cell >= (int)person_.pass.size()) continue;

        Seen& s = seen_[t.id];
        const bool fresh = s.cell < 0 || ts_ms > s.last_ms + kForgetMs;
        if (fresh && s.cell >= 0 && s.group != kNone && !s.hidden)
            ++layer(s.group).deaths[(size_t)s.cell];
        s.last_ms = ts_ms;
        s.hidden = false;
        if (!fresh && s.cell == cell) continue;
        Layer& l = layer(g);
        if (!fresh) {
            const int d = dir_bin(p_.cols, s.cell, cell);
            if (d >= 0) ++l.flow[(size_t)cell * kDirs + d];
        }
        s.cell = cell;
        s.group = g;
        ++l.pass[(size_t)cell];
        if (night) ++l.night[(size_t)cell];
        if (fresh) {
            ++l.tracks;
            ++l.births[(size_t)cell];
        }
        updated_ = now;
        dirty_ = true;
    }
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (ts_ms > it->second.last_ms + kForgetMs) {
            if (it->second.group != kNone && it->second.cell >= 0 && !it->second.hidden)
                ++layer(it->second.group).deaths[(size_t)it->second.cell];
            dirty_ = true;
            it = seen_.erase(it);
        } else {
            ++it;
        }
    }
}

Json ActivityMap::json_locked(bool with_meta) const {
    Json j = Json::object();
    j.set("version", Json::number_value(2));
    if (with_meta) {
        j.set("enabled", Json::bool_value(p_.enabled));
        j.set("learning", Json::bool_value(p_.learning));
    }
    j.set("cols", Json::number_value(p_.cols));
    j.set("rows", Json::number_value(p_.rows));
    j.set("since", Json::number_value((double)since_));
    j.set("updated", Json::number_value((double)updated_));
    Json tracks = Json::object();
    tracks.set("person", Json::number_value((double)person_.tracks));
    tracks.set("vehicle", Json::number_value((double)vehicle_.tracks));
    j.set("tracks", tracks);
    j.set("person", counts_json(person_.pass));
    j.set("vehicle", counts_json(vehicle_.pass));
    Json hours = Json::array();
    hours.push(Json::number_value(p_.night_start));
    hours.push(Json::number_value(p_.night_end));
    j.set("night_hours", hours);
    const struct {
        const char* key;
        std::vector<uint32_t> Layer::*field;
    } extra[] = {{"night", &Layer::night}, {"births", &Layer::births}, {"deaths", &Layer::deaths}};
    for (const auto& e : extra) {
        Json o = Json::object();
        o.set("person", counts_json(person_.*(e.field)));
        o.set("vehicle", counts_json(vehicle_.*(e.field)));
        j.set(e.key, o);
    }
    Json flow = Json::object();
    flow.set("person", flow_json(person_.flow, kDirs));
    flow.set("vehicle", flow_json(vehicle_.flow, kDirs));
    j.set("flow", flow);
    return j;
}

Json ActivityMap::json() const {
    std::lock_guard<std::mutex> lk(mu_);
    return json_locked(true);
}

bool ActivityMap::load(std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (p_.path.empty()) return false;
    Json j;
    std::string perr;
    if (!Json::parse_file(p_.path, &j, &perr)) {
        if (err) *err = perr;
        return false;
    }
    const size_t n = (size_t)p_.cols * (size_t)p_.rows;
    Layer person, vehicle;
    if (j["cols"].integer(0) != p_.cols || j["rows"].integer(0) != p_.rows ||
        !read_counts(j["person"], n, &person.pass) || !read_counts(j["vehicle"], n, &vehicle.pass)) {
        if (err) *err = "网格尺寸不一致或数据损坏, 重新统计";
        return false;
    }
    // version 1 的文件只有通行量: 其余从零开始攒
    for (auto& pair : {std::make_pair(&person, "person"), std::make_pair(&vehicle, "vehicle")}) {
        Layer* l = pair.first;
        if (!read_counts(j["night"][pair.second], n, &l->night)) l->night.assign(n, 0);
        if (!read_counts(j["births"][pair.second], n, &l->births)) l->births.assign(n, 0);
        if (!read_counts(j["deaths"][pair.second], n, &l->deaths)) l->deaths.assign(n, 0);
        read_flow(j["flow"][pair.second], n, kDirs, &l->flow);
        l->tracks = (uint64_t)std::max(0.0, j["tracks"][pair.second].number(0.0));
    }
    person_ = std::move(person);
    vehicle_ = std::move(vehicle);
    since_ = (time_t)j["since"].number((double)wall_now());
    updated_ = (time_t)j["updated"].number(0.0);
    seen_.clear();
    dirty_ = false;
    return true;
}

bool ActivityMap::save_locked(std::string* err) {
    if (p_.path.empty()) return false;
    const Json j = json_locked(false);

    const std::string tmp = p_.path + ".tmp";    FILE* f = fopen(tmp.c_str(), "wb");
    const std::string text = j.dump() + "\n";
    bool ok = f && fwrite(text.data(), 1, text.size(), f) == text.size();
    if (f) ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp.c_str(), p_.path.c_str()) != 0) {
        remove(tmp.c_str());
        if (err) *err = "写 " + p_.path + " 失败";
        return false;
    }
    dirty_ = false;
    return true;
}

bool ActivityMap::maybe_save(uint64_t now_ms, bool force, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!dirty_ || p_.path.empty()) return true;
    if (!force && now_ms < last_save_ms_ + (uint64_t)(p_.save_interval_sec * 1000.0f)) return true;
    last_save_ms_ = now_ms;
    return save_locked(err);
}

}  // namespace mb
