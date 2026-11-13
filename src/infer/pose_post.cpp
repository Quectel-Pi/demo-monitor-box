#include "infer/pose_post.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mb {

const int kSkeleton[19][2] = {
    {15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12}, {5, 6},  {5, 7},
    {6, 8},   {7, 9},   {8, 10},  {1, 2},   {0, 1},   {0, 2},  {1, 3}, {2, 4},  {3, 5},
    {4, 6},
};

namespace {

float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// float16 -> float32。板上模型的输出是 float16, rknn_outputs_get 在 want_float=1 时
// 已经转成 float32 返回, 所以这里只在检测到裸 f16 时兜底。
float half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    if (exp == 0) {
        if (mant == 0) {
            float f;
            const uint32_t bits = sign;
            memcpy(&f, &bits, 4);
            return f;
        }
        // 次正规数: 归一化
        exp = 127 - 15;
        while ((mant & 0x400u) == 0) {
            mant <<= 1;
            exp -= 1;
        }
        mant &= 0x3FFu;
        const uint32_t bits = sign | (exp << 23) | (mant << 13);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    if (exp == 0x1F) {  // inf / nan
        const uint32_t bits = sign | 0x7F800000u | (mant << 13);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    const uint32_t bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// 把输出张量统一看成 float 序列。量化模型输出 int8 + affine, 这里解成 float。
bool as_floats(const RknnEngine::OutputTensor& t, std::vector<float>* out, std::string* err) {
    // 只乘有效维度: 板上模型是 3 维 [1,56,8400], dims[3] 恒为 0, 四个全乘会得到 0。
    size_t n = t.n_dims > 0 ? 1 : 0;
    for (uint32_t d = 0; d < t.n_dims && d < 4; ++d) n *= t.dims[d];
    if (n == 0) {
        if (err) *err = "姿态模型输出维度全 0";
        return false;
    }
    out->resize(n);
    const size_t bytes = t.data.size();
    if (bytes == n * 4) {
        memcpy(out->data(), t.data.data(), bytes);
        return true;
    }
    if (bytes == n * 2) {
        const uint16_t* p = (const uint16_t*)t.data.data();
        for (size_t i = 0; i < n; ++i) (*out)[i] = half_to_float(p[i]);
        return true;
    }
    if (bytes == n) {
        // int8 + affine 量化
        const int8_t* p = (const int8_t*)t.data.data();
        for (size_t i = 0; i < n; ++i) (*out)[i] = ((float)p[i] - (float)t.zp) * t.scale;
        return true;
    }
    if (err) {
        *err = "姿态模型输出长度不对: " + std::to_string(bytes) + " 字节 / " + std::to_string(n) +
               " 个元素";
    }
    return false;
}

struct Box {
    float x1, y1, x2, y2, score;
};

float iou_of(const Box& a, const Box& b) {
    const float ix1 = std::max(a.x1, b.x1);
    const float iy1 = std::max(a.y1, b.y1);
    const float ix2 = std::min(a.x2, b.x2);
    const float iy2 = std::min(a.y2, b.y2);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    const float inter = iw * ih;
    const float uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

}  // namespace

namespace {

// SimCC 输出 [1, K, L] (个别导出会多一个尾部 1)。返回 K 和 L, 认不出返回 false。
bool simcc_shape(const RknnEngine::OutputTensor& t, uint32_t* k, uint32_t* l) {
    uint32_t d[4];
    uint32_t nd = 0;
    for (uint32_t i = 0; i < t.n_dims && i < 4; ++i) {
        if (t.dims[i] != 1) d[nd++] = t.dims[i];
    }
    if (nd != 2 || d[0] == 0 || d[1] == 0) return false;
    *k = d[0];
    *l = d[1];
    return true;
}

// 找出 simcc_x / simcc_y 两个输出的下标与 split 比例。
bool simcc_layout(const RknnEngine& engine, int* ix, int* iy, float* split) {
    const auto& outs = engine.outputs();
    if (outs.size() != 2) return false;
    uint32_t k0, l0, k1, l1;
    if (!simcc_shape(outs[0], &k0, &l0) || !simcc_shape(outs[1], &k1, &l1)) return false;
    if (k0 != (uint32_t)kNumKeypoints || k1 != (uint32_t)kNumKeypoints) return false;
    const int w = engine.input_width();
    const int h = engine.input_height();
    if (w <= 0 || h <= 0) return false;
    auto match = [&](uint32_t lx, uint32_t ly, float* s) {
        if (lx % (uint32_t)w != 0 || ly % (uint32_t)h != 0) return false;
        if (lx / (uint32_t)w != ly / (uint32_t)h || lx / (uint32_t)w == 0) return false;
        *s = (float)(lx / (uint32_t)w);
        return true;
    };
    if (match(l0, l1, split)) {
        *ix = 0;
        *iy = 1;
        return true;
    }
    if (match(l1, l0, split)) {
        *ix = 1;
        *iy = 0;
        return true;
    }
    return false;
}

}  // namespace

PoseModelKind pose_model_kind(const RknnEngine& engine) {
    const auto& outs = engine.outputs();
    if (outs.size() == 1) {
        const auto& t = outs[0];
        for (uint32_t i = 1; i < t.n_dims && i < 4; ++i) {
            if (t.dims[i] == 56) return PoseModelKind::YoloPose56;
        }
        return PoseModelKind::Unknown;
    }
    int ix, iy;
    float split;
    if (simcc_layout(engine, &ix, &iy, &split)) return PoseModelKind::RtmPoseSimcc;
    return PoseModelKind::Unknown;
}

const char* pose_model_kind_name(PoseModelKind kind) {
    switch (kind) {
        case PoseModelKind::YoloPose56: return "yolov8-pose (56ch, bottom-up)";
        case PoseModelKind::RtmPoseSimcc: return "rtmpose (simcc, top-down)";
        default: return "unknown";
    }
}

bool rtmpose_postprocess(const RknnEngine& engine, const Letterbox& lb, Keypoint* kpts,
                         std::string* err) {
    int ix, iy;
    float split;
    if (!simcc_layout(engine, &ix, &iy, &split)) {
        if (err) *err = "不是 RTMPose SimCC 输出 (期望 2 个输出 [1,17,W*k] / [1,17,H*k])";
        return false;
    }
    const auto& outs = engine.outputs();
    std::vector<float> fx, fy;
    if (!as_floats(outs[(size_t)ix], &fx, err) || !as_floats(outs[(size_t)iy], &fy, err)) {
        return false;
    }
    const size_t lx = fx.size() / kNumKeypoints;
    const size_t ly = fy.size() / kNumKeypoints;
    for (int k = 0; k < kNumKeypoints; ++k) {
        const float* px = fx.data() + (size_t)k * lx;
        const float* py = fy.data() + (size_t)k * ly;
        const size_t ax = (size_t)(std::max_element(px, px + lx) - px);
        const size_t ay = (size_t)(std::max_element(py, py + ly) - py);
        const float conf = std::min(px[ax], py[ay]);
        kpts[k].x = ((float)ax / split - (float)lb.x_pad) / lb.scale;
        kpts[k].y = ((float)ay / split - (float)lb.y_pad) / lb.scale;
        kpts[k].conf = conf > 0.0f ? conf : 0.0f;
    }
    return true;
}

bool pose_postprocess(const RknnEngine& engine, const PoseParams& params, const Letterbox& lb,
                      std::vector<PoseResult>* out, std::string* err) {
    out->clear();
    const auto& outs = engine.outputs();
    if (outs.size() != 1) {
        if (err) {
            *err = "姿态模型输出数 = " + std::to_string(outs.size()) +
                   ", 期望 1 (板上 yolo_pose.rknn 是单输出 56x8400)";
        }
        return false;
    }
    const RknnEngine::OutputTensor& t = outs[0];

    // dims 可能是 [1,56,8400] 也可能被压成 [1,8400,56]; 靠通道数判断。
    uint32_t c = 0, n = 0;
    if (t.n_dims == 3) {
        if (t.dims[1] == 56) {
            c = t.dims[1];
            n = t.dims[2];
        } else if (t.dims[2] == 56) {
            c = t.dims[2];
            n = t.dims[1];
        }
    } else if (t.n_dims == 4) {
        if (t.dims[1] == 56) {
            c = t.dims[1];
            n = t.dims[2] * t.dims[3];
        } else if (t.dims[3] == 56) {
            c = t.dims[3];
            n = t.dims[1] * t.dims[2];
        }
    }
    if (c == 65 || c == 64 + 1 + 51) {
        if (err) {
            *err = "这是 airockchip 的 65 通道 DFL 姿态模型, 当前只实现了 56 通道 ultralytics 路径";
        }
        return false;
    }
    if (c != 56 || n == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "姿态模型输出 shape 不认识: n_dims=%u dims=[%u,%u,%u,%u]",
                 t.n_dims, t.dims[0], t.dims[1], t.dims[2], t.dims[3]);
        if (err) *err = buf;
        return false;
    }

    std::vector<float> f;
    if (!as_floats(t, &f, err)) return false;
    if (f.size() < (size_t)c * n) {
        if (err) *err = "姿态输出元素数不足";
        return false;
    }

    // 布局: 连续 [c][n] 还是 [n][c]? rknn 的 dims[1]=56 意味着通道在前。
    const bool channel_first = (t.n_dims >= 2 && t.dims[1] == 56);
    auto at = [&](uint32_t ch, uint32_t i) -> float {
        return channel_first ? f[(size_t)ch * n + i] : f[(size_t)i * c + ch];
    };

    // 一次扫过所有 anchor, 解码成框, 顺手算关键点。
    struct Cand {
        Box box;
        Keypoint k[kNumKeypoints];
    };
    std::vector<Cand> cands;
    cands.reserve(256);

    const int in_w = engine.input_width();
    const int in_h = engine.input_height();
    (void)in_h;

    for (uint32_t i = 0; i < n; ++i) {
        float score = at(4, i);
        // ultralytics 导出分数已 sigmoid; 万一模型给的是 logit, 这里补一下。
        if (score < 0.0f || score > 1.0f) score = sigmoid(score);
        if (score < params.conf_threshold) continue;

        const float cx = at(0, i);
        const float cy = at(1, i);
        const float bw = at(2, i);
        const float bh = at(3, i);
        // 坐标本来就以输入尺度 (640) 为准, 个别导出给归一化值, 这里兜一下。
        const float sx = (cx <= 1.5f && bw <= 1.5f) ? (float)in_w : 1.0f;
        const float sy = (cy <= 1.5f && bh <= 1.5f) ? (float)in_h : 1.0f;

        Cand cand;
        cand.box.x1 = (cx - bw * 0.5f) * sx;
        cand.box.y1 = (cy - bh * 0.5f) * sy;
        cand.box.x2 = (cx + bw * 0.5f) * sx;
        cand.box.y2 = (cy + bh * 0.5f) * sy;
        cand.box.score = score;
        for (int k = 0; k < kNumKeypoints; ++k) {
            float kx = at((uint32_t)(5 + k * 3), i);
            float ky = at((uint32_t)(6 + k * 3), i);
            float kc = at((uint32_t)(7 + k * 3), i);
            if (kc < 0.0f || kc > 1.0f) kc = sigmoid(kc);
            // 关键点坐标同样是输入尺度 (ultralytics 直接给的像素)。归一化模型很少见,
            // 用同一套 sx/sy 兜底。
            cand.k[k].x = kx * sx;
            cand.k[k].y = ky * sy;
            cand.k[k].conf = kc;
        }
        cands.push_back(cand);
    }

    // 按分数从高到低做 NMS。候选必须先全收再排序截断: 按 anchor 顺序提前 break 会
    // 只留下 stride-8 网格上同一个人的重复框, 后面的人 (和更高分的框) 全丢。
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.box.score > b.box.score; });
    if (cands.size() > (size_t)kMaxPoses * 16) cands.resize((size_t)kMaxPoses * 16);
    std::vector<int> keep;
    for (size_t i = 0; i < cands.size(); ++i) {
        bool suppressed = false;
        for (int j : keep) {
            if (iou_of(cands[i].box, cands[(size_t)j].box) > params.nms_threshold) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) {
            keep.push_back((int)i);
            if (keep.size() >= (size_t)kMaxPoses) break;
        }
    }

    // letterbox 反变换: 640 画布 -> 原图。
    auto back_x = [&](float x) { return (x - (float)lb.x_pad) / lb.scale; };
    auto back_y = [&](float y) { return (y - (float)lb.y_pad) / lb.scale; };

    out->reserve(keep.size());
    for (int idx : keep) {
        const Cand& c = cands[(size_t)idx];
        PoseResult r;
        r.score = c.box.score;
        r.x1 = back_x(c.box.x1);
        r.y1 = back_y(c.box.y1);
        r.x2 = back_x(c.box.x2);
        r.y2 = back_y(c.box.y2);
        for (int k = 0; k < kNumKeypoints; ++k) {
            r.kpts[k].x = back_x(c.k[k].x);
            r.kpts[k].y = back_y(c.k[k].y);
            r.kpts[k].conf = c.k[k].conf;
        }
        out->push_back(r);
    }
    return true;
}

}  // namespace mb
