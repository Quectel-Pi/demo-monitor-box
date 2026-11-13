#ifndef MONITOR_BOX_POSE_POST_H_
#define MONITOR_BOX_POSE_POST_H_

#include <string>
#include <vector>

#include "infer/rknn_engine.h"
#include "infer/types.h"

namespace mb {

constexpr int kNumKeypoints = 17;
constexpr int kMaxPoses = 64;

// COCO 17 关键点索引。跌倒判定里到处按数字取, 写名字比写裸下标好查。
enum KptIndex {
    KPT_NOSE = 0,
    KPT_L_EYE = 1, KPT_R_EYE = 2, KPT_L_EAR = 3, KPT_R_EAR = 4,
    KPT_L_SHOULDER = 5, KPT_R_SHOULDER = 6,
    KPT_L_ELBOW = 7, KPT_R_ELBOW = 8,
    KPT_L_WRIST = 9, KPT_R_WRIST = 10,
    KPT_L_HIP = 11, KPT_R_HIP = 12,
    KPT_L_KNEE = 13, KPT_R_KNEE = 14,
    KPT_L_ANKLE = 15, KPT_R_ANKLE = 16,
};

struct Keypoint {
    float x = 0.0f;
    float y = 0.0f;
    float conf = 0.0f;
};

// 一个人的姿态结果。坐标已还原到原始图像像素空间。
struct PoseResult {
    float score = 0.0f;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    Keypoint kpts[kNumKeypoints];
};

// COCO 骨架连线 (19 条), 画图用。
extern const int kSkeleton[19][2];

struct PoseParams {
    float conf_threshold = 0.35f;
    float nms_threshold = 0.45f;
};

// 姿态模型种类, 按输出张量形状自动识别, 配置里不用写。
enum class PoseModelKind {
    Unknown,
    YoloPose56,    // 单阶段 (bottom-up): 1 个输出 56 通道, 整帧或 ROI 都能跑, 自带人框
    RtmPoseSimcc,  // 两阶段 (top-down): 2 个输出 simcc_x/simcc_y, 只能按人 ROI 跑, 不出框
};

PoseModelKind pose_model_kind(const RknnEngine& engine);
const char* pose_model_kind_name(PoseModelKind kind);

// RTMPose 的输入长宽比 (W:H = 192:256)。裁人框时要先把框补成这个比例, 和 mmpose 的
// TopDownGetBboxCenterScale 一致, 否则人在模型输入里不居中, 关键点置信度明显下降。
constexpr float kRtmPoseBoxPadding = 1.25f;

// 解码 RTMPose SimCC 输出 (单人)。kpts 坐标经 lb 反算到 letterbox 之前的图像 (即 ROI 裁图)
// 坐标系; 调用方再加 ROI 偏移。置信度 = min(max(simcc_x), max(simcc_y)), 与 mmpose/rtmlib 相同。
bool rtmpose_postprocess(const RknnEngine& engine, const Letterbox& lb, Keypoint* kpts,
                         std::string* err);

// 解码 yolov8-pose (ultralytics 56 通道单输出) 并还原到原图。
//
// 板上那个 yolo_pose.rknn 的实测特征:
//   - 1 个输出, dims [1, 56, 8400], float16 (quant=0)
//   - 56 = 4 (cx,cy,w,h) + 1 (类别分) + 17*3 (关键点 x,y,conf)
// 注意这不是 airockchip 的 65 通道 DFL 格式, 所以不需要 softmax 反投影。
// 若哪天换成 airockchip 导出的模型, 这里会明确报错而不是静默出错。
bool pose_postprocess(const RknnEngine& engine, const PoseParams& params, const Letterbox& lb,
                      std::vector<PoseResult>* out, std::string* err);

}  // namespace mb

#endif  // MONITOR_BOX_POSE_POST_H_
