#ifndef MONITOR_BOX_TRACK_TRACKER_H_
#define MONITOR_BOX_TRACK_TRACKER_H_

#include <cstdint>
#include <map>
#include <vector>

#include "infer/types.h"

namespace mb {

// 一条跟踪轨迹。坐标都在原图像素空间, 脚点 = 框底边中点。
struct Track {
    int id = 0;
    int cls_id = 0;
    float score = 0.0f;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;

    int age = 0;          // 首次出现至今的帧数
    int hits = 0;         // 命中次数
    int lost = 0;         // 连续丢失帧数
    bool confirmed = false;  // hits >= min_hits 才算正式目标, 避免一帧误检就算入侵

    // 平稳后的速度 (像素/帧), 用于方向轨迹判断
    float vx = 0.0f;
    float vy = 0.0f;

    float cx() const { return (x1 + x2) * 0.5f; }
    float cy() const { return (y1 + y2) * 0.5f; }
    float foot_x() const { return (x1 + x2) * 0.5f; }
    float foot_y() const { return y2; }
    float width() const { return x2 - x1; }
    float height() const { return y2 - y1; }
};

struct TrackerParams {
    float high_threshold = 0.5f;   // 第一轮关联用的高置信度门槛
    float low_threshold = 0.1f;    // 第二轮 (ByteTrack) 低分补关联
    float iou_threshold = 0.3f;    // 关联的最小 IoU
    int track_buffer = 30;         // 丢失多少帧后删除
    int min_hits = 3;              // 连续命中多少帧后才 confirmed
    bool enabled = true;
};

// ByteTrack 精简版: 卡尔曼 (恒定速度) + 匈牙利指派 + 两轮关联。
//
// 为什么是两轮: 人走过柱子、半个身子被挡时检测分会在 0.1~0.5 之间, 只保留高分
// 检测会把一条轨迹断成两条, 于是「滞留计时」清零、「越线计数」翻倍。
// 第一轮把高分检测和已有轨迹配, 第二轮用低分检测去捡剩下的轨迹。
//
// 本实现不做 ReID (外观特征): 单向场景下 IoU + 速度足够; 交叉换 ID 仍会发生,
// 这是纯 IoU 跟踪的固有限制, 写进 README 的已知限制。
class Tracker {
public:
    explicit Tracker(const TrackerParams& params = TrackerParams{});

    void set_params(const TrackerParams& p) { params_ = p; }
    const TrackerParams& params() const { return params_; }

    // 输入当帧检测 (坐标已在原图), 返回更新后的轨迹 (含未确认的)。
    // 调用方自己过滤 confirmed。
    const std::vector<Track>& update(const std::vector<Detection>& dets, int64_t frame_index);

    // 去掉指定类别的轨迹 (不同功能用不同类别时避免互相污染)。
    void reset();

private:
    struct KalmanState {
        float x[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // cx, cy, w, h, vcx, vcy, vw, vh
        float p[64] = {0};                       // 8x8 协方差
    };
    struct TrackImpl {
        Track t;
        KalmanState kf;
        uint64_t last_frame = 0;
        // 最近几次命中的类别加权分。检测模型在相近类别间来回跳时 (例如猫和背包),
        // 用时间上的多数证据稳定显示和规则使用的类别。
        std::map<int, float> class_scores;
    };

    void init_filter(TrackImpl* tr, const Detection& d);
    void predict(TrackImpl* tr);
    void correct(TrackImpl* tr, const Detection& d);
    void observe_class(TrackImpl* tr, const Detection& d);

    TrackerParams params_;
    std::vector<TrackImpl> tracks_;
    std::vector<Track> out_;
    int next_id_ = 1;
};

// 两框 IoU。
float iou(const Detection& a, const Detection& b);
float iou_boxes(float ax1, float ay1, float ax2, float ay2, float bx1, float by1, float bx2,
                float by2);

}  // namespace mb

#endif  // MONITOR_BOX_TRACK_TRACKER_H_
