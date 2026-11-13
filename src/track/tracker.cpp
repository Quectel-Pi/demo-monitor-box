#include "track/tracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace mb {
namespace {

constexpr float kBigCost = 1e6f;
// 未匹配的高分检测与已有轨迹 IoU 超过它, 视为同一目标的重复框
constexpr float kDuplicateIou = 0.7f;
// 同一位置前后帧类别不同时, 仍允许关联, 但比同类关联多这点代价。这样模型在
// cat/backpack 之间抖动不会把同一物体拆成两条轨迹, 重叠的不同类别目标也仍有优先级。
constexpr float kClassMismatchCost = 0.08f;
// 类别历史按帧衰减, 新类别需要在加权分上明显超过原类别才切换, 避免单帧误检闪标签。
constexpr float kClassScoreDecay = 0.8f;
constexpr float kClassSwitchMargin = 1.25f;

// 4x4 求逆 (用于卡尔曼纠正里的 S^-1)。奇异返回 false。
bool invert4(const double a[4][4], double out[4][4]) {
    double m[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[i][j] = a[i][j];
            m[i][j + 4] = (i == j) ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
        }
        if (std::fabs(m[pivot][col]) < 1e-12) return false;
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) std::swap(m[col][c], m[pivot][c]);
        }
        const double d = m[col][col];
        for (int c = 0; c < 8; ++c) m[col][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = m[r][col];
            if (f == 0.0) continue;
            for (int c = 0; c < 8; ++c) m[r][c] -= f * m[col][c];
        }
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) out[i][j] = m[i][j + 4];
    }
    return true;
}

