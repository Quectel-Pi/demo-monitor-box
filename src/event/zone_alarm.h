#ifndef MONITOR_BOX_EVENT_ZONE_ALARM_H_
#define MONITOR_BOX_EVENT_ZONE_ALARM_H_

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "common/json.h"
#include "event/speaker.h"
#include "rules/rule_engine.h"

namespace mb {

// 入侵 / 电子围栏 / 跌倒 / 火焰烟雾的联动配置, features.<key>.actions。
// 四类功能默认启用扬声器; 未配置 actions 时不延长录像。
struct ZoneAlarmActions {
    // 人还在区内就一直续录, 离开后再录 clip_post_sec 收尾, 单段上限 record_max_sec。
    bool record = true;
    double record_max_sec = 60.0;

    // 告警确认 (过去抖) 后立即放警报音; 人不走按 repeat_sec 重复, 最多 max_times 次。
    bool speaker = false;
    double speaker_repeat_sec = 10.0;  // 0 = 只放一次
    int speaker_max_times = 3;         // 0 = 不限
    std::string speaker_file;
    std::string speaker_device;
    std::string speaker_command;
};

struct ZoneAlarmConfig {
    // 键 = 功能, 或 功能|区域 (该区域在 regions.<id>.rules 里覆盖了参数)。
    std::map<std::string, ZoneAlarmActions> features;
    double clip_post_sec = 5.0;

    static ZoneAlarmConfig from_json(const Json& cfg);
    // 先找该区域自己的联动配置, 没有再用功能的; 都没有返回 nullptr。
    const ZoneAlarmActions* find(const std::string& feature, const std::string& region_id) const;
};

// 区域告警确认后按存活状态重复播放、续录像; 跌倒、火焰和烟雾确认后播放一次。
// 离开 / 丢失 / 撤防 (liveness=Gone) 结束区域联动。
// 只在主线程调用 (summary() 除外)。
class ZoneAlarm {
public:
    struct SpeakRequest {
        std::string command;
        std::string feature;
        std::string region_name;
        std::string event_id;
        int track_id = -1;
        int count = 0;
        AlarmTone tone = AlarmTone::Warning;
    };
    using SpeakFn = std::function<bool(const SpeakRequest&)>;
    using ExtendClipFn = std::function<bool(const std::string& event_id, uint64_t until_ts_ms)>;
    using LivenessFn = std::function<RuleEngine::Liveness(const RuleEvent&)>;

    void configure(const ZoneAlarmConfig& cfg);

    // 测试/替换用: 不设则用 player (默认自带一个) 执行命令。
    void set_speaker(SpeakFn fn) { speak_fn_ = std::move(fn); }
    void set_player(Speaker* p) { player_ = p ? p : &own_player_; }
    void set_clip_extender(ExtendClipFn fn) { extend_fn_ = std::move(fn); }

    // 跌倒、火焰和烟雾只播放一次, 忙时保留待播; 区域告警按存活状态重复播放。
    void on_fired(const RuleEvent& ev, const std::string& event_id, bool clip_recording,
                  AlarmTone tone = AlarmTone::Warning);

    // 每帧调用。liveness 一般就是 RuleEngine::liveness。
    void update(uint64_t ts_ms, const LivenessFn& liveness);

    void clear();          // 待机等场合: 丢掉所有进行中的告警
    Json summary() const;  // /status: 进行中的告警数和累计喊话次数

private:
    struct Alert {
        RuleEvent ev;
        std::string event_id;
        int warnings = 0;
        uint64_t last_warn_ts = 0;
        uint64_t speak_pending_since = 0;
        bool clip_open = false;
        AlarmTone tone = AlarmTone::Warning;
        uint64_t clip_start_ts = 0;
        uint64_t clip_until_ts = 0;
    };

    void maybe_speak(const ZoneAlarmActions& a, Alert* s, uint64_t ts_ms);
    void maybe_extend(const ZoneAlarmActions& a, Alert* s, uint64_t ts_ms);

    ZoneAlarmConfig cfg_;
    SpeakFn speak_fn_;
    ExtendClipFn extend_fn_;
    Speaker own_player_;
    Speaker* player_ = &own_player_;

    mutable std::mutex mu_;
    std::map<std::string, Alert> active_;  // 键 = feature|region|track_id
    uint64_t total_alarms_ = 0;
    uint64_t total_warnings_ = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_EVENT_ZONE_ALARM_H_
