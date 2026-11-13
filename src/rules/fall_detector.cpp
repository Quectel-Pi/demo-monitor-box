#include "rules/fall_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace mb {
namespace {

constexpr float kRad2Deg = 57.29577951308232f;

// 参与"贴地"和"横向分布"两项打分的关键点: 鼻子、双肩、双髋、双膝、双踝。
// 和 demo-fall-detect 的 SCORE_KEYS 一致, 改这里要同步改 Python 那边。
const int kScoreKeys[9] = {KPT_NOSE,      KPT_L_SHOULDER, KPT_R_SHOULDER, KPT_L_HIP, KPT_R_HIP,
                           KPT_L_KNEE,    KPT_R_KNEE,     KPT_L_ANKLE,    KPT_R_ANKLE};

float variance(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    float mean = 0.0f;
    for (float x : v) mean += x;
    mean /= (float)v.size();
    float acc = 0.0f;
    for (float x : v) {
        const float d = x - mean;
        acc += d * d;
    }
    return acc / (float)v.size();
}

// 跌倒检测只关心人, 姿态模型也只有 person 一类。
constexpr int kPersonClsId = 0;
constexpr const char* kPersonLabel = "person";

}  // namespace

int FallDetector::person_key(const PoseResult& pose, int track_id) {
    if (track_id > 0) return track_id;
    // 没配到轨迹 (刚出现、跟踪器还没确认) 时, 退化成 demo 的做法: 框中心按
    // 48px 量化当临时身份。量化后取负, 保证不会和从 1 开始的 track_id 撞。
    int qx = (int)std::floor(((pose.x1 + pose.x2) * 0.5f) / 48.0f);
    int qy = (int)std::floor(((pose.y1 + pose.y2) * 0.5f) / 48.0f);
    qx = std::min(9999, std::max(0, qx));
    qy = std::min(9999, std::max(0, qy));
    return -(qx * 10000 + qy) - 1;
}

std::vector<int> FallDetector::match_poses_to_tracks(const std::vector<PoseResult>& poses,
                                                     const std::vector<Track>& tracks) {
    std::vector<int> out(poses.size(), -1);
    if (poses.empty() || tracks.empty()) return out;

    struct Pair {
        float iou;
        size_t p;
        size_t t;
    };
    std::vector<Pair> pairs;
    pairs.reserve(poses.size() * tracks.size());
    for (size_t i = 0; i < poses.size(); ++i) {
        for (size_t j = 0; j < tracks.size(); ++j) {
            if (!tracks[j].confirmed) continue;
            const float v = iou_boxes(poses[i].x1, poses[i].y1, poses[i].x2, poses[i].y2,
                                      tracks[j].x1, tracks[j].y1, tracks[j].x2, tracks[j].y2);
            if (v >= 0.3f) pairs.push_back(Pair{v, i, j});
        }
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const Pair& a, const Pair& b) { return a.iou > b.iou; });

    std::vector<char> used_p(poses.size(), 0), used_t(tracks.size(), 0);
    for (const Pair& pr : pairs) {
        if (used_p[pr.p] || used_t[pr.t]) continue;  // 一个人只配一条轨迹, 反之亦然
        used_p[pr.p] = 1;
        used_t[pr.t] = 1;
        out[pr.p] = tracks[pr.t].id;
    }
    return out;
}

