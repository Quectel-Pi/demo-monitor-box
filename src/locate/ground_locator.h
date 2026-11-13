#ifndef MONITOR_BOX_LOCATE_GROUND_LOCATOR_H_
#define MONITOR_BOX_LOCATE_GROUND_LOCATOR_H_

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "common/geometry.h"
#include "common/json.h"
#include "locate/ground_calib.h"
#include "rules/rule_types.h"

namespace mb {

// 定位系统 (#9) 的参数: features.locate (可被挂载点覆盖)。
struct LocateParams {
    std::string feature_key = "locate";
    float report_interval_sec = 5.0f;
    float direction_window_sec = 1.5f;
    float direction_change_deg = 60.0f;
    float smooth_accel = 3.0f;      // 卡尔曼过程噪声 (m/s^2)
    float foot_sigma_px = 3.0f;     // 脚点像素噪声下限
    float foot_sigma_ratio = 0.02f; // 脚点噪声 = max(foot_sigma_px, 框高 * ratio)
    float trail_sec = 30.0f;
    float zone_margin_m = 0.3f;
    float zone_exit_sec = 1.0f;
    float zone_sustain_sec = 0.5f;
    // 分组名 -> 标签名; 解析后的 cls_id -> 分组名
    std::map<std::string, std::vector<std::string>> groups = default_groups();
    std::map<int, std::string> group_of;

    // 默认分组: 人员 / 车辆 / 宠物
    static std::map<std::string, std::vector<std::string>> default_groups();
    // 从功能参数 JSON 读; class_map 把分组里的标签解析成 cls_id (不认识的标签跳过)。
    static bool from_json(const Json& f, const std::map<std::string, int>& class_map,
                          LocateParams* out, std::string* err);
};

// 地面多边形区域 (ground_zones)。points_m 直接给米制坐标; points_img 在画面上画,
// 按当前标定换算 (画面对齐跟随时一起移动)。
struct GroundZone {
    std::string id;
    std::string name;
    bool enabled = true;
    std::string source = "meters";      // "meters" / "image"
    std::vector<Point> base_img;        // source=image: 配置坐标 (归一化, 未对齐)
    std::vector<Point> pts_m;           // 换算后的米制多边形
    bool valid = true;                  // source=image 且有点换算不了 (在地平线以上) 时为 false
    bool alarm_enter = false;
    float dwell_sec = 0.0f;
    std::vector<std::string> groups;    // 空 = 全部

    // 解析一个 ground_zones 成员。group_names 为合法分组名。
    static bool from_json(const std::string& id, const Json& j,
                          const std::map<std::string, std::vector<std::string>>& groups,
                          GroundZone* out, std::string* err);
};

// 鸟瞰图场景 (OSD 画中画 / APP 共用的数据)。
struct BevScene {
    bool valid = false;
    std::vector<Point> hull_m;
    struct Zone {
        std::vector<Point> pts;
        std::string name;
        bool occupied = false;
        bool alarm = false;
    };
    std::vector<Zone> zones;
    struct Target {
        int id = 0;
        std::string group;
        Point pos;
        float heading_deg = 0.0f;
        bool has_heading = false;
        bool in_calib = true;
        bool visible = true;
        std::vector<Point> trail;
    };
    std::vector<Target> targets;
};

// 二维视觉定位: 脚点 -> 地面坐标 -> 平滑轨迹 -> 地面区域越界 / 停留分析。
//
// 由 RuleEngine 持有 (跟着热加载 / 画面对齐 / 轨迹接续走), 每帧对符合类别的已确认轨迹调
// observe(), 帧末调 end_frame()。全部在主线程; Web 线程只拿 to_json() 的快照。
class GroundLocator {
public:
    // 地面标定区 (配置坐标) + 功能参数 + ground_zones。region 为空 = 定位未启用。
    bool configure(const Json& region, const std::string& region_id, const std::string& name,
                   const LocateParams& params, const Json& zones, std::string* err);
    // 只校验 ground_zones (定位关着时 PUT /config 也要拦住写错的区域)。
    static bool check_zones(const Json& zones, const Json& locate_feature, std::string* err);

    bool enabled() const { return enabled_; }
    const GroundCalib& calib() const { return calib_; }
    const std::string& region_id() const { return region_id_; }
    const std::vector<GroundZone>& zones() const { return zones_; }

    // 画面对齐: 标定点和 points_img 区域跟着走, 单应性重算。
    void set_view_transform(const Affine2& t);

