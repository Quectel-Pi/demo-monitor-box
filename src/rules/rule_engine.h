#ifndef MONITOR_BOX_RULES_RULE_ENGINE_H_
#define MONITOR_BOX_RULES_RULE_ENGINE_H_

#include <atomic>
#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/geometry.h"
#include "common/json.h"
#include "locate/ground_locator.h"
#include "rules/rule_bindings.h"
#include "rules/rule_types.h"
#include "track/tracker.h"

namespace mb {

// 区域颜色与配置数量是一一对应的；超过这个数量后编辑器无法再用颜色区分区域。
constexpr int kMaxRegions = 12;

// 九个功能共用的空间规则引擎。
//
// 一次 update() 拿到当前帧的全部轨迹, 走三类判断:
//   1. 脚点 (框底边中点) 投到 region 做多边形测试      -> 入侵 / 电子围栏 / 区域定位
//   2. 相邻两帧脚点连成的线段与 line 求交              -> 越线人数
//   3. 脚点经地面标定投到米制地面, 平滑成二维轨迹       -> 定位 / 地面区域越界与停留
//      (locate/ground_locator.h)
//
// 三类判断只调用 common/geometry.h 里的函数, 所以电子围栏、入侵、滞留、定位的行为
// 天然一致, 现场调参只需要调一次。
//
// 跌倒不在这里: 它看 17 个关键点不是框, 见 rules/fall_detector.h;
// 但它吐出的 RuleEvent 和这里完全同构。
class RuleEngine {
public:
    // 一个 Feature 是功能在一个挂载点上的实例 (见 rules/rule_bindings.h): 同一个功能挂在
    // 三个区域上就有三个 Feature, key 相同, region_id 不同, 参数可以各自覆盖。
    // 没挂任何区域/线的功能 (包裹、跌倒, 或还没选区域的) 只有一个实例, 目标为空。
    struct Feature {
        std::string key;            // features 里的键, 也直接当事件的 feature 字段
        std::string id;             // 实例 id: key 或 key@目标, 日志和布防状态用
        bool enabled = false;
        std::string model;          // "yolo" / "pose" / "fire"; 空 = yolo (fall/fire 除外)
        std::string rule;           // "zone" / "line" / "homography" / "object"
        std::string trigger;        // "enter" / "cross" / "stay"
        std::vector<int> classes;   // 解析后的类别 id; 空 = 不限
        std::string region_id;
        std::vector<std::string> line_ids;

        float sustain_sec = 0.5f;   // enter: 连续在区内多久才报
        // zone 滞回: 脚点出界后离边界不超过 zone_margin (归一化) 且不满 exit_sec,
        // 视为边界抖动, 仍算在区内 (计时不清零, 也不会出界又进界重复告警)。
        float zone_margin = 0.02f;
        float exit_sec = 1.0f;
        // stay: "track" = 按目标计时 (同一个人在区内累计停留, 走动不清零, ID 切换接续);
        //       "region" = 旧的区域级计时 (区内人群整体不动才算, 只记录不告警)。
        std::string scope = "track";
        float dwell_sec = 60.0f;    // stay: 滞留多久才报
        float escalate_sec = 0.0f;  // stay/track: 超过它再报一次 stay_long (升级告警), 0 = 不升级
        float move_threshold_px = 20.0f;
        float line_margin = 0.03f;  // 归一化死区宽度, 扣除摄像头抖动
        float max_lost_sec = 3.0f;
        // line: 越线计数每隔多少小时自动清零一次 (按本地时间整点对齐), 0 = 不清零
        int reset_hours = 0;
        // object (包裹/遗留物): 按"位置"而不是 track id 记账, 见 ObjectSite。
        float static_sec = 20.0f;        // 静止多久算遗留
        float move_confirm_sec = 1.0f;   // 偏移要持续这么久才算真的被挪动 (滤掉框抖动/半遮挡变形)
        float unattended_sec = 5.0f;     // 周边无人持续多久才报; 0 = 不看周边有没有人
        float unattended_radius = 0.5f;  // 包裹中心到人框的距离 <= 人框高 * 该系数 算"有人看着"
        float remove_sec = 5.0f;         // 已报的包裹在无遮挡情况下消失多久报 removed; 0 = 不报

