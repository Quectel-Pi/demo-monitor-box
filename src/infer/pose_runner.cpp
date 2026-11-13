#include "infer/pose_runner.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mb {

float box_iou(const Detection& a, const Detection& b) {
    const float iw = std::min(a.x2, b.x2) - std::max(a.x1, b.x1);
    const float ih = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    const float inter = iw * ih;
    const float uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

void crop_rgb(const ImageRGB& src, int x, int y, int w, int h, uint8_t fill, ImageRGB* dst) {
    dst->width = w;
    dst->height = h;
    dst->data.assign((size_t)w * h * 3, fill);
    const int sx0 = std::max(0, x);
    const int sx1 = std::min(src.width, x + w);
    if (sx1 <= sx0) return;
    for (int r = 0; r < h; ++r) {
        const int sy = y + r;
        if (sy < 0 || sy >= src.height) continue;
        memcpy(dst->row(r) + (size_t)(sx0 - x) * 3, src.row(sy) + (size_t)sx0 * 3,
               (size_t)(sx1 - sx0) * 3);
    }
}

namespace {

Detection box_of(const PoseResult& p) {
    Detection d;
    d.x1 = p.x1;
    d.y1 = p.y1;
    d.x2 = p.x2;
    d.y2 = p.y2;
    d.score = p.score;
    return d;
}

void shift_pose(PoseResult* p, float dx, float dy) {
    p->x1 += dx;
    p->x2 += dx;
    p->y1 += dy;
    p->y2 += dy;
    for (Keypoint& k : p->kpts) {
        k.x += dx;
        k.y += dy;
    }
}

}  // namespace

bool PoseRunner::run_full(RknnEngine& engine, const ImageRGB& img, const PoseParams& pp,
                          const PreprocessOptions& pre, std::vector<PoseResult>* out,
                          std::string* err) {
    in_.width = engine.input_width();
    in_.height = engine.input_height();
    Letterbox lb;
    const char* warn = nullptr;
    if (!letterbox_rgb(img, &in_, &lb, pre, &warn)) {
        if (err) *err = "letterbox 失败";
        return false;
    }
    last_runs_ = 1;
    return engine.run(in_.data.data(), in_.size_bytes(), err) &&
           pose_postprocess(engine, pp, lb, out, err);
}

bool PoseRunner::run(RknnEngine& engine, const ImageRGB& img, const std::vector<Detection>* dets,
                     const std::vector<Detection>& hints, const PoseParams& pp,
                     const PoseRoiParams& rp, const PreprocessOptions& pre,
                     std::vector<PoseResult>* out, std::string* err) {
    out->clear();
    last_runs_ = 0;
    const PoseModelKind kind = pose_model_kind(engine);
    if (kind == PoseModelKind::Unknown) {
        if (err) *err = "认不出姿态模型输出格式 (支持 yolov8-pose 56 通道 / RTMPose SimCC)";
        return false;
    }
    const bool topdown = kind == PoseModelKind::RtmPoseSimcc;
    const bool has_source = dets != nullptr || !hints.empty();
    if (!topdown && (!rp.enabled || !has_source)) {
        return run_full(engine, img, pp, pre, out, err);
    }

    std::vector<Detection> rois;
    if (dets) {
        for (const Detection& d : *dets) {
            if (d.cls_id == rp.person_cls_id) rois.push_back(d);
        }
    }
    const size_t n_det = rois.size();
    for (const Detection& h : hints) {
        bool dup = false;
        for (size_t i = 0; i < n_det && !dup; ++i) dup = box_iou(h, rois[i]) > 0.3f;
        if (!dup) rois.push_back(h);
    }
    std::stable_sort(rois.begin(), rois.end(), [](const Detection& a, const Detection& b) {
        return (a.x2 - a.x1) * (a.y2 - a.y1) > (b.x2 - b.x1) * (b.y2 - b.y1);
    });
    if (rp.max_persons >= 0 && rois.size() > (size_t)rp.max_persons) {
        rois.resize((size_t)rp.max_persons);
    }

    in_.width = engine.input_width();
    in_.height = engine.input_height();
    std::vector<PoseResult> found;
    for (const Detection& d : rois) {
        const float cx = (d.x1 + d.x2) * 0.5f;
        const float cy = (d.y1 + d.y2) * 0.5f;
        float w = d.x2 - d.x1;
        float h = d.y2 - d.y1;
        int x0, y0, x1, y1;
        if (topdown) {
            // mmpose TopDownGetBboxCenterScale: 外扩后把框补到模型输入长宽比, 越界不截断
            w *= kRtmPoseBoxPadding;
            h *= kRtmPoseBoxPadding;
            const float aspect = (float)in_.width / (float)in_.height;
            if (w > h * aspect) {
                h = w / aspect;
            } else {
                w = h * aspect;
            }
            x0 = (int)std::floor(cx - w * 0.5f);
            y0 = (int)std::floor(cy - h * 0.5f);
            x1 = (int)std::ceil(cx + w * 0.5f);
            y1 = (int)std::ceil(cy + h * 0.5f);
            // 框完全在画面外 (轨迹预测飘出去了) 时不跑
            if (x1 <= 0 || y1 <= 0 || x0 >= img.width || y0 >= img.height) continue;
        } else {
            w *= rp.expand;
            h *= rp.expand;
            x0 = std::max(0, (int)std::floor(cx - w * 0.5f));
            y0 = std::max(0, (int)std::floor(cy - h * 0.5f));
            x1 = std::min(img.width, (int)std::ceil(cx + w * 0.5f));
            y1 = std::min(img.height, (int)std::ceil(cy + h * 0.5f));
        }
        if (x1 - x0 < rp.min_size || y1 - y0 < rp.min_size) continue;

        crop_rgb(img, x0, y0, x1 - x0, y1 - y0, 0, &roi_);
        Letterbox lb;
        const char* warn = nullptr;
        if (!letterbox_rgb(roi_, &in_, &lb, pre, &warn)) {
            if (err) *err = "ROI letterbox 失败";
            return false;
        }
        if (!engine.run(in_.data.data(), in_.size_bytes(), err)) return false;
        ++last_runs_;

        if (topdown) {
            PoseResult p;
            if (!rtmpose_postprocess(engine, lb, p.kpts, err)) return false;
            // RTMPose 不出框, 人框和分数沿用检测/轨迹
            p.score = d.score;
            p.x1 = d.x1 - (float)x0;
            p.y1 = d.y1 - (float)y0;
            p.x2 = d.x2 - (float)x0;
            p.y2 = d.y2 - (float)y0;
            shift_pose(&p, (float)x0, (float)y0);
            found.push_back(p);
            continue;
        }

        std::vector<PoseResult> poses;
        if (!pose_postprocess(engine, pp, lb, &poses, err)) return false;
        // 外扩后的 ROI 里可能夹着旁边人的半个身子, 只取和这个人框最吻合的那个姿态
        int best = -1;
        float best_iou = 0.1f;
        for (size_t i = 0; i < poses.size(); ++i) {
            shift_pose(&poses[i], (float)x0, (float)y0);
            const float v = box_iou(box_of(poses[i]), d);
            if (v > best_iou) {
                best_iou = v;
                best = (int)i;
            }
        }
        if (best >= 0) found.push_back(poses[(size_t)best]);
    }

    // 两个框重叠时可能各自取到同一个人, 跨 ROI 再做一次 NMS
    std::sort(found.begin(), found.end(),
              [](const PoseResult& a, const PoseResult& b) { return a.score > b.score; });
    for (const PoseResult& p : found) {
        bool dup = false;
        for (const PoseResult& q : *out) {
            if (box_iou(box_of(p), box_of(q)) > pp.nms_threshold) {
                dup = true;
                break;
            }
        }
        if (!dup) out->push_back(p);
    }
    return true;
}

}  // namespace mb