// 匈牙利算法 (e-maxx 版)。要求 rows <= cols, cost 为 rows x cols。
// 返回每行分配到的列, 未分配为 -1。
void hungarian(const std::vector<float>& cost, int rows, int cols, std::vector<int>* row_to_col) {
    row_to_col->assign(rows, -1);
    if (rows <= 0 || cols <= 0) return;
    const double kInf = 1e18;
    std::vector<double> u(rows + 1, 0.0), v(cols + 1, 0.0);
    std::vector<int> p(cols + 1, 0), way(cols + 1, 0);
    for (int i = 1; i <= rows; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(cols + 1, kInf);
        std::vector<char> used(cols + 1, 0);
        do {
            used[j0] = 1;
            const int i0 = p[j0];
            int j1 = -1;
            double delta = kInf;
            for (int j = 1; j <= cols; ++j) {
                if (used[j]) continue;
                const double cur = (double)cost[(size_t)(i0 - 1) * cols + (j - 1)] - u[i0] - v[j];
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }
            if (j1 < 0) return;  // 不该发生; 保护一下
            for (int j = 0; j <= cols; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    for (int j = 1; j <= cols; ++j) {
        if (p[j] > 0) (*row_to_col)[p[j] - 1] = j - 1;
    }
}

}  // namespace

float iou_boxes(float ax1, float ay1, float ax2, float ay2, float bx1, float by1, float bx2,
                float by2) {
    const float ix1 = std::max(ax1, bx1);
    const float iy1 = std::max(ay1, by1);
    const float ix2 = std::min(ax2, bx2);
    const float iy2 = std::min(ay2, by2);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    const float inter = iw * ih;
    const float area_a = std::max(0.0f, ax2 - ax1) * std::max(0.0f, ay2 - ay1);
    const float area_b = std::max(0.0f, bx2 - bx1) * std::max(0.0f, by2 - by1);
    const float uni = area_a + area_b - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

float iou(const Detection& a, const Detection& b) {
    return iou_boxes(a.x1, a.y1, a.x2, a.y2, b.x1, b.y1, b.x2, b.y2);
}

Tracker::Tracker(const TrackerParams& params) : params_(params) {}

void Tracker::reset() {
    tracks_.clear();
    out_.clear();
    next_id_ = 1;
}

void Tracker::observe_class(TrackImpl* tr, const Detection& d) {
    for (auto& kv : tr->class_scores) kv.second *= kClassScoreDecay;
    tr->class_scores[d.cls_id] += std::max(0.05f, d.score);

    auto current = tr->class_scores.find(tr->t.cls_id);
    const float current_score = current == tr->class_scores.end() ? 0.0f : current->second;
    int best_cls = tr->t.cls_id;
    float best_score = current_score;
    for (const auto& kv : tr->class_scores) {
        if (kv.second > best_score) {
            best_cls = kv.first;
            best_score = kv.second;
        }
    }
    if (best_cls != tr->t.cls_id && best_score > current_score * kClassSwitchMargin) {
        tr->t.cls_id = best_cls;
    }
}

void Tracker::init_filter(TrackImpl* tr, const Detection& d) {
    KalmanState& k = tr->kf;
    memset(k.x, 0, sizeof(k.x));
    memset(k.p, 0, sizeof(k.p));
    float w = d.x2 - d.x1;
    float h = d.y2 - d.y1;
    if (w < 1.0f) w = 1.0f;
    if (h < 1.0f) h = 1.0f;
    k.x[0] = (d.x1 + d.x2) * 0.5f;
    k.x[1] = (d.y1 + d.y2) * 0.5f;
    k.x[2] = w;
    k.x[3] = h;

    // 不确定性按目标尺寸缩放: 远处的框本来就抖, 远处的框变化幅度也小。
    const float sp = h * (1.0f / 20.0f);
    const float sv = h * (1.0f / 160.0f);
    // 取 ByteTrack 的标准初值: 位置 std = 2*sp, 速度 std = 10*sv (刚出现时速度完全未知)
    for (int i = 0; i < 4; ++i) k.p[i * 8 + i] = 4.0f * sp * sp;
    for (int i = 4; i < 8; ++i) k.p[i * 8 + i] = 100.0f * sv * sv;
}

void Tracker::predict(TrackImpl* tr) {
    KalmanState& k = tr->kf;
    // 状态转移: 位置 += 速度, 速度不变。
    for (int i = 0; i < 4; ++i) k.x[i] += k.x[i + 4];

    // P = F P F^T。F = I + E (E[i][i+4]=1)。先右乘 a = P F^T, 再左乘 F。
    float a[64];
    for (int i = 0; i < 8; ++i) {
        for (int l = 0; l < 8; ++l) {
            a[i * 8 + l] = k.p[i * 8 + l] + (l + 4 < 8 ? k.p[i * 8 + l + 4] : 0.0f);
        }
    }
    float np[64];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            np[i * 8 + j] = a[i * 8 + j] + (i + 4 < 8 ? a[(i + 4) * 8 + j] : 0.0f);
        }
    }

    // 过程噪声: 匀速假设至少不成立到"速度会变", 给位置多留一点余量。
    const float h = std::max(1.0f, k.x[3]);
    const float sp = h * (1.0f / 20.0f);
    const float sv = h * (1.0f / 160.0f);
    // ByteTrack 的过程噪声。原来乘了 0.01, 滤波器过"硬", 速度几乎学不到, 走动的人一漏检就跟丢
    for (int i = 0; i < 4; ++i) np[i * 8 + i] += sp * sp;
    for (int i = 4; i < 8; ++i) np[i * 8 + i] += sv * sv;
    memcpy(k.p, np, sizeof(k.p));
}

void Tracker::correct(TrackImpl* tr, const Detection& d) {
    KalmanState& k = tr->kf;
    const float w = std::max(1.0f, k.x[3]);

    // 观测 y = z - Hx, H 取前 4 维。
    const double z[4] = {(double)((d.x1 + d.x2) * 0.5f), (double)((d.y1 + d.y2) * 0.5f),
                         (double)std::max(0.001f, d.x2 - d.x1),
                         (double)std::max(0.001f, d.y2 - d.y1)};
    double y[4];
    for (int i = 0; i < 4; ++i) y[i] = z[i] - k.x[i];

    // S = H P H^T + R, 即 P 的左上 4x4 + 观测噪声。
    const double sp = (double)w * (1.0 / 20.0);
    double s[4][4];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) s[i][j] = k.p[i * 8 + j];
        s[i][i] += sp * sp;
    }
    double s_inv[4][4];
    if (!invert4(s, s_inv)) return;

    // K = P H^T S^-1, 其中 P H^T 就是 P 的前 4 列。
    double k_gain[8][4];
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 4; ++j) {
            double sum = 0.0;
            for (int m = 0; m < 4; ++m) sum += (double)k.p[i * 8 + m] * s_inv[m][j];
            k_gain[i][j] = sum;
        }
    }
    for (int i = 0; i < 8; ++i) {
        double delta = 0.0;
        for (int j = 0; j < 4; ++j) delta += k_gain[i][j] * y[j];
        k.x[i] += (float)delta;
    }
    // P = (I - K H) P。
    float np[64];
    memcpy(np, k.p, sizeof(np));
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            double delta = 0.0;
            for (int m = 0; m < 4; ++m) delta += k_gain[i][m] * (double)k.p[m * 8 + j];
            np[i * 8 + j] -= (float)delta;
        }
    }
    // 数值保护: 协方差对角不能变负, 否则后面越跑越偏。
    for (int i = 0; i < 8; ++i) {
        if (!(np[i * 8 + i] > 1e-9f)) np[i * 8 + i] = 1e-9f;
    }
    memcpy(k.p, np, sizeof(k.p));
}

