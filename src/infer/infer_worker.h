#ifndef MONITOR_BOX_INFER_INFER_WORKER_H_
#define MONITOR_BOX_INFER_INFER_WORKER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "infer/pose_post.h"
#include "infer/pose_runner.h"
#include "infer/preprocess.h"
#include "infer/rknn_engine.h"
#include "infer/types.h"
#include "infer/yolo_post.h"

namespace mb {

struct InferJob {
    std::shared_ptr<const ImageRGB> img;  // 整帧拷贝; 推理期间主线程继续用自己的那份
    uint64_t ts_ms = 0;
    bool run_det = true;
    bool run_pose = false;
    bool run_fire = false;  // 火焰模型独立整帧 letterbox, 节拍由 models.fire.fps 决定
    YoloParams yolo;
    YoloParams fire;
    PoseParams pose;
    PoseRoiParams pose_roi;
    // 额外的 ROI 提示框 (原图坐标), 一般是主线程上一轮的 person 轨迹 (含卡尔曼预测的丢失轨迹)。
    // 人躺倒后检测器经常漏检, 没有它 ROI 模式会在最关键的时候停跑姿态。
    std::vector<Detection> pose_hints;
    PreprocessOptions pre;
};

struct InferResult {
    uint64_t ts_ms = 0;
    int img_w = 0;
    int img_h = 0;
    bool has_det = false;
    std::vector<Detection> dets;
    bool has_pose = false;
    std::vector<PoseResult> poses;
    bool has_fire = false;
    std::vector<Detection> fire_dets;
    double ai_ms = 0.0;   // 本次任务的 NPU + 前后处理总耗时
    std::string error;    // 非空 = 本次失败 (检测或姿态), 结果里对应部分无效
};

// 推理线程: 让视频通路 (解码 -> OSD -> 编码/预览) 不再等 NPU。
//
// 以前检测 (~28ms) 和姿态 (~60ms) 都串在主循环里: 推理帧的处理时间超过 33ms,
// 一开跌倒采集就从 30 掉到 18 fps, APP 画面和告警录像跟着卡。现在主循环只在
// 推理线程空闲时把当前帧拷一份交过来, 拿到结果后再更新跟踪/规则; 画面始终按
// 采集帧率走, 框比画面晚一次推理 (约 30~90ms)。
//
// 一次只排一个任务: 推理慢了就少推几帧, 绝不积压 (积压只会让结果越来越旧)。
// 两个 RknnEngine 由调用方持有; 交给本线程后只在本线程里 run()。
// 火焰模型同理 (第三个 engine, 可为空)。
class InferWorker {
public:
    InferWorker() = default;
    ~InferWorker();

    void start(RknnEngine* det, RknnEngine* pose, RknnEngine* fire = nullptr);
    void stop();

    bool busy() const { return busy_; }
    // 线程空闲时接受任务并返回 true; 忙时返回 false (调用方本帧跳过推理)。
    bool submit(InferJob job);
    // 非阻塞取结果。
    bool poll(InferResult* out);

private:
    void loop();
    void run_job(const InferJob& job, InferResult* r);

    RknnEngine* det_ = nullptr;
    RknnEngine* pose_ = nullptr;
    RknnEngine* fire_ = nullptr;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool running_ = false;
    std::atomic<bool> busy_{false};
    bool has_job_ = false;
    InferJob job_;
    bool has_result_ = false;
    InferResult result_;

    // 线程内复用的模型输入缓冲
    ImageRGB det_in_;
    ImageRGB fire_in_;
    PoseRunner pose_runner_;
};

}  // namespace mb

#endif  // MONITOR_BOX_INFER_INFER_WORKER_H_