FallDetector::PersonVerdict FallDetector::judge(const PoseResult& pose, int img_w, int img_h,
                                                int track_id) {
    PersonVerdict v;
    const Keypoint* k = pose.kpts;

    bool vis[kNumKeypoints];
    int vis_n = 0;
    for (int i = 0; i < kNumKeypoints; ++i) {
        vis[i] = k[i].conf > params_.min_kpt_conf;
        if (vis[i]) ++vis_n;
    }
    const float vis_ratio = (float)vis_n / (float)kNumKeypoints;
    v.feat.kpt_vis_ratio = vis_ratio;
    // 骨架观测不完整时直接不下结论: 侧身、半出画的人只有半个骨架, 强行打分
    // 会得到"腿接近髋部高度"之类的假阳性。
    if (vis_ratio < params_.kpt_visible_ratio) return v;

    const int core[4] = {KPT_L_SHOULDER, KPT_R_SHOULDER, KPT_L_HIP, KPT_R_HIP};
    int core_vis = 0;
    for (int i : core) {
        if (vis[i]) ++core_vis;
    }

    // ---- 1) 躯干倾角: 肩中点 -> 髋中点 与垂直方向的夹角 ----
    float angle = 0.0f;
    float hip_x = 0.0f;
    float hip_y = 0.0f;
    float torso_len = 0.0f;
    if (core_vis >= 3) {
        const float sx = (k[KPT_L_SHOULDER].x + k[KPT_R_SHOULDER].x) * 0.5f;
        const float sy = (k[KPT_L_SHOULDER].y + k[KPT_R_SHOULDER].y) * 0.5f;
        hip_x = (k[KPT_L_HIP].x + k[KPT_R_HIP].x) * 0.5f;
        hip_y = (k[KPT_L_HIP].y + k[KPT_R_HIP].y) * 0.5f;
        v.has_hip = true;
        v.hip_y = hip_y;

        const float vx = hip_x - sx;
        const float vy = hip_y - sy;
        torso_len = std::sqrt(vx * vx + vy * vy);
        if (torso_len > 1e-3f) {
            float c = std::fabs(vy) / torso_len;
            c = std::min(1.0f, std::max(-1.0f, c));
            angle = std::acos(c) * kRad2Deg;
            v.feat.angle_ok = angle >= params_.torso_angle_threshold;
        }
    }
    v.feat.torso_angle = angle;

    const float half_thr = params_.torso_angle_threshold * 0.6f;
    float score = 0.0f;
    if (v.feat.angle_ok) {
        score += 2.0f;
    } else if (angle >= half_thr) {
        score += 1.0f;
    }

    // ---- 1b) 腿部水平: 膝/踝接近髋部高度, 躺姿标志 (坐姿腿朝下, 不命中) ----
    bool legs_h = false;
    if (v.has_hip && torso_len > 1e-3f) {
        const int legs[4] = {KPT_L_KNEE, KPT_R_KNEE, KPT_L_ANKLE, KPT_R_ANKLE};
        float dmin = 0.0f;
        bool any = false;
        for (int i : legs) {
            if (!vis[i]) continue;
            const float d = std::fabs(k[i].y - hip_y);
            if (!any || d < dmin) {
                dmin = d;
                any = true;
            }
        }
        if (any) legs_h = dmin <= torso_len * 0.45f;
    }
    if (legs_h) {
        score += 1.5f;
        // 半躺组合: 躯干半倾 + 腿水平, 是斜靠撑地的典型形态, 补半分
        if (angle >= half_thr && angle < params_.torso_angle_threshold) score += 0.5f;
    }

    // ---- 2) 身体长宽比: 优先用关键点 (头顶 vs 脚踝, 肩宽), 不可见才用框兜底 ----
    bool ratio_ok = false;
    const float box_w = pose.x2 - pose.x1;
    const float box_h = pose.y2 - pose.y1;
    if (vis[KPT_NOSE] && (vis[KPT_L_ANKLE] || vis[KPT_R_ANKLE])) {
        const float head_y = k[KPT_NOSE].y;
        const float ankle_y = std::max(k[KPT_L_ANKLE].y, k[KPT_R_ANKLE].y);
        const float body_h = std::fabs(ankle_y - head_y);
        float body_w = 0.0f;
        if (vis[KPT_L_SHOULDER] && vis[KPT_R_SHOULDER]) {
            body_w = std::fabs(k[KPT_L_SHOULDER].x - k[KPT_R_SHOULDER].x);
        }
        if (body_w > 1.0f) ratio_ok = body_h / body_w < params_.height_ratio_threshold;
    } else {
        const float ratio = box_w > 1.0f ? box_h / box_w : 99.0f;
        ratio_ok = ratio <= params_.aspect_ratio_threshold;
    }
    if (ratio_ok) score += 2.0f;
    v.feat.ratio_ok = ratio_ok;

    // ---- 3) 贴地: 多数关键点落在画面下部 30% ----
    if (img_w > 0 && img_h > 0 && core_vis >= 3) {
        const float bottom = (float)img_h * 0.7f;
        int cnt = 0;
        int tot = 0;
        for (int i : kScoreKeys) {
            if (!vis[i]) continue;
            ++tot;
            if (k[i].y > bottom) ++cnt;
        }
        if (tot > 0 && (float)cnt / (float)tot > 0.6f) score += 1.5f;
    }

    // ---- 4) 横向分布: x 方差远大于 y 方差, 说明骨架是横着摊开的 ----
    if (img_w > 0 && img_h > 0 && core_vis >= 3) {
        std::vector<float> xs, ys;
        xs.reserve(9);
        ys.reserve(9);
        for (int i : kScoreKeys) {
            if (!vis[i]) continue;
            xs.push_back(k[i].x);
            ys.push_back(k[i].y);
        }
        if (xs.size() >= 4) {
            const float yv = variance(ys);
            if (yv > 1e-6f) {
                const float r = variance(xs) / yv;
                if (r > 3.0f) {
                    score += 2.0f;
                } else if (r > 1.5f) {
                    score += 1.0f;
                }
            }
            float ymin = ys[0];
            float ymax = ys[0];
            for (float y : ys) {
                ymin = std::min(ymin, y);
                ymax = std::max(ymax, y);
            }
            if ((ymax - ymin) / (float)img_h < 0.25f) score += 1.0f;
        }
    }

    v.feat.fall_score = score;

    // 硬门槛: 倒地姿态必须躯干至少半倾。坐姿躯干直立, 即便"腿水平 + 框偏扁 +
    // 贴底"凑到 3 分也不算倒地。这条是整个算法最关键的防误报。
    v.feat.posture = (score >= 3.0f && angle >= half_thr);

    // ---- 5) 髋部快速下坠: 必须同时倾角超标, 防止"坐下"被当成摔倒 ----
    if (params_.use_velocity && v.has_hip) {
        const int key = person_key(pose, track_id);
        std::deque<float>& hist = hip_history_[key];
        hist.push_back(hip_y);
        while ((int)hist.size() > 10) hist.pop_front();

        if (hist.size() >= 2) {
            const int win = std::max(1, params_.velocity_window);
            const int start = std::max(0, (int)hist.size() - win);
            const int n = (int)hist.size() - start;
            float drop = 0.0f;
            if (n > 1) {
                float mn = hist[(size_t)start];
                for (int i = start; i < start + n - 1; ++i) mn = std::min(mn, hist[(size_t)i]);
                drop = hist[(size_t)(start + n - 1)] - mn;
            }
            v.feat.hip_drop = drop;
            v.feat.vel_ok = (drop >= params_.velocity_drop_px) && v.feat.angle_ok;
        }
    }

    v.fallen = v.feat.posture || v.feat.vel_ok;
    return v;
}