        // locate (二维视觉定位, 见 locate/ground_locator.h)
        LocateParams locate;

        // 布防 (只对 zone 的 enter/cross 生效): "always" 一直布防, "off" 撤防,
        // "schedule" 只在 arm_windows 里的时段布防。撤防时不判断、不告警, 区内状态清掉,
        // 重新布防时区内的人按新进入处理 (布防那一刻还在院子里的人也会报)。
        struct ArmWindow {
            unsigned days = 0x7F;  // bit0 = 周一 ... bit6 = 周日
            int start_min = 0;     // 当天分钟数 [0, 1440)
            int end_min = 0;       // < start_min 表示跨零点; == start_min 表示全天
        };
        std::string arm_mode = "always";
        std::vector<ArmWindow> arm_windows;

        // 是否吃 YOLO 检测结果。跌倒/火焰用各自的模型, 不算。
        bool uses_detector() const {
            if (model.empty()) return key != "fall" && key != "fire";
            return model == "yolo";
        }
    };

    struct LineCount {
        int in = 0;   // 由线的一侧到另一侧
        int out = 0;  // 反向
        int total() const { return in + out; }
        int net() const { return in - out; }
    };
    using LineCounts = std::map<std::string, LineCount>;

    RuleEngine() = default;
    // 引擎会被整体拷贝 (热加载 next_rules -> rules, 测试里也这样用), 但 line_mu_
    // 不能拷, 所以拷贝要手写: 见 rule_engine.cpp 的 copy_from()。新增成员记得加进去。
    RuleEngine(const RuleEngine& o) { copy_from(o); }
    RuleEngine& operator=(const RuleEngine& o) {
        if (this != &o) copy_from(o);
        return *this;
    }

    // class_map: 标签名 -> 类别 id, 把配置里的 "person" 解析成 0。
    bool load(const Json& cfg, const std::map<std::string, int>& class_map, std::string* err);

    const std::vector<Feature>& features() const { return features_; }
    const std::vector<NamedPolygon>& regions() const { return regions_; }
    const std::vector<NamedLine>& lines() const { return lines_; }
    // 越线计数快照 (线程安全): 主线程 update() 累加, Web 线程读取/清零, 所以返回拷贝。
    LineCounts line_counts() const;
    // 画面内人数: 最近一次 update() 里已确认、未丢失、且属于人数统计类别的轨迹数 (线程安全)。
    int people_in_frame() const { return people_in_frame_.load(std::memory_order_relaxed); }
    // 清零每条线的计数, line_id 为空 = 全部; 返回 false 表示没有这条线。
    bool reset_line_counts(const std::string& line_id);

    // 有没有启用的功能需要 YOLO 检测。全关时调用方可以不跑检测模型。
    bool needs_detector() const;
    // 该类别是否属于某个启用的检测类功能 (classes 为空 = 全部类别), 画框用。
    // "物体检测"开关只决定它自己那组类别显不显示, 其他功能关心的类别照样显示。
    bool shows_class(int cls_id) const;

    // 像素坐标下的顶点, 给 OSD 和网页画图用。
    std::vector<Point> region_pixels(const NamedPolygon& r, int w, int h) const;
    void line_pixels(const NamedLine& l, int w, int h, Point* a, Point* b) const;

    // 区域/线此刻是否有用: 被某个启用的功能引用, 或是忽略区。OSD 只画有用的, 免得
    // 没开的功能也在画面上留一堆框。
    bool region_active(const NamedPolygon& r) const;
    uint32_t region_color(const NamedPolygon& r) const;
    bool line_active(const NamedLine& l) const;
    // 像素脚点是否落在某个忽略区里。落在里面的目标不参与任何规则。
    bool ignored(Point foot_px, int w, int h) const;
    static bool valid_role(const std::string& role);

