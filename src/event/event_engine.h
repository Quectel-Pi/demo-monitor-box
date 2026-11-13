#ifndef MONITOR_BOX_EVENT_EVENT_ENGINE_H_
#define MONITOR_BOX_EVENT_EVENT_ENGINE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/json.h"
#include "infer/types.h"
#include "rules/rule_types.h"
#include "track/tracker.h"

namespace mb {

class VideoPipeline;

// 事件分级。RuleEvent 只有 Info / Alarm 两档; Alarm 再按功能细分成 warning / critical
// (DESIGN 2.4: 火焰/跌倒高优, 其余告警普通, 滞留/人数/定位只记录)。
enum class EventLevel { Info = 0, Warning = 1, Critical = 2 };

const char* level_name(EventLevel l);
bool parse_level(const std::string& s, EventLevel* out);

// 每个级别触发后做什么。
struct EventRoute {
    bool persist = true;   // 写事件 JSON 落盘 (出现在 /events 列表)
    bool snapshot = true;  // 存带框 JPEG
    bool clip = true;      // 截前后 N 秒 MP4
    std::string exec;      // 联动插件位: 非空则 /bin/sh -c 执行, 事件信息走环境变量
};

struct EventEngineConfig {
    int debounce_frames = 5;
    double cooldown_sec = 30.0;
    std::string dir = "/userdata/monitor_box/events";
    int jpeg_quality = 90;
    double clip_pre_sec = 5.0;
    double clip_post_sec = 5.0;
    int max_events = 300;
    // 事件目录总占用上限 / 分区最少保留空间 (MB)。超了从最旧的事件删起, 0 = 不限。
    double max_storage_mb = 2048.0;
    double min_free_mb = 256.0;
    int info_ring = 200;
    std::map<std::string, bool> feature_enabled;
    std::map<std::string, EventLevel> feature_levels;  // 只对 Alarm 生效
    EventRoute routes[3];

    static EventEngineConfig from_json(const Json& cfg);
};

// 一条已触发 (通过去抖 + 冷却) 的事件。
struct EventRecord {
    std::string id;
    RuleEvent ev;
    EventLevel level = EventLevel::Info;
    int64_t wall_ms = 0;     // 墙钟 (给人看)
    int suppressed = 0;      // 冷却期内被合并掉的同类事件数 (上一次触发以来)
    bool want_snapshot = false;
    bool want_clip = false;
    std::string snapshot;    // 文件名 (相对 dir)
    std::string clip;
    std::string clip_state;  // "" / "recording" / "ready" / "failed" / "deleted"
    bool deleted = false;    // 已被用户/保留策略删除, 写线程不再为它落盘

    Json to_json() const;
};

// 事件引擎: 规则/跌倒吐出的原始事件 -> 去抖 -> 冷却 -> 按级别路由 (快照/JSON/录像/联动)。
//
// 去抖 (events.debounce_frames): 按"推理帧"计 (规则每采集帧都跑, 但轨迹只在推理帧变化,
// 按采集帧数会把去抖时长跟着帧率漂)。带 track_id 的告警先挂起, 该轨迹在之后 N 个推理帧里
// 都还在 (lost==0) 才算数; 中途连续丢失 N 帧就作废。跌倒等自带帧计数确认的来源用
// pre_confirmed=true 直接跳过去抖。
//
// 去抖期间若提供了 liveness 回调 (规则引擎), 以它为准: 目标必须仍满足规则 (如仍在区内)
// 才计命中; 遮挡/推理停顿时既不计命中也不计丢失; 已离开则计丢失。
//
// 冷却 (events.cooldown_sec): 键 = feature/kind/rule_id/track_id。同一目标同类告警 30s 内
// 只出一条, 期间被压掉的条数记在下一条的 suppressed 里; 不同目标互不压制, 第二个人闯入
// 仍有自己的告警和录像。Info 级不冷却 (越线计数不能被吞)。
//
// 落盘全部在后台写线程里做: 主循环只拷一帧画面, JPEG 编码 / 写文件 / 删旧事件不占推理时间。
class EventEngine {
public:
    enum class Liveness { Unknown, Active, Occluded, Gone };
    using LivenessFn = std::function<Liveness(const RuleEvent&)>;

    enum class DeleteResult { Ok, NotFound, Busy, Error };