    // 一条已确认、本帧可见的轨迹。foot 为像素脚点, box_h 为框高 (像素)。
    void observe(int track_id, int cls_id, const std::string& label, Point foot, float box_h,
                 int img_w, int img_h, uint64_t ts_ms, std::vector<RuleEvent>* out);
    // 帧末: 过期 (超过 max_lost_sec 没见到) 的目标结束区域停留、从场景里删掉。
    void end_frame(uint64_t ts_ms, float max_lost_sec, std::vector<RuleEvent>* out);
    // 轨迹接续: 新 id 继承旧 id 的滤波器、轨迹和区域停留。
    void relink(int old_id, int new_id);

    // 区域停留是否仍成立 (去抖 / 联动用)。
    // 返回 1 = 仍在区内且可见, 0 = 在区内但本帧没看到, -1 = 已离开或不存在。
    int zone_liveness(int track_id, const std::string& zone_id, uint64_t now_ms) const;

    // 热加载: 搬运目标状态和区域统计 (区域形状没变的才保留停留状态)。
    void adopt_state(const GroundLocator& old);
    void reset_stats();

    BevScene scene() const;
    // GET /ground (不含 geo)。
    Json to_json(uint64_t now_ms) const;

private:
    struct ZoneOcc {
        bool inside = false;
        bool confirmed = false;  // 已过 sustain, 已报 zone_enter
        uint64_t since = 0;
        uint64_t out_since = 0;
        bool dwell_reported = false;
    };
    struct Kalman1 {
        double x = 0, v = 0;
        double p00 = 1, p01 = 0, p11 = 1;
    };
    struct Target {
        int cls_id = -1;
        std::string label;
        std::string group;
        bool donated = false;
        Kalman1 kx, ky;
        bool has_filter = false;
        Point raw;
        Point pos;
        float sigma_m = 0.0f;
        bool in_calib = true;
        uint64_t first_ms = 0;
        uint64_t last_ms = 0;      // 最近一次可见
        uint64_t last_meas_ms = 0; // 最近一次有新测量 (脚点变了)
        Point last_foot{-1.0f, -1.0f};
        float last_box_h = -1.0f;
        float distance_m = 0.0f;
        std::deque<std::pair<uint64_t, Point>> trail;
        float heading = 0.0f;
        bool has_heading = false;
        uint64_t last_direction_ms = 0;
        uint64_t last_report_ms = 0;
        std::map<std::string, ZoneOcc> zones;
    };
    struct ZoneStats {
        int visits = 0;
        int completed = 0;  // 已结束的停留次数 (算平均停留时长用)
        double total_dwell_sec = 0.0;
        double max_dwell_sec = 0.0;
        std::map<std::string, int> by_group;
    };
    // 运行期监控: 固定长度的环形统计 (按秒分桶, 保留 5 分钟)
    struct HealthBucket {
        uint64_t sec = 0;
        int obs = 0;
        int out_of_calib = 0;
        int unmapped = 0;
        double sigma_sum = 0.0;
        int sigma_n = 0;
        double jitter_sq = 0.0;
        int jitter_n = 0;
    };

    void rebuild();
    void step_filter(Target* t, Point z, float sigma, uint64_t ts_ms);
    bool zone_applies(const GroundZone& z, const std::string& group) const;
    void update_zones(int id, Target* t, uint64_t ts_ms, std::vector<RuleEvent>* out);
    void close_zone(int id, Target* t, const GroundZone& z, ZoneOcc* occ, uint64_t end_ms,
                    uint64_t ts_ms, const char* why, std::vector<RuleEvent>* out);
    RuleEvent make_event(const char* kind, const std::string& rule_id, const std::string& name,
                         int id, const Target& t, uint64_t ts_ms, Severity sev) const;
    HealthBucket& bucket(uint64_t ts_ms);
    Json runtime_json(uint64_t now_ms) const;

    bool enabled_ = false;
    std::string region_id_;
    std::string region_name_;
    std::string mode_;
    LocateParams params_;
    std::vector<GroundPair> base_calib_;
    std::vector<GroundPair> base_check_;
    GroundCalib calib_;
    Affine2 view_;
    std::vector<GroundZone> zones_;
    std::map<int, Target> targets_;
    std::map<std::string, ZoneStats> stats_;
    std::deque<HealthBucket> health_;
    uint64_t last_frame_ms_ = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_LOCATE_GROUND_LOCATOR_H_
