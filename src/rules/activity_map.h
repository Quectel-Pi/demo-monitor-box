#ifndef MONITOR_BOX_RULES_ACTIVITY_MAP_H_
#define MONITOR_BOX_RULES_ACTIVITY_MAP_H_

#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/geometry.h"
#include "common/json.h"
#include "track/tracker.h"

namespace mb {

class RuleEngine;

// 活动热图: 统计人和车的脚点 "经过" 了画面里哪些格子, 给 APP 推荐区域 (车多的是马路,
// 人常走的是通道/门口)。
//
// 按 "轨迹进入一个新格子" 计数, 不按帧计数: 停着的车/站着的人只算一次, 热图反映的是
// 通行量而不是停留时长, 也和帧率、推理频率无关。
//
// 除了通行量, 还记三样东西给 APP 推荐门口和进出线 (app/mb_regions.py::suggest):
//   flow    每格 8 个方向的穿行次数 (轨迹从相邻格子走进来时, 按走向计一次)
//   births / deaths  轨迹第一次出现 / 最后一次出现的格子: 不在画面边缘却经常凭空出现、
//           消失的地方, 多半是门口
//   night   夜间时段 (night_start..night_end 点) 的通行量, 推荐夜间布防的区域
//
// 主线程 update(), Web 线程 json()/reset(), 内部加锁。
class ActivityMap {
public:
    struct Params {
        bool enabled = true;
        bool learning = false;  // 学习中: 即使没有功能需要, 也让检测模型跑起来
        int cols = 64;
        int rows = 36;
        std::string path;            // 持久化文件, 空 = 不落盘
        float save_interval_sec = 300.0f;
        int night_start = 20;        // 本地时间, 小时 [0, 24)
        int night_end = 6;           // < night_start 表示跨零点

        static Params from_json(const Json& cfg);
    };

    void configure(const Params& p, const std::map<std::string, int>& class_map);
    const Params& params() const { return p_; }

    // tracks 里只看已确认且本帧可见的; rules 非空时跳过忽略区里的目标。
    void update(const std::vector<Track>& tracks, int img_w, int img_h, uint64_t ts_ms,
                const RuleEngine* rules);

    Json json() const;
    void reset();
    // 摄像头被碰歪后区域跟着平移: 已有统计也按同一变换搬到新画面坐标 (归一化 delta:
    // 旧画面 -> 新画面)。搬出画面的格子丢弃。
    void warp(const Affine2& delta);

    bool load(std::string* err);
    // 到点且有新数据才写; force = 退出时无条件写一次。
    bool maybe_save(uint64_t now_ms, bool force, std::string* err);

    // 测试用: 固定 "现在" 的墙钟时间 (秒), 0 = 用系统时间。
    void set_wall_time(time_t t) { wall_override_ = t; }

    static constexpr int kDirs = 8;  // 0 = 画面右, 顺时针每 45 度一格 (图像 y 向下)

private:
    enum Group { kNone = 0, kPerson = 1, kVehicle = 2 };

    // 一类目标 (人 / 车) 的全部统计, 各数组按 cell = y * cols + x 排。
    struct Layer {
        std::vector<uint32_t> pass;    // 通行量
        std::vector<uint32_t> night;   // 夜间通行量
        std::vector<uint32_t> births;  // 轨迹出现
        std::vector<uint32_t> deaths;  // 轨迹消失
        std::vector<uint32_t> flow;    // cell * kDirs + dir
        uint64_t tracks = 0;
    };

    Group group_of(int cls_id) const;
    Layer& layer(Group g) { return g == kVehicle ? vehicle_ : person_; }
    void clear_locked();
    time_t wall_now() const { return wall_override_ ? wall_override_ : time(nullptr); }
    bool is_night(time_t t) const;
    Json json_locked(bool with_meta) const;
    bool save_locked(std::string* err);

    struct Seen {
        int cell = -1;
        uint64_t last_ms = 0;
        Group group = kNone;
        // 走进忽略区 (树后面、邻居家) 的目标不计数但继续记着, 免得在忽略区边上记成
        // "消失" 又 "出现", 被当成门口; 在忽略区里丢掉的也不记消失。
        bool hidden = false;
    };

    Params p_;
    std::map<int, Group> groups_;  // 类别 id -> 分组
    mutable std::mutex mu_;
    Layer person_;
    Layer vehicle_;
    time_t since_ = 0;
    time_t updated_ = 0;
    std::map<int, Seen> seen_;  // track id -> 上次所在格子
    bool dirty_ = false;
    uint64_t last_save_ms_ = 0;
    time_t wall_override_ = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_RULES_ACTIVITY_MAP_H_