    // 每帧调用一次。tracks 含未确认轨迹, 内部只处理 confirmed。
    std::vector<RuleEvent> update(const std::vector<Track>& tracks, int img_w, int img_h,
                                  uint64_t ts_ms);

    // 长时间没出现的轨迹, 状态清掉, 免得 track id 复用时带着旧状态开局。
    void expire(uint64_t ts_ms) { expire(ts_ms, nullptr); }

    // 当前在 stay 区域里计时的目标 (按目标滞留), 给 OSD / 滞留联动 / 网页用。
    // 被遮挡但状态未过期的也在列表里 (visible=false), 免得一挡就当成离开。
    struct Dweller {
        int session = 0;
        int track_id = -1;
        int cls_id = -1;
        std::string label;
        std::string feature;
        std::string region_id;
        std::string region_name;
        uint64_t since_ms = 0;
        float seconds = 0.0f;
        float dwell_sec = 0.0f;  // 该功能的告警门限, OSD 画倒计时用
        int level = 0;           // 0 计时中, 1 已报 stay, 2 已报 stay_long
        bool visible = true;
        Point foot;              // 像素
    };
    std::vector<Dweller> dwellers() const;

    // 给事件引擎的去抖用: 挂起中的告警现在是否还成立。
    enum class Liveness {
        Unknown,   // 规则引擎不跟踪这类事件, 由调用方按轨迹存活自行判断
        Active,    // 目标仍在 (例如仍在区内)
        Occluded,  // 目标暂时没被看到 (遮挡 / 推理停顿), 既不算命中也不算丢失
        Gone,      // 已离开区域或轨迹状态已过期
    };
    Liveness liveness(const RuleEvent& ev) const;

    // 热加载时把旧引擎里仍然适用的运行状态 (区域计时、越线侧、计数等) 搬过来,
    // 免得改一次无关配置就让所有人重新计时、重复告警。
    void adopt_state(const RuleEngine& old);

    // ID 切换后, 旧 id 指向的当前 id (没有切换则原样返回)。
    int resolve_id(int id) const;

    // 该功能此刻是否布防 (按最近一次 update 的本地时间)。不存在/未启用返回 false。
    // 只给 key: 任一挂载点布防即为 true; 给 target: 只看挂在该区域上的那一处。
    bool armed(const std::string& key) const;
    bool armed(const std::string& key, const std::string& target) const;
    // 测试用: 固定"现在"的墙钟时间 (秒), 0 = 用系统时间。
    void set_wall_time(time_t t);

    // 画面对齐: 配置里的区域/线按参考画面画, t 把它们映射到当前画面 (归一化坐标)。
    // load() 后为恒等; 热加载后要重新设置。地面标定的单应性随之重算。
    void set_view_transform(const Affine2& t);
    const Affine2& view_transform() const { return view_; }
    // 配置里的区域/线几何 (未经对齐) 是否一样; 不一样说明用户按当前画面重画过。
    bool same_geometry(const RuleEngine& other) const;
    // 配置坐标下的忽略区, 画面对齐时跳过这些地方 (树会晃、窗户会变)。
    std::vector<std::vector<Point>> base_ignore_polygons() const;

    // 二维视觉定位 (地面标定 / 地面轨迹 / 地面区域 / 鸟瞰图数据)。只在主线程读;
    // Web 线程要的快照由主线程 locator().to_json() 发布。
    const GroundLocator& locator() const { return locator_; }
    void reset_ground_stats() { locator_.reset_stats(); }

private:
    struct ZoneState {
        bool inside = false;
        uint64_t since = 0;     // 进入时刻
        uint64_t out_since = 0; // 出界 (抖动带内) 开始时刻, 0 = 当前在界内
        bool reported = false;
        // stay/track
        int session = 0;
        int stay_level = 0;     // 0 计时中, 1 已报 stay, 2 已报 stay_long
    };