const std::vector<Track>& Tracker::update(const std::vector<Detection>& dets, int64_t frame_index) {
    (void)frame_index;
    if (!params_.enabled) {
        out_.clear();
        return out_;
    }

    for (TrackImpl& tr : tracks_) predict(&tr);

    // 分成高低两档。低分档只用来续命已有轨迹, 不会新建轨迹。
    std::vector<int> high, low;
    for (size_t i = 0; i < dets.size(); ++i) {
        if (dets[i].score >= params_.high_threshold) {
            high.push_back((int)i);
        } else if (dets[i].score >= params_.low_threshold) {
            low.push_back((int)i);
        }
    }

    std::vector<char> track_matched(tracks_.size(), 0);

    // 关联用卡尔曼预测框: 目标在动或丢了几帧时, 上一次的框已经和当前检测对不上,
    // 用旧框配会让 IoU 掉到门槛以下, 于是断链换 ID。
    std::vector<std::array<float, 4>> pred(tracks_.size());
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const KalmanState& k = tracks_[i].kf;
        const float w = std::max(1.0f, k.x[2]);
        const float h = std::max(1.0f, k.x[3]);
        pred[i] = {k.x[0] - w * 0.5f, k.x[1] - h * 0.5f, k.x[0] + w * 0.5f, k.x[1] + h * 0.5f};
    }
    std::vector<char> det_used(dets.size(), 0);

    // 两轮关联共用同一套代价矩阵构造。
    auto associate = [&](const std::vector<int>& cand, float gate) {
        if (tracks_.empty() || cand.empty()) return;
        std::vector<int> rows;
        for (size_t t = 0; t < tracks_.size(); ++t) {
            if (!track_matched[t]) rows.push_back((int)t);
        }
        if (rows.empty()) return;
        const int nr = (int)rows.size();
        const int nc = (int)cand.size();
        const int cols = std::max(nr, nc);

        std::vector<float> cost((size_t)nr * cols, kBigCost);
        for (int r = 0; r < nr; ++r) {
            const Track& t = tracks_[(size_t)rows[(size_t)r]].t;
            const std::array<float, 4>& pb = pred[(size_t)rows[(size_t)r]];
            for (int c = 0; c < nc; ++c) {
                const Detection& d = dets[(size_t)cand[(size_t)c]];
                // 预测框和上一次的框取 IoU 较大者: 刚起步速度估计还不准时不至于配丢
                const float ov = std::max(
                    iou_boxes(pb[0], pb[1], pb[2], pb[3], d.x1, d.y1, d.x2, d.y2),
                    iou_boxes(t.x1, t.y1, t.x2, t.y2, d.x1, d.y1, d.x2, d.y2));
                if (ov < gate) continue;
                cost[(size_t)r * cols + (size_t)c] = 1.0f - ov +
                    (d.cls_id == t.cls_id ? 0.0f : kClassMismatchCost);
            }
        }

        std::vector<int> assign;
        hungarian(cost, nr, cols, &assign);
        for (int r = 0; r < nr; ++r) {
            const int c = assign[(size_t)r];
            if (c < 0 || c >= nc) continue;
            if (cost[(size_t)r * cols + (size_t)c] >= kBigCost * 0.5f) continue;
            const int ti = rows[(size_t)r];
            TrackImpl& tr = tracks_[(size_t)ti];
            const Detection& d = dets[(size_t)cand[(size_t)c]];
            correct(&tr, d);
            tr.t.x1 = tr.kf.x[0] - tr.kf.x[2] * 0.5f;
            tr.t.y1 = tr.kf.x[1] - tr.kf.x[3] * 0.5f;
            tr.t.x2 = tr.kf.x[0] + tr.kf.x[2] * 0.5f;
            tr.t.y2 = tr.kf.x[1] + tr.kf.x[3] * 0.5f;
            tr.t.score = d.score;
            tr.t.vx = tr.kf.x[4];
            tr.t.vy = tr.kf.x[5];
            tr.t.hits += 1;
            tr.t.lost = 0;
            tr.t.age += 1;
            observe_class(&tr, d);
            tr.t.confirmed = tr.t.confirmed || tr.t.hits >= params_.min_hits;
            track_matched[(size_t)ti] = 1;
            det_used[(size_t)cand[(size_t)c]] = 1;
        }
    };

    associate(high, params_.iou_threshold);
    associate(low, params_.iou_threshold);

    // 没配上任何检测的轨迹: 丢一次分, 超过缓冲就删。
    std::vector<TrackImpl> kept;
    kept.reserve(tracks_.size());
    for (size_t i = 0; i < tracks_.size(); ++i) {
        TrackImpl& tr = tracks_[i];
        if (!track_matched[i]) {
            tr.t.lost += 1;
            tr.t.age += 1;
            tr.t.vx = tr.kf.x[4];
            tr.t.vy = tr.kf.x[5];
        }
        if (tr.t.lost > params_.track_buffer) continue;
        kept.push_back(tr);
    }
    tracks_.swap(kept);

    // 高分检测里没被任何轨迹吃掉的, 开新轨迹。
    // 只有和已匹配轨迹几乎重合 (NMS 漏掉的重复框) 才不开: 两个人挨着站时 IoU 本来就
    // 不低, 用关联门槛去判"已覆盖"会让后来的那个人一直没有轨迹, 入侵直接漏报。
    for (int di : high) {
        if (det_used[(size_t)di]) continue;
        const Detection& d = dets[(size_t)di];
        bool covered = false;
        for (const TrackImpl& tr : tracks_) {
            if (tr.t.lost != 0) continue;
            if (tr.t.cls_id != d.cls_id) continue;
            if (iou_boxes(tr.t.x1, tr.t.y1, tr.t.x2, tr.t.y2, d.x1, d.y1, d.x2, d.y2) >=
                kDuplicateIou) {
                covered = true;
                break;
            }
        }
        if (covered) continue;

        TrackImpl tr;
        tr.t.id = next_id_++;
        tr.t.cls_id = d.cls_id;
        tr.t.score = d.score;
        tr.t.x1 = d.x1;
        tr.t.y1 = d.y1;
        tr.t.x2 = d.x2;
        tr.t.y2 = d.y2;
        tr.t.age = 1;
        tr.t.hits = 1;
        tr.t.lost = 0;
        tr.t.confirmed = params_.min_hits <= 1;
        tr.class_scores[d.cls_id] = std::max(0.05f, d.score);
        init_filter(&tr, d);
        tracks_.push_back(tr);
    }

    out_.clear();
    out_.reserve(tracks_.size());
    for (const TrackImpl& tr : tracks_) out_.push_back(tr.t);
    return out_;
}

}  // namespace mb
