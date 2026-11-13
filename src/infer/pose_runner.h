#ifndef MONITOR_BOX_INFER_POSE_RUNNER_H_
#define MONITOR_BOX_INFER_POSE_RUNNER_H_

#include <string>
#include <vector>

#include "infer/pose_post.h"
#include "infer/preprocess.h"
#include "infer/rknn_engine.h"
#include "infer/types.h"

namespace mb {

// 按人 ROI 跑姿态 (top-down): 用检测出的 person 框外扩后裁剪送 pose, 而不是整帧 letterbox。
// 远处的人在整帧 640 画布上只有几十像素, 关键点基本是噪声; 裁出来再放大到模型输入,
// 关键点精度明显好, 场上没人时也不跑 pose。
struct PoseRoiParams {
    bool enabled = true;      // 只对 YOLO-pose 有意义; RTMPose 只能按人跑, 忽略此开关
    int person_cls_id = 0;    // 检测模型里 person 的类别号 (COCO = 0)
    int max_persons = 3;      // 每次最多送几个人 (按面积取大的), 每人一次 pose 推理
    float expand = 1.25f;     // YOLO-pose 的框外扩倍数; RTMPose 固定用 kRtmPoseBoxPadding
    int min_size = 24;        // 外扩后短边小于这个像素就不送 (太小出不了可靠关键点)
};

// 姿态推理调度: 整帧 / 按人 ROI, 两种模型 (YOLO-pose / RTMPose) 统一出 PoseResult。
// mb-capture 的推理线程和 mb-infer 共用这一份, 离线验证跑的就是线上路径。
// 不是线程安全的 (内部复用缓冲), 一个线程一个实例。
class PoseRunner {
public:
    // dets: 本帧检测结果, nullptr = 本帧没跑检测/检测失败。
    // hints: 额外 ROI (原图坐标), 一般是上一轮的 person 轨迹, 兜住躺倒后检测器漏检的人。
    // 没有任何 ROI 来源时: YOLO-pose 退回整帧, RTMPose 输出空 (它没法整帧跑)。
    bool run(RknnEngine& engine, const ImageRGB& img, const std::vector<Detection>* dets,
             const std::vector<Detection>& hints, const PoseParams& pp, const PoseRoiParams& rp,
             const PreprocessOptions& pre, std::vector<PoseResult>* out, std::string* err);

    // 最近一次 run() 实际送进 pose 模型的次数 (ROI 数或 1), 用于统计耗时。
    int last_runs() const { return last_runs_; }

private:
    bool run_full(RknnEngine& engine, const ImageRGB& img, const PoseParams& pp,
                  const PreprocessOptions& pre, std::vector<PoseResult>* out, std::string* err);

    ImageRGB in_;
    ImageRGB roi_;
    int last_runs_ = 0;
};

// 把 src 的 [x, y, x+w, y+h) 拷到 dst; 超出原图的部分填 fill (RTMPose 训练时仿射越界填 0)。
void crop_rgb(const ImageRGB& src, int x, int y, int w, int h, uint8_t fill, ImageRGB* dst);

float box_iou(const Detection& a, const Detection& b);

}  // namespace mb

#endif  // MONITOR_BOX_INFER_POSE_RUNNER_H_