    struct TrackState {
        uint64_t last_seen_ms = 0;
        int cls_id = -1;
        Point last_foot;     // 像素
        float last_h = 0.0f; // 框高, 像素; ID 切换接续时做距离门限
        float box[4] = {0, 0, 0, 0};  // 最后一次的框, 遮挡导致脚点跳变时靠 IoU 接续
        bool donated = false;  // 状态已被新 id 接续过一次, 不再给第二个新 id

        // zone, 键 = feature|region: 同一区域挂多个功能时互不影响
        std::map<std::string, ZoneState> zones;

        // line: 上一次离开死区时在线的哪一侧 (+/-)
        std::map<std::string, float> line_side;
    };

    // scope=region 的 stay 是区域级计时, 不跟 track id 绑 (DESIGN 2.3.2 的低成本方案)。
    // scope=track (默认) 的计时放在 TrackState::zones 里, ID 切换靠 relink 接续。
    struct StayState {
        uint64_t since_ms = 0;
        uint64_t last_update_ms = 0;  // 上一次这个区域里还有人的时刻, 用于 expire
        Point last_centroid;
        bool active = false;
        bool reported = false;
    };

    // object: 一个"放着东西的位置"。轨迹只是来给它续命的, 换 ID / 被猫挡住 / 轨迹过期都
    // 不影响它是否已报, 所以同一个包裹只报一次 abandoned, 被拿走时报一次 removed。
    struct ObjectSite {
        int id = 0;
        int cls_id = -1;
        int track_id = -1;            // 最近一次绑定的轨迹
        float anchor[4] = {0, 0, 0, 0};  // 放下/重新锚定时的框, 判移动和遮挡都用它
        float box[4] = {0, 0, 0, 0};     // 最近一次看到的框
        uint64_t since = 0;           // 静止计时起点
        uint64_t last_seen = 0;
        uint64_t moved_since = 0;     // 偏移开始时刻, 0 = 没偏移
        uint64_t absent_since = 0;    // 无遮挡且看不到的起点, 0 = 看得到或被挡着
        uint64_t last_person = 0;     // 最近一次周边有人的时刻
        bool reported = false;
    };

    bool matches(const Feature& f, const Track& t) const;
    static bool stay_by_track(const Feature& f) { return f.trigger == "stay" && f.scope != "region"; }
    static bool zone_rule(const Feature& f) {
        return f.rule == "zone" || (f.rule.empty() && !f.region_id.empty());
    }
    static bool armable(const Feature& f) { return zone_rule(f) && f.trigger != "stay"; }
    bool armed_instance(const Feature& f) const;
    static bool armed_at(const Feature& f, time_t t);
    void refresh_armed();
    // 到点在 line_mu_ 里把 in/out 清零 (reset_hours > 0 时)。每帧 update() 调一次。
    void maybe_reset_counts();

    void expire(uint64_t ts_ms, std::vector<RuleEvent>* out);
    int session_level(const std::string& zk, int session, const TrackState* self) const;

    // zone 滞回状态机: 返回 +1 本帧刚进入, -1 本帧确认离开, 0 没变 (含抖动带内)。
    int zone_step(const Feature& f, const NamedPolygon& r, ZoneState* z, Point foot, int w, int h,
                  uint64_t ts_ms) const;
    RuleEvent make_event(const Feature& f, const char* kind, const std::string& rule_id,
                         const std::string& name, int track_id, int cls_id, uint64_t ts_ms,
                         Severity sev, Point at) const;

