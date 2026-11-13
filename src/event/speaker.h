#ifndef MONITOR_BOX_EVENT_SPEAKER_H_
#define MONITOR_BOX_EVENT_SPEAKER_H_

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/json.h"

namespace mb {

enum class AlarmTone { Silent, Warning, Critical };

// 大量事件同时出现时, 音频只负责“让人注意到”, 不逐条补播历史。事件记录、快照和录像不受影响。
struct SpeakerConfig {
    double warning_coalesce_sec = 10.0;   // 普通告警的合并窗口
    double critical_coalesce_sec = 3.0;   // 紧急告警的合并窗口
    double max_delay_sec = 5.0;           // 排队超过该时长仍未出声就丢弃, 0 = 不限制
    bool preempt_lower_priority = true;   // 紧急告警打断正在播放的普通告警

    static SpeakerConfig from_json(const Json& cfg);
};

enum class AnnounceResult {
    Started,    // 已启动播放
    Coalesced,  // 本时间段已有同类音频, 或排队太久; 调用方应视为已处理
    Busy,       // 更高优先级正在播放; 调用方下一帧再试
    Failed,     // 命令为空或启动失败; 调用方下一帧再试
};

std::string shell_quote(const std::string& s);

// fork + /bin/sh -c, extra_env 追加/覆盖环境变量。detach=true 时不等结束、不留僵尸 (返回 0)。
pid_t spawn_shell(const std::string& cmd, const std::vector<std::string>& extra_env, bool detach);

// 生成一段双音警报 WAV (16kHz/16bit/单声道), 板上没准备音频文件时扬声器也能响。
bool write_alarm_wav(const std::string& path, double seconds,
                     AlarmTone tone = AlarmTone::Warning);

// 扬声器播放器: 入侵、跌倒、火焰烟雾、围栏、滞留共用一个实例, 同一时刻只放一路。
// 只在主线程调用。
class Speaker {
public:
    Speaker() = default;
    ~Speaker();
    Speaker(const Speaker&) = delete;
    Speaker& operator=(const Speaker&) = delete;

    void configure(const SpeakerConfig& cfg) { cfg_ = cfg; }

    // 播放器空闲时启动 command; 忙 / 命令为空 / 启动失败返回 false (调用方下一帧再试)。
    bool play(const std::string& command, const std::vector<std::string>& env);

    // 事件音频入口。deferred_ms = 该提示已经等待的时长, 超时后直接合并丢弃。
    // 同类音频在合并窗口内只响一次; critical 可打断 warning。
    AnnounceResult announce(const std::string& command, const std::vector<std::string>& env,
                            AlarmTone tone, uint64_t deferred_ms = 0);

    // 回收已结束的播放进程; 卡死超过 60s 的强制结束。
    void poll();

    // 立即结束当前播放, 用于待机、紧急告警抢占或对象销毁。
    void stop();

    // 由配置拼出播放命令: command 非空直接用; 否则 aplay file (空 = 内置警报音)。
    std::string command_for(const std::string& file, const std::string& device,
                            const std::string& command, AlarmTone tone = AlarmTone::Warning);

private:
    void stop_current();

    SpeakerConfig cfg_;
    pid_t player_ = -1;
    int64_t started_ms_ = 0;
    int64_t last_warning_ms_ = 0;
    int64_t last_critical_ms_ = 0;
    bool has_warning_audio_ = false;
    bool has_critical_audio_ = false;
    AlarmTone current_tone_ = AlarmTone::Silent;
    std::string warning_path_;
    std::string critical_path_;
    bool error_logged_ = false;
};

}  // namespace mb

#endif  // MONITOR_BOX_EVENT_SPEAKER_H_
