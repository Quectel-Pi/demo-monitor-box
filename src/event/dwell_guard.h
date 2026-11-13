#ifndef MONITOR_BOX_EVENT_DWELL_GUARD_H_
#define MONITOR_BOX_EVENT_DWELL_GUARD_H_

#include <sys/types.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common/json.h"
#include "event/speaker.h"
#include "rules/rule_engine.h"

namespace mb {

// 一个 stay 功能 (features.<key>.actions) 的联动配置。
struct DwellActions {
    // 持续录像: 告警录像在人还在区内时一直往后续, 离开后再录 clip_post_sec 收尾。
    bool record = true;
    double record_max_sec = 120.0;  // 单段录像上限 (从触发时刻算), 防止内存里攒太多帧

    // 扬声器喊话 / 警报音
    bool speaker = false;
    double speaker_start_sec = 0.0;   // 停留多久开始喊, 0 = 跟 stay 告警同时; 可小于 dwell_sec 做"预警劝离"
    double speaker_repeat_sec = 20.0; // 人不走, 每隔多久再喊一次
    int speaker_max_times = 3;        // 每个会话同一音频等级最多喊几次, 0 = 不限
    std::string speaker_file;         // WAV; 空 = 用内置的警报音
    std::string speaker_device;       // aplay -D 的设备名, 空 = default, 打不开再试 sysdefault
    std::string speaker_command;      // 非空则 /bin/sh -c 执行它 (如 spd-say), 忽略 file/device

    bool log = true;      // 会话结束写一行 JSONL
    std::string exec_end; // 会话结束时执行的命令 (可选), 信息走 MB_DWELL_* 环境变量
};

struct DwellGuardConfig {
    // 只含 trigger=stay 且按目标计时的功能。键 = 功能, 或 功能|区域 (该区域覆盖了参数)。
    std::map<std::string, DwellActions> features;
    std::string log_path;                          // 空 = 不写文件
    double clip_post_sec = 5.0;
    int history = 100;
    long long log_rotate_bytes = 4ll * 1024 * 1024;

    static DwellGuardConfig from_json(const Json& cfg);
    // 先找该区域自己的联动配置, 没有再用功能的; 都没有返回 nullptr。
    const DwellActions* find(const std::string& feature, const std::string& region_id) const;
};

// 人员滞留联动: 规则引擎给出"谁在区内待了多久", 这里负责后续动作:
//   - 扬声器: 到点喊话/放警报音, 人不走按间隔重复, 有次数上限; 同一时刻只放一路
//   - 录像:   告警事件的录像随滞留持续延长 (上限 record_max_sec)
//   - 记录:   每次滞留 (一个会话) 结束写一行 JSONL: 起止时间、时长、最高级别、
//             喊话次数、是否喊话后离开、关联事件和录像; 内存里留最近 N 条给网页
//   - exec_end: 会话结束时的联动钩子
//
// 只在主线程调用 update/on_fired/reconfigure; json()/summary() 给 Web 线程, 线程安全。
class DwellGuard {
public:
    struct SpeakRequest {
        std::string command;  // 完整 shell 命令
        int session = 0;
        int track_id = -1;
        int count = 0;        // 这是第几次喊
        float seconds = 0.0f;
        std::string region_name;
        AlarmTone tone = AlarmTone::Warning;
    };
    // 返回 false 表示播放器忙 (上一段还没放完), 这次不算数, 下一帧再试。
    using SpeakFn = std::function<bool(const SpeakRequest&)>;
    using ExtendClipFn = std::function<bool(const std::string& event_id, uint64_t until_ts_ms)>;

    DwellGuard() = default;
    ~DwellGuard();

    void configure(const DwellGuardConfig& cfg);

    // 测试/替换用: 不设则 fork /bin/sh 执行; 录像续期不设则不续。
    void set_speaker(SpeakFn fn) { speak_fn_ = std::move(fn); }
    void set_clip_extender(ExtendClipFn fn) { extend_fn_ = std::move(fn); }
    // 和围栏警报共用一个播放器, 同一时刻只放一路; 不设则用自己的。
    void set_player(Speaker* p) { player_ = p ? p : &own_player_; }

    // 每帧调用: dwellers 来自 RuleEngine::dwellers()。
    void update(const std::vector<RuleEngine::Dweller>& dwellers, uint64_t ts_ms);

    // 事件引擎本帧触发的滞留告警 (已过去抖/冷却), 用来关联事件 id 和录像。
    void on_fired(int session, const std::string& event_id, const std::string& kind,
                  bool clip_recording, uint64_t trigger_ts_ms,
                  AlarmTone tone = AlarmTone::Warning);

    // update() 先标记本帧消失的会话; 等事件引擎处理完本帧 stay_end 后再收尾,
    // 这样结束事件也能写入对应的会话记录。
    void finish_missing();

    Json json() const;     // /dwell: 进行中 + 最近结束的会话
    Json summary() const;  // /status 里的精简版
    int active_alarms() const;

private:
    struct Session {
        int session = 0;
        std::string feature;
        std::string region_id;
        std::string region_name;
        std::string label;
        std::vector<int> track_ids;
        int track_id = -1;
        uint64_t since_ts = 0;
        int64_t start_wall_ms = 0;
        float seconds = 0.0f;
        int level = 0;
        bool visible = true;
        int warnings = 0;
        int tone_warnings = 0;
        AlarmTone tone = AlarmTone::Warning;
        uint64_t last_warn_ts = 0;
        uint64_t speak_pending_since = 0;
        std::vector<std::string> event_ids;
        std::string clip_event;
        uint64_t clip_start_ts = 0;
        uint64_t clip_until_ts = 0;
        bool clip_open = false;

        Json to_json(bool active, int64_t end_wall_ms) const;
    };

    void maybe_speak(const DwellActions& a, Session* s, uint64_t ts_ms);
    void maybe_extend(const DwellActions& a, Session* s, uint64_t ts_ms);
    void finish(const Session& s);
    void append_log(const std::string& line);
    AnnounceResult default_speak(const SpeakRequest& r, uint64_t deferred_ms);

    DwellGuardConfig cfg_;
    SpeakFn speak_fn_;
    ExtendClipFn extend_fn_;
    Speaker own_player_;
    Speaker* player_ = &own_player_;

    mutable std::mutex mu_;
    std::map<int, Session> active_;
    std::set<int> pending_finish_;
    std::deque<Json> history_;
    uint64_t total_sessions_ = 0;
    uint64_t total_alarms_ = 0;
    uint64_t total_warnings_ = 0;
    uint64_t left_after_warning_ = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_EVENT_DWELL_GUARD_H_