    void write_zone_events(const Feature& f, const NamedPolygon& r, TrackState* st,
                           const Track& t, Point foot, int w, int h, uint64_t ts_ms,
                           std::vector<RuleEvent>* out);
    void write_dwell_events(const Feature& f, const NamedPolygon& r, TrackState* st,
                            const Track& t, Point foot, int w, int h, uint64_t ts_ms,
                            std::vector<RuleEvent>* out);
    void write_line_events(const Feature& f, const NamedLine& l, TrackState* st, const Track& t,
                           Point foot, int w, int h, uint64_t ts_ms, std::vector<RuleEvent>* out);
    void write_object_events(const Feature& f, const std::vector<Track>& tracks, uint64_t ts_ms,
                             std::vector<RuleEvent>* out);
    void write_stay_events(const Feature& f, const NamedPolygon& r, const std::vector<Track>& tracks,
                           int w, int h, uint64_t ts_ms, std::vector<RuleEvent>* out);

    // 解析功能在一个挂载点上的实例并校验引用 (target 空 = 没挂)。启用的实例还会更新 max_lost_sec_。
    bool parse_feature(const std::string& key, const std::string& target, BindKind kind,
                       const Json& f, const std::map<std::string, int>& class_map, Feature* out,
                       std::string* err);

    const NamedPolygon* find_region(const std::string& id) const;
    const NamedLine* find_line(const std::string& id) const;
    // 只给 key: 第一个实例 (同一功能的 rule/trigger 相同, 用来判类型);
    // 给 target: 挂在该区域/线上的实例, 没挂返回 nullptr。
    const Feature* find_feature(const std::string& key) const;
    const Feature* find_feature(const std::string& key, const std::string& target) const;

    // 新出现的确认轨迹: 找一条刚丢失、同类、位置接近的旧轨迹, 继承它的状态。
    void relink(const Track& t, const std::vector<int>& live_ids, uint64_t ts_ms);

    // 拷贝除互斥量外的全部成员; 计数在源对象的锁里取, Web 线程同时在读也安全。
    void copy_from(const RuleEngine& o);

    static std::string zone_key(const Feature& f, const std::string& region_id) {
        return f.key + "|" + region_id;
    }

    struct Alias {
        int to = 0;
        uint64_t ts_ms = 0;
    };

    std::vector<Feature> features_;
    std::vector<NamedPolygon> regions_;
    std::vector<NamedLine> lines_;
    GroundLocator locator_;  // 二维视觉定位: 地面标定 + 地面轨迹 + 地面区域
    std::vector<std::vector<Point>> base_region_points_;  // 与 regions_ 一一对应, 未对齐
    std::vector<std::pair<Point, Point>> base_line_points_;
    Affine2 view_;
    std::map<std::string, TrackState> states_;
    std::map<std::string, StayState> stay_;
    std::map<std::string, LineCount> line_counts_;
    // 计数器自动清零的节奏: reset_sec = 0 表示不清零, reset_bucket_ 是上一次清零
    // 所在的本地时间周期序号 (见 rule_engine.cpp 的 period_bucket)。
    int reset_sec_ = 0;
    long long reset_bucket_ = -1;
    std::atomic<int> people_in_frame_{0};
    // 计数由主线程 update() 写, Web 线程 (GET/DELETE /counts) 读和清零。
    mutable std::mutex line_mu_;
    std::map<int, std::string> class_names_;  // id -> 标签名, 填事件里的 label
    std::map<int, Alias> aliases_;            // 旧 track id -> 接续它的新 id
    std::map<std::string, std::vector<ObjectSite>> sites_;  // feature key -> 包裹位置
    int next_site_ = 0;
    int person_cls_ = -1;
    float max_lost_sec_ = 3.0f;
    float relink_sec_ = 2.0f;    // 丢失多久以内的旧轨迹可以被接续
    float relink_dist_ = 0.6f;   // 脚点距离 <= 框高 * 该系数才算同一个人
    uint64_t last_update_ms_ = 0;
    int next_session_ = 0;
    std::map<std::string, bool> armed_;  // Feature::id -> 本帧是否布防
    time_t wall_override_ = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_RULES_RULE_ENGINE_H_