    EventEngine() = default;
    ~EventEngine();

    bool start(const EventEngineConfig& cfg, VideoPipeline* video, std::string* err);
    void stop();
    void reconfigure(const EventEngineConfig& cfg);

    // 喂原始事件。pre_confirmed=true 表示来源已自带时序确认 (跌倒), 不再去抖。
    void push(const std::vector<RuleEvent>& evs, bool pre_confirmed);

    // 每帧调用一次。tracks_updated=true 表示这一帧跑过推理 (轨迹有更新), 推进去抖计数。
    // 返回本帧新触发的事件 (给 OSD / 控制台)。
    std::vector<EventRecord> update(const std::vector<Track>& tracks, bool tracks_updated,
                                    uint64_t ts_ms, const LivenessFn& liveness = nullptr);

    // 在 OSD 画完之后调用: 给本帧触发且需要快照的事件存图 (拷贝一份交给写线程)。
    void attach_frame(const ImageRGB& annotated);

    // 本帧告警和仍在录像的触发源, 给 OSD 持续高亮 (含被联动延长的录像)。
    std::vector<RuleEvent> highlight_sources() const;

    // ---- 给 Web 用, 线程安全 ----
    Json list(int limit, const std::string& level, const std::string& before_id) const;
    bool get(const std::string& id, Json* out) const;
    // id 合法且文件存在时返回绝对路径, 否则空串。ext 取 "jpg" / "mp4" / "json"。
    std::string file_path(const std::string& id, const std::string& ext) const;
    Json stats() const;

    // 删除一条已落盘的事件 (JSON + 快照 + 录像), clip_only=true 时只删录像、保留记录。
    // 录像还在生成中返回 Busy。线程安全, 与写线程串行。
    DeleteResult remove(const std::string& id, bool clip_only, std::string* err);

    // 把还在录制中的事件录像的结束时刻推迟到 until_ts_ms (采集时间戳), 用于"人不走就一直录"。
    // 录像已收尾/没有录像返回 false。只在主线程调用。
    bool extend_clip(const std::string& id, uint64_t until_ts_ms);

    const std::string& dir() const { return dir_; }

    static bool valid_id(const std::string& id);

private:
    struct Pending {
        RuleEvent ev;
        int hits = 0;
        int misses = 0;
    };
    struct WriteJob {
        std::shared_ptr<EventRecord> rec;
        std::shared_ptr<ImageRGB> frame;  // 空 = 不存快照
        bool first = true;                // false = 录像完成后的 JSON 重写, 不再触发联动
    };

    void fire(const RuleEvent& ev);
    std::string make_id(int64_t wall_ms);
    void enqueue(WriteJob job);
    void writer_loop();
    void write_json(const EventRecord& rec);
    void run_exec(const EventRecord& rec, const std::string& cmd);
    void load_index();
    void enforce_retention();
    void remove_files(const std::string& id, std::initializer_list<const char*> exts);

    EventEngineConfig cfg_;  // 主线程读写; 其它线程读必须持 mu_
    std::string dir_;        // start() 后不变, 各线程可直接读
    VideoPipeline* video_ = nullptr;
    std::atomic<size_t> pending_n_{0};

    // 主线程状态
    std::map<std::string, Pending> pending_;
    std::map<std::string, uint64_t> last_fire_;  // cooldown 键 -> ts_ms
    std::map<std::string, int> suppressed_;
    std::vector<std::shared_ptr<EventRecord>> this_frame_;
    uint64_t now_ts_ = 0;
    int seq_ = 0;

    // 共享 (Web 线程读)
    mutable std::mutex mu_;
    std::deque<std::shared_ptr<EventRecord>> index_;  // 已落盘的告警, 时间升序
    std::deque<EventRecord> info_;                    // Info 级只进内存环
    std::map<std::string, uint64_t> counters_;

    // 文件操作锁: 写线程每个任务、保留策略、用户删除都持有它, 保证"删了不会被写回来"
    std::mutex fmu_;

    // 写线程
    std::thread writer_;
    std::mutex wmu_;
    std::condition_variable wcv_;
    std::deque<WriteJob> wq_;
    std::atomic<bool> running_{false};
};

}  // namespace mb

#endif  // MONITOR_BOX_EVENT_EVENT_ENGINE_H_
