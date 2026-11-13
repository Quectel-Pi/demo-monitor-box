#ifndef MONITOR_BOX_VIDEO_VIEW_GUARD_H_
#define MONITOR_BOX_VIDEO_VIEW_GUARD_H_

#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/geometry.h"
#include "common/json.h"
#include "infer/types.h"

namespace mb {

// 小灰度图, 画面对齐只在 320 宽的缩略图上做。
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
};

// 按块平均缩小并转灰度。
void downscale_gray(const ImageRGB& src, int w, int h, GrayImage* out);

// 归一化框 (当前画面里的人/车, 对齐时不用这些地方的匹配)。
struct NormBox {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

struct AlignResult {
    bool confident = false;
    Affine2 transform;  // 参考画面 -> 当前画面, 归一化坐标
    int textured = 0;   // 参考画面里纹理够、参与匹配的块
    int matched = 0;    // 在当前画面里找到对应位置的块
    int inliers = 0;    // 符合同一个平移/旋转/缩放的块
    float rms_px = 0;   // 内点残差 (分析分辨率像素)
};

// 分块匹配 + RANSAC 拟合相似变换 (平移、旋转、等比缩放)。
// 匹配在梯度幅值上做归一化互相关, 对整体亮度/对比度变化不敏感。
// max_shift: 搜索范围, 画面宽度的比例。ref_masks: 参考画面里不参与的多边形 (忽略区)。
AlignResult align_views(const GrayImage& ref, const GrayImage& cur, float max_shift,
                        const std::vector<std::vector<Point>>& ref_masks,
                        const std::vector<NormBox>& cur_masks);

struct ViewShift {
    float dx = 0;            // 画面中心的水平位移, 画面宽度的比例
    float dy = 0;            // 垂直位移, 画面高度的比例
    float rotation_deg = 0;  // 顺时针为正 (图像坐标 y 向下)
    float scale = 1;
};
// aspect = 宽 / 高。
ViewShift describe_shift(const Affine2& t, float aspect);
// 两个变换在画面内 3x3 采样点上的最大差距, 单位是画面宽度。
float transform_distance(const Affine2& a, const Affine2& b, float aspect);
// 参考画面经 t 后还留在当前画面里的比例。
float visible_fraction(const Affine2& t);

// 摄像头被碰歪检测与区域自动跟随。
//
// 保存区域时 (或首次运行时) 存一张参考画面; 之后每 interval_sec 拿一张干净缩略图和参考
// 画面对齐。同一个偏移持续 confirm_sec 才算真的动了:
//   - 小偏移 (< dead_band) 当抖动忽略;
//   - 可靠且在 auto 范围内: 区域/线/地面标定按偏移跟随, APP 提示「已自动校正」, 可撤销;
//   - 太大或对不上: 不动区域, 提示用户确认 (持续 alert_sec 对不上才提示, 夜里画面差异、
//     人车经过都不会立刻报)。
// 配置里的区域坐标始终不改, 偏移单独存在 dir/view_guard.json。
//
// 主线程 wants_frame()/offer()/take_change(), Web 线程 json()/command(), 计算在自己的线程。
class ViewGuard {
public:
    static constexpr int kWidth = 320;
    static constexpr int kMaxRefs = 4;  // 1 张基准 + 最多 3 张不同光照下的补充参考

    struct Params {
        bool enabled = true;
        bool auto_correct = true;
        float interval_sec = 15.0f;
        float confirm_sec = 60.0f;
        float settle_sec = 10.0f;      // 摄像头刚打开, 等曝光稳定
        float alert_sec = 600.0f;      // 一直对不上多久才提示
        float dead_band = 0.01f;       // 画面宽度比例
        float max_shift = 0.2f;        // 自动跟随的最大平移
        float max_rotation_deg = 6.0f;
        float max_scale_change = 0.1f;
        float min_visible = 0.7f;      // 参考画面至少还有这么多留在画面里
        int dark_level = 12;           // 平均亮度低于它不判断 (全黑)
        std::string dir = "/userdata/monitor_box";

        static Params from_json(const Json& cfg);
    };

    struct Change {
        Affine2 applied;
        bool warp_heat = false;
        Affine2 heat_delta;  // 旧画面 -> 新画面
    };

    ViewGuard() = default;
    ~ViewGuard();
    ViewGuard(const ViewGuard&) = delete;
    ViewGuard& operator=(const ViewGuard&) = delete;

    void configure(const Params& p);
    Params params() const;
    bool load(std::string* err);
    void start();
    void stop();
    // 测试用: offer() 里直接算完。
    void set_synchronous(bool on) { synchronous_ = on; }
    void set_wall_time(time_t t) { wall_override_ = t; }

    // 配置坐标下的忽略区。
    void set_ignore(std::vector<std::vector<Point>> polys);

    // ---- 主线程 ----
    bool wants_frame(uint64_t now_ms);
    void offer(GrayImage img, std::vector<NormBox> boxes, uint64_t now_ms);
    bool take_change(Change* out);
    Affine2 applied() const;
    // 用户按当前画面重画了区域: 偏移清零, 下一帧作为新的参考画面。
    void request_reference(const char* why);

    // ---- Web 线程 ----
    Json json() const;
    // ack: 隐藏当前提示; reference/undo: 以当前画面为准 (区域按配置原样使用);
    // accept: 画面没动, 把当前画面记为补充参考 (例如夜视画面差异太大时)。
    bool command(const std::string& action, std::string* err);

private:
    enum class Cmd { None, Reference, Accept };

    struct Ref {
        GrayImage img;
        Affine2 base_to_ref;  // 配置坐标 -> 这张参考画面
        float mean = 0;
        time_t created = 0;
    };

    void loop();
    void process(const GrayImage& img, const std::vector<NormBox>& boxes, uint64_t now_ms);
    void reference_locked(const char* why);
    void set_notice_locked(const std::string& notice);
    void push_change_locked(const Affine2& applied, bool warp, const Affine2& delta);
    bool save_locked(bool refs, std::string* err);
    time_t wall_now() const { return wall_override_ ? wall_override_ : time(nullptr); }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread th_;
    bool running_ = false;
    bool synchronous_ = false;
    time_t wall_override_ = 0;

    Params p_;
    std::vector<Ref> refs_;
    std::vector<std::vector<Point>> ignore_;
    Affine2 applied_;
    std::string state_ = "starting";
    std::string notice_;  // "" / corrected / moved / unmatched
    time_t notice_time_ = 0;
    bool acked_ = true;
    Cmd cmd_ = Cmd::None;
    uint64_t gen_ = 0;

    bool has_pending_ = false;
    Affine2 pending_;
    uint64_t pending_since_ms_ = 0;
    int pending_count_ = 0;
    uint64_t low_since_ms_ = 0;

    AlignResult last_;
    Affine2 last_measured_;
    time_t last_time_ = 0;
    float last_mean_ = 0;
    float last_ms_ = 0;
    float aspect_ = 16.0f / 9.0f;

    bool has_change_ = false;
    Change change_;

    uint64_t last_seen_ms_ = 0;
    uint64_t settle_until_ms_ = 0;
    uint64_t next_due_ms_ = 0;
    uint64_t last_offer_ms_ = 0;
    bool busy_ = false;
    bool has_input_ = false;
    GrayImage in_img_;
    std::vector<NormBox> in_boxes_;
    uint64_t in_ms_ = 0;
    std::string save_error_;
};

}  // namespace mb

#endif  // MONITOR_BOX_VIDEO_VIEW_GUARD_H_