std::vector<RuleEvent> FallDetector::update(const std::vector<PoseResult>& poses,
                                            const std::vector<Track>& tracks, int img_w,
                                            int img_h, uint64_t ts_ms) {
    std::vector<RuleEvent> events;

    // 低分姿态不解码, 和高分检测一样会带来抖动
    std::vector<PoseResult> dets;
    dets.reserve(poses.size());
    for (const PoseResult& p : poses) {
        if (p.score >= params_.min_person_score) dets.push_back(p);
    }

    if (dets.empty()) {
        fallen_count_ = 0;
        features_.clear();
        // 没人不重置 DOWN: 人倒下后扭出画面边缘, 不该把告警悄悄消掉。
        return events;
    }

    const std::vector<int> ids = match_poses_to_tracks(dets, tracks);

    struct Result {
        bool fallen = false;
        int id = -1;
        const PoseResult* pose = nullptr;
        FallFeatures feat;
    };
    std::vector<Result> results;
    results.reserve(dets.size());
    std::set<int> alive;

    for (size_t i = 0; i < dets.size(); ++i) {
        const PersonVerdict v = judge(dets[i], img_w, img_h, ids[i]);
        Result r;
        r.fallen = v.fallen;
        r.id = ids[i];
        r.pose = &dets[i];
        r.feat = v.feat;
        results.push_back(r);

        const int key = person_key(dets[i], ids[i]);
        alive.insert(key);
        features_[key] = v.feat;
    }

    // 清掉已经离开画面的人, 避免长跑后 map 无限增长
    for (auto it = hip_history_.begin(); it != hip_history_.end();) {
        it = alive.count(it->first) ? std::next(it) : hip_history_.erase(it);
    }
    for (auto it = features_.begin(); it != features_.end();) {
        it = alive.count(it->first) ? std::next(it) : features_.erase(it);
    }

    // 任一目标倒地即算有人倒地; 恢复要所有人都站回来。
    const Result* best = nullptr;
    for (const Result& r : results) {
        if (!r.fallen) continue;
        if (!best || r.feat.fall_score > best->feat.fall_score) best = &r;
    }
    const bool anyone_fallen = best != nullptr;

    if (!down_) {
        fallen_count_ = anyone_fallen ? fallen_count_ + 1 : 0;
        const bool cooled =
            !has_event_ ||
            (ts_ms >= last_event_ms_ &&
             ts_ms - last_event_ms_ >= (uint64_t)(params_.cooldown_s * 1000.0f));
        if (fallen_count_ >= params_.min_trigger_frames && cooled) {
            down_ = true;
            upright_count_ = 0;
            last_event_ms_ = ts_ms;
            has_event_ = true;

            RuleEvent e;
            e.feature = "fall";
            e.kind = "fall";
            e.name = std::string("跌倒");
            e.cls_id = kPersonClsId;
            e.label = kPersonLabel;
            e.ts_ms = ts_ms;
            e.severity = Severity::Alarm;
            if (best) {
                e.track_id = best->id;
                e.x = (best->pose->x1 + best->pose->x2) * 0.5f;
                e.y = best->pose->y2;
                char buf[192];
                snprintf(buf, sizeof(buf),
                         "躯干 %.0f°, 得分 %.1f, 关键点可见 %.0f%%, 髋部下坠 %.0fpx",
                         best->feat.torso_angle, best->feat.fall_score,
                         best->feat.kpt_vis_ratio * 100.0f, best->feat.hip_drop);
                e.extra = buf;
            } else {
                e.extra = "检测到倒地姿态";
            }
            events.push_back(e);
        }
    } else {
        upright_count_ = anyone_fallen ? 0 : upright_count_ + 1;
        if (upright_count_ >= params_.recover_frames) {
            down_ = false;
            fallen_count_ = 0;
            upright_count_ = 0;
        }
    }

    return events;
}

void FallDetector::reset() {
    down_ = false;
    fallen_count_ = 0;
    upright_count_ = 0;
    last_event_ms_ = 0;
    has_event_ = false;
    hip_history_.clear();
    features_.clear();
}

}  // namespace mb
