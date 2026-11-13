#include "event/speaker.h"

#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

extern char** environ;

namespace mb {
namespace {

int64_t monotonic_now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void put_le(std::string* s, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) s->push_back((char)((v >> (8 * i)) & 0xFF));
}

}  // namespace

SpeakerConfig SpeakerConfig::from_json(const Json& cfg) {
    SpeakerConfig c;
    const Json& a = cfg.path("events.audio");
    c.warning_coalesce_sec = std::max(0.0, a["warning_coalesce_sec"].number(c.warning_coalesce_sec));
    c.critical_coalesce_sec =
        std::max(0.0, a["critical_coalesce_sec"].number(c.critical_coalesce_sec));
    c.max_delay_sec = std::max(0.0, a["max_delay_sec"].number(c.max_delay_sec));
    c.preempt_lower_priority = a["preempt_lower_priority"].boolean(c.preempt_lower_priority);
    return c;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// 环境变量在 fork 前拼好: 多线程进程里 fork 后只该调 async-signal-safe 的函数。
pid_t spawn_shell(const std::string& cmd, const std::vector<std::string>& extra_env, bool detach) {
    std::vector<std::string> env_store;
    for (char** e = environ; e && *e; ++e) {
        bool overridden = false;
        for (const std::string& x : extra_env) {
            const size_t eq = x.find('=');
            if (strncmp(*e, x.c_str(), eq + 1) == 0) {
                overridden = true;
                break;
            }
        }
        if (!overridden) env_store.emplace_back(*e);
    }
    env_store.insert(env_store.end(), extra_env.begin(), extra_env.end());
    std::vector<char*> envp;
    for (std::string& s : env_store) envp.push_back(&s[0]);
    envp.push_back(nullptr);
    char sh[] = "/bin/sh";
    char dash_c[] = "-c";
    std::string cmd_copy = cmd;
    char* argv[] = {sh, dash_c, &cmd_copy[0], nullptr};

    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // detach: 二次 fork 让孙进程被 init 收养, 父进程立即回收子进程
        if (detach && fork() != 0) _exit(0);
        setsid();
        execve("/bin/sh", argv, envp.data());
        _exit(127);
    }
    if (detach) {
        waitpid(pid, nullptr, 0);
        return 0;
    }
    return pid;
}

bool write_alarm_wav(const std::string& path, double seconds, AlarmTone tone) {
    constexpr int kRate = 16000;
    const int n = std::max(1, (int)(seconds * kRate));
    const bool critical = tone == AlarmTone::Critical;
    const int seg = critical ? kRate / 10 : kRate / 4;
    const int fade = kRate / 200;
    std::string pcm;
    pcm.reserve((size_t)n * 2);
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const int pos = i % seg;
        const double freq = (i / seg) % 2 == 0 ? (critical ? 1320.0 : 880.0)
                                                : (critical ? 990.0 : 660.0);
        phase += 2.0 * M_PI * freq / kRate;
        double amp = 0.6;
        if (pos < fade) amp *= (double)pos / fade;  // 首尾淡入淡出, 免得切换处爆音
        if (seg - pos < fade) amp *= (double)(seg - pos) / fade;
        put_le(&pcm, (uint32_t)(int16_t)(std::sin(phase) * amp * 32767.0), 2);
    }
    std::string wav = "RIFF";
    put_le(&wav, (uint32_t)(36 + pcm.size()), 4);
    wav += "WAVEfmt ";
    put_le(&wav, 16, 4);
    put_le(&wav, 1, 2);  // PCM
    put_le(&wav, 1, 2);  // mono
    put_le(&wav, kRate, 4);
    put_le(&wav, kRate * 2, 4);
    put_le(&wav, 2, 2);
    put_le(&wav, 16, 2);
    wav += "data";
    put_le(&wav, (uint32_t)pcm.size(), 4);
    wav += pcm;

    const std::string tmp = path + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(wav.data(), 1, wav.size(), f) == wav.size();
    if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
        ::remove(tmp.c_str());
        return false;
    }
    return true;
}

Speaker::~Speaker() { stop(); }

