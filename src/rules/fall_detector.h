#ifndef MONITOR_BOX_RULES_FALL_DETECTOR_H_
#define MONITOR_BOX_RULES_FALL_DETECTOR_H_

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "infer/pose_post.h"
#include "rules/rule_types.h"
#include "track/tracker.h"

namespace mb {

// 跌倒判定参数。默认值取自 demo-fall-detect/config.yaml, 那个值是在 M2 上
// 对着 640x640 的 yolo_pose 调出来的, 别随手改。换成 RTMPose 后几何阈值不受影响;
// min_kpt_conf 按 RTMPose 的 SimCC 置信度 (清晰人体 0.6~0.9, 部分遮挡 0.45~0.7) 核过仍适用,
// 但还没在真实跌倒视频上回归过, 现场调参用 mb-capture --fall-debug 看每次判定的特征。
struct FallParams {
    float torso_angle_threshold = 55.0f;  // 躯干与垂直方向夹角 (度)
    float height_ratio_threshold = 1.2f;  // 关键点算出的身体高宽比
    float aspect_ratio_threshold = 1.05f; // bbox 兜底时的 高/宽
    float min_kpt_conf = 0.40f;
    float kpt_visible_ratio = 0.70f;      // 可信节点不足就不判, 防止侧面半身误报
    int min_trigger_frames = 15;          // 连续 N 次姿态结果倒地才触发 (按姿态节拍计, 5 次/秒约 3s)
    int recover_frames = 30;              // 连续 N 帧恢复才解除
    bool use_velocity = true;
    float velocity_drop_px = 110.0f;
    int velocity_window = 5;
    float min_person_score = 0.40f;
    float cooldown_s = 15.0f;
};

// 一个人的姿态特征, 只用于日志/网页展示, 便于现场调阈值。
struct FallFeatures {
    float kpt_vis_ratio = 0.0f;
    float torso_angle = 0.0f;
    float fall_score = 0.0f;
    bool angle_ok = false;
    bool ratio_ok = false;
    bool vel_ok = false;
    bool posture = false;
    float hip_drop = 0.0f;
    float hip_y = 0.0f;
};

// 跌倒判定状态机 (从 demo-fall-detect/fall_detector.py 移植)。
//
// 打分制: 躯干倾角 + 腿部水平 + 身体长宽比 + 贴地 + 横向分布, 总分 >= 3 且躯干
// 至少半倾才算"倒地姿态"。硬门槛是关键 —— 没有它, 坐姿(腿部水平+框偏扁+贴底)
// 一样能凑够 3 分。
//
// 状态机:
//   NORMAL --连续 min_trigger_frames 帧倒地姿态, 且距上次事件 >= cooldown_s--> 报 fall, 进 DOWN
//   DOWN   --连续 recover_frames 帧没人倒地---------------------------------> 回 NORMAL
// 画面里没人时不重置 DOWN: 人倒下后扭出画面边缘不该把告警消掉。
//
// 与 demo 的一处差异: 髋部下坠历史改用 track_id 做键, 而不是框中心按 48px 量化。
// 有跟踪 ID 就不用再做那个粗量化, 交叉/靠近时不会串人。
class FallDetector {
public:
    FallDetector() = default;
    explicit FallDetector(const FallParams& p) : params_(p) {}

    void set_params(const FallParams& p) { params_ = p; }
    const FallParams& params() const { return params_; }
    void reset();

    // poses: 当帧全部人体姿态; tracks 用来把姿态绑到轨迹 id 上 (为空时退化到
    // 按位置量化的键, 和 demo-fall-detect 的做法一致)。
    // img_w/img_h: 原图尺寸, 贴地和横向分布两个特征要用。
    // 返回本帧产生的事件 (0 或 1 条 'fall')。
    std::vector<RuleEvent> update(const std::vector<PoseResult>& poses,
                                  const std::vector<Track>& tracks, int img_w, int img_h,
                                  uint64_t ts_ms);

    // 最近一次判定的特征, 按 track_id 索引 (供 OSD/网页显示)。
    const std::map<int, FallFeatures>& features() const { return features_; }

    bool is_down() const { return down_; }
    int fallen_count() const { return fallen_count_; }

    // 姿态与轨迹的配对 (按 IoU 贪心)。返回每个 pose 对应的 track id, -1 表示没配上。
    static std::vector<int> match_poses_to_tracks(const std::vector<PoseResult>& poses,
                                                  const std::vector<Track>& tracks);

private:
    struct PersonVerdict {
        bool fallen = false;
        float hip_y = 0.0f;
        bool has_hip = false;
        FallFeatures feat;
    };

    PersonVerdict judge(const PoseResult& pose, int img_w, int img_h, int track_id);

    // 髋部历史/特征用键: 有跟踪 id 就用它, 没配上就退化成框中心按 48px 量化
    // (demo 的做法)。负键不会和从 1 开始的 track_id 撞。
    static int person_key(const PoseResult& pose, int track_id);

    FallParams params_;
    bool down_ = false;
    int fallen_count_ = 0;
    int upright_count_ = 0;
    uint64_t last_event_ms_ = 0;
    bool has_event_ = false;
    std::map<int, std::deque<float>> hip_history_;  // track_id -> 髋部 y 历史
    std::map<int, FallFeatures> features_;
};

}  // namespace mb

#endif  // MONITOR_BOX_RULES_FALL_DETECTOR_H_