void Speaker::poll() {
    if (player_ <= 0) return;
    int status = 0;
    pid_t r = waitpid(player_, &status, WNOHANG);
    if (r == 0) {
        // 播放命令卡死 (音频设备被占等) 不能让后面的警告永远发不出去
        if (monotonic_now_ms() - started_ms_ < 60000) return;
        kill(-player_, SIGKILL);
        kill(player_, SIGKILL);
        r = waitpid(player_, &status, 0);
        fprintf(stderr, "[speaker] 扬声器命令 60s 未结束, 已强制终止\n");
    }
    if (r == player_ && WIFEXITED(status) && WEXITSTATUS(status) != 0 && !error_logged_) {
        error_logged_ = true;
        fprintf(stderr, "[speaker] 扬声器命令退出码 %d (没接扬声器/设备名不对?), 后续不再提示\n",
                WEXITSTATUS(status));
    }
    player_ = -1;
    current_tone_ = AlarmTone::Silent;
}

void Speaker::stop_current() {
    if (player_ <= 0) return;
    kill(-player_, SIGKILL);
    kill(player_, SIGKILL);
    while (waitpid(player_, nullptr, 0) < 0 && errno == EINTR) {
    }
    player_ = -1;
    current_tone_ = AlarmTone::Silent;
}

void Speaker::stop() {
    poll();
    stop_current();
}

AnnounceResult Speaker::announce(const std::string& command, const std::vector<std::string>& env,
                                 AlarmTone tone, uint64_t deferred_ms) {
    poll();
    if (tone == AlarmTone::Silent || command.empty()) return AnnounceResult::Failed;

    // 历史事件积压时, 过期音频不再补播。事件本身已经落盘, 不受影响。
    if (cfg_.max_delay_sec > 0.0 &&
        deferred_ms >= (uint64_t)(cfg_.max_delay_sec * 1000.0)) {
        return AnnounceResult::Coalesced;
    }

    const int64_t now = monotonic_now_ms();
    const double coalesce_sec = tone == AlarmTone::Critical ? cfg_.critical_coalesce_sec
                                                            : cfg_.warning_coalesce_sec;
    const bool has_previous = tone == AlarmTone::Critical ? has_critical_audio_ : has_warning_audio_;
    const int64_t previous = tone == AlarmTone::Critical ? last_critical_ms_ : last_warning_ms_;
    if (coalesce_sec > 0.0 && has_previous && now >= previous &&
        now - previous < (int64_t)(coalesce_sec * 1000.0)) {
        return AnnounceResult::Coalesced;
    }

    if (player_ > 0) {
        if (tone != AlarmTone::Critical || current_tone_ == AlarmTone::Critical ||
            !cfg_.preempt_lower_priority) {
            return AnnounceResult::Busy;
        }
        fprintf(stderr, "[speaker] 紧急告警打断普通告警\n");
        stop_current();
    }

    const pid_t pid = spawn_shell(command, env, false);
    if (pid < 0) {
        fprintf(stderr, "[speaker] 启动扬声器命令失败: %s\n", strerror(errno));
        return AnnounceResult::Failed;
    }
    player_ = pid;
    started_ms_ = now;
    current_tone_ = tone;
    if (tone == AlarmTone::Critical) {
        has_critical_audio_ = true;
        last_critical_ms_ = now;
    } else {
        has_warning_audio_ = true;
        last_warning_ms_ = now;
    }
    return AnnounceResult::Started;
}

bool Speaker::play(const std::string& command, const std::vector<std::string>& env) {
    return announce(command, env, AlarmTone::Warning) == AnnounceResult::Started;
}

std::string Speaker::command_for(const std::string& file_in, const std::string& device,
                                 const std::string& command, AlarmTone tone) {
    if (tone == AlarmTone::Silent) return "";
    if (!command.empty()) return command;
    std::string file = file_in;
    if (file.empty()) {
        std::string& tone_path = tone == AlarmTone::Critical ? critical_path_ : warning_path_;
        if (tone_path.empty()) {
            const std::string p = tone == AlarmTone::Critical ? "/tmp/mb_alarm_critical.wav"
                                                             : "/tmp/mb_alarm_warning.wav";
            if (write_alarm_wav(p, 1.5, tone)) tone_path = p;
            else fprintf(stderr, "[speaker] 生成内置警报音失败: %s\n", strerror(errno));
        }
        file = tone_path;
        if (file.empty()) return "";
    }
    const std::string q = shell_quote(file);
    if (!device.empty()) return "aplay -q -D " + shell_quote(device) + " " + q;
    // 服务以 root 运行时, default 常被桌面用户的 PipeWire 接管而打不开 (Host is down),
    // 退回 sysdefault 直接走默认声卡
    return "aplay -q " + q + " 2>/dev/null || aplay -q -D sysdefault " + q;
}

}  // namespace mb
