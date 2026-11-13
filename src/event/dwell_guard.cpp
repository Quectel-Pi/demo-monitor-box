#include "event/dwell_guard.h"

#include <sys/stat.h>
#include <sys/time.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>

#include "event/speaker.h"

namespace mb {
namespace {

int64_t wall_now_ms() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

std::string format_time(int64_t wall_ms) {
    const time_t t = (time_t)(wall_ms / 1000);
    tm lt;
    localtime_r(&t, &lt);
    char buf[48];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &lt);
    return buf;
}

const char* level_text(int level) {
    return level >= 2 ? "stay_long" : level == 1 ? "stay" : "counting";
}

bool parse_dwell_actions(const std::string& key, const Json& f, DwellActions* out) {
    if (f["trigger"].str("") != "stay" || f["scope"].str("track") == "region") return false;
    const Json& a = f["actions"];
    DwellActions d;
    d.speaker = key == "dwell";
    d.record = a["record"]["enabled"].boolean(d.record);
    d.record_max_sec = std::max(0.0, a["record"]["max_sec"].number(d.record_max_sec));
    const Json& sp = a["speaker"];
    d.speaker = sp["enabled"].boolean(d.speaker);
    d.speaker_start_sec = std::max(0.0, sp["start_sec"].number(d.speaker_start_sec));
    d.speaker_repeat_sec = std::max(0.0, sp["repeat_sec"].number(d.speaker_repeat_sec));
    d.speaker_max_times = std::max(0, sp["max_times"].integer(d.speaker_max_times));
    d.speaker_file = sp["file"].str("");
    d.speaker_device = sp["device"].str("");
    d.speaker_command = sp["command"].str("");
    d.log = a["log"]["enabled"].boolean(d.log);
    d.exec_end = a["exec_end"].str("");
    *out = d;
    return true;
}

}  // namespace

const DwellActions* DwellGuardConfig::find(const std::string& feature,
                                           const std::string& region_id) const {
    auto it = features.find(feature + "|" + region_id);
    if (it == features.end()) it = features.find(feature);
    return it == features.end() ? nullptr : &it->second;
}

DwellGuardConfig DwellGuardConfig::from_json(const Json& cfg) {
    DwellGuardConfig c;
    for (const auto& kv : cfg["features"].members()) {
        DwellActions d;
        if (parse_dwell_actions(kv.first, kv.second, &d)) c.features[kv.first] = d;
        for (const RuleBinding& b : feature_bindings(cfg, kv.first))
            if (parse_dwell_actions(kv.first, b.params, &d)) c.features[kv.first + "|" + b.target] = d;
    }
    const Json& e = cfg["events"];
    c.clip_post_sec = std::max(0.0, e["clip_post_sec"].number(c.clip_post_sec));
    const std::string dir = e["snapshot_dir"].str("/userdata/monitor_box/events");
    c.log_path = e["dwell_log"].str(dir + "/dwell_log.jsonl");
    return c;
}

Json DwellGuard::Session::to_json(bool active, int64_t end_wall_ms) const {
    Json j = Json::object();
    j.set("session", Json::number_value(session));
    j.set("feature", Json::string_value(feature));
    j.set("region_id", Json::string_value(region_id));
    j.set("name", Json::string_value(region_name));
    j.set("label", Json::string_value(label));
    j.set("track_id", Json::number_value(track_id));
    Json ids = Json::array();
    for (int id : track_ids) ids.push(Json::number_value(id));
    j.set("track_ids", ids);
    j.set("start_time", Json::string_value(format_time(start_wall_ms)));
    j.set("start_wall_ms", Json::number_value((double)start_wall_ms));
    if (!active) {
        j.set("end_time", Json::string_value(format_time(end_wall_ms)));
        j.set("end_wall_ms", Json::number_value((double)end_wall_ms));
    }
    j.set("duration_sec", Json::number_value(std::round(seconds * 10.0f) / 10.0));
    j.set("level", Json::string_value(level_text(level)));
    j.set("warnings", Json::number_value(warnings));
    if (active) j.set("visible", Json::bool_value(visible));
    Json evs = Json::array();
    for (const std::string& id : event_ids) evs.push(Json::string_value(id));
    j.set("events", evs);
    if (!clip_event.empty()) j.set("clip_event", Json::string_value(clip_event));
    return j;
}

DwellGuard::~DwellGuard() = default;

void DwellGuard::configure(const DwellGuardConfig& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = cfg;
}

AnnounceResult DwellGuard::default_speak(const SpeakRequest& r, uint64_t deferred_ms) {
    char buf[64];
    std::vector<std::string> env;
    env.push_back("MB_DWELL_SESSION=" + std::to_string(r.session));
    env.push_back("MB_DWELL_TRACK=" + std::to_string(r.track_id));
    env.push_back("MB_DWELL_COUNT=" + std::to_string(r.count));
    snprintf(buf, sizeof(buf), "MB_DWELL_SEC=%.0f", r.seconds);
    env.push_back(buf);
    env.push_back("MB_DWELL_REGION=" + r.region_name);
    env.push_back(std::string("MB_DWELL_LEVEL=") +
                  (r.tone == AlarmTone::Critical ? "critical" : "warning"));
    return player_->announce(r.command, env, r.tone, deferred_ms);
}

void DwellGuard::maybe_speak(const DwellActions& a, Session* s, uint64_t ts_ms) {
    if (!a.speaker || !s->visible || s->tone == AlarmTone::Silent) return;
    const bool due = a.speaker_start_sec > 0.0 ? s->seconds >= a.speaker_start_sec : s->level >= 1;
    if (!due) return;
    if (a.speaker_max_times > 0 && s->tone_warnings >= a.speaker_max_times) return;
    if (s->tone_warnings > 0) {
        if (a.speaker_repeat_sec <= 0.0) return;
        if (ts_ms < s->last_warn_ts + (uint64_t)(a.speaker_repeat_sec * 1000.0)) return;
    }
    SpeakRequest r;
    r.command = speak_fn_ ? a.speaker_command
                          : player_->command_for(a.speaker_file, a.speaker_device,
                                                 a.speaker_command, s->tone);
    r.tone = s->tone;
    r.session = s->session;
    r.track_id = s->track_id;
    r.count = s->warnings + 1;
    r.seconds = s->seconds;
    r.region_name = s->region_name;
    if (s->speak_pending_since == 0) s->speak_pending_since = ts_ms;
    const uint64_t deferred_ms = ts_ms >= s->speak_pending_since
                                     ? ts_ms - s->speak_pending_since
                                     : 0;
    const AnnounceResult result = speak_fn_
        ? (speak_fn_(r) ? AnnounceResult::Started : AnnounceResult::Busy)
        : default_speak(r, deferred_ms);
    if (result == AnnounceResult::Busy || result == AnnounceResult::Failed) return;
    s->speak_pending_since = 0;
    ++s->warnings;
    ++s->tone_warnings;
    s->last_warn_ts = ts_ms;
    if (result == AnnounceResult::Started) {
        ++total_warnings_;
        printf("[dwell] 扬声器警告 #%d: 会话 %d id%d 在 %s 已停留 %.0fs\n", s->warnings,
               s->session, s->track_id, s->region_name.c_str(), s->seconds);
        fflush(stdout);
    }
}

void DwellGuard::maybe_extend(const DwellActions& a, Session* s, uint64_t ts_ms) {
    if (!a.record || !s->clip_open || !extend_fn_) return;
    const uint64_t post_ms = (uint64_t)(cfg_.clip_post_sec * 1000.0);
    const uint64_t cap = s->clip_start_ts + (uint64_t)(a.record_max_sec * 1000.0);
    const uint64_t until = std::min(ts_ms + post_ms, cap);
    // 每 0.5s 续一次就够了, 不用每帧都去拿编码线程的锁
    if (until < s->clip_until_ts + 500 && until < cap) return;
    if (until <= s->clip_until_ts) {
        if (until >= cap) s->clip_open = false;
        return;
    }
    if (!extend_fn_(s->clip_event, until)) {
        s->clip_open = false;  // 录像已经收尾 (或被删), 不再续
        return;
    }
    s->clip_until_ts = until;
    if (until >= cap) s->clip_open = false;
}

void DwellGuard::update(const std::vector<RuleEngine::Dweller>& dwellers, uint64_t ts_ms) {
    player_->poll();
    std::lock_guard<std::mutex> lk(mu_);
    static const DwellActions kDefault;
    const int64_t now_wall = wall_now_ms();
    std::set<int> seen;
    for (const RuleEngine::Dweller& d : dwellers) {
        if (d.session <= 0) continue;
        seen.insert(d.session);
        Session& s = active_[d.session];
        if (s.session == 0) {
            s.session = d.session;
            s.feature = d.feature;
            s.region_id = d.region_id;
            s.region_name = d.region_name;
            s.label = d.label;
            s.since_ts = d.since_ms;
            s.start_wall_ms = now_wall - (int64_t)(d.seconds * 1000.0f);
        }
        s.track_id = d.track_id;
        if (std::find(s.track_ids.begin(), s.track_ids.end(), d.track_id) == s.track_ids.end())
            s.track_ids.push_back(d.track_id);
        s.seconds = d.seconds;
        s.visible = d.visible;
        pending_finish_.erase(d.session);
        const DwellActions* found = cfg_.find(d.feature, d.region_id);
        const DwellActions& a = found ? *found : kDefault;
        maybe_speak(a, &s, ts_ms);
        maybe_extend(a, &s, ts_ms);
    }
    for (const auto& kv : active_)
        if (!seen.count(kv.first)) pending_finish_.insert(kv.first);
}

void DwellGuard::finish_missing() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto pending = pending_finish_.begin(); pending != pending_finish_.end();) {
        auto it = active_.find(*pending);
        if (it == active_.end()) {
            pending = pending_finish_.erase(pending);
            continue;
        }
        finish(it->second);
        active_.erase(it);
        pending = pending_finish_.erase(pending);
    }
}

void DwellGuard::on_fired(int session, const std::string& event_id, const std::string& kind,
                          bool clip_recording, uint64_t trigger_ts_ms, AlarmTone tone) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = active_.find(session);
    if (it == active_.end()) return;
    Session& s = it->second;
    s.event_ids.push_back(event_id);
    if (kind == "stay" || kind == "stay_long") {
        if (s.tone != tone) {
            s.tone_warnings = 0;
            s.speak_pending_since = 0;
        }
        s.tone = tone;
        const int level = kind == "stay_long" ? 2 : 1;
        if (level > s.level) {
            if (s.level == 0) ++total_alarms_;
            s.level = level;
        }
    }
    // 只续第一段 (stay) 的录像; stay_long 自己那段按普通告警录前后 N 秒
    if (clip_recording && kind == "stay" && s.clip_event.empty()) {
        s.clip_event = event_id;
        s.clip_open = true;
        s.clip_start_ts = trigger_ts_ms;
        s.clip_until_ts = trigger_ts_ms + (uint64_t)(cfg_.clip_post_sec * 1000.0);
    }
}

// 调用方持有 mu_。
void DwellGuard::finish(const Session& s) {
    // 只路过没到告警、也没被喊过的, 不算一次滞留
    if (s.level <= 0 && s.warnings <= 0) return;
    const int64_t end_wall = wall_now_ms();
    Json j = s.to_json(false, end_wall);
    const char* reason = s.visible ? "left" : "lost";
    j.set("end_reason", Json::string_value(reason));
    const bool dispersed = s.visible && s.warnings > 0;
    j.set("left_after_warning", Json::bool_value(dispersed));
    ++total_sessions_;
    if (dispersed) ++left_after_warning_;
    history_.push_front(j);
    while ((int)history_.size() > std::max(1, cfg_.history)) history_.pop_back();

    const DwellActions* actions = cfg_.find(s.feature, s.region_id);
    const bool log = !actions || actions->log;
    if (log && !cfg_.log_path.empty()) append_log(j.dump() + "\n");

    printf("[dwell] 会话 %d 结束: id%d 在 %s 停留 %.0fs, 最高 %s, 喊话 %d 次, %s\n", s.session,
           s.track_id, s.region_name.c_str(), s.seconds, level_text(s.level), s.warnings,
           s.visible ? (s.warnings ? "已劝离" : "已离开") : "目标丢失");
    fflush(stdout);

    if (actions && !actions->exec_end.empty()) {
        char buf[64];
        std::vector<std::string> env;
        env.push_back("MB_DWELL_SESSION=" + std::to_string(s.session));
        env.push_back("MB_DWELL_TRACK=" + std::to_string(s.track_id));
        snprintf(buf, sizeof(buf), "MB_DWELL_SEC=%.0f", s.seconds);
        env.push_back(buf);
        env.push_back("MB_DWELL_REGION=" + s.region_name);
        env.push_back(std::string("MB_DWELL_LEVEL=") + level_text(s.level));
        env.push_back("MB_DWELL_WARNINGS=" + std::to_string(s.warnings));
        env.push_back(std::string("MB_DWELL_END=") + reason);
        env.push_back("MB_DWELL_CLIP_EVENT=" + s.clip_event);
        env.push_back("MB_DWELL_JSON=" + j.dump());
        spawn_shell(actions->exec_end, env, true);
    }
}

void DwellGuard::append_log(const std::string& line) {
    struct stat st;
    if (cfg_.log_rotate_bytes > 0 && stat(cfg_.log_path.c_str(), &st) == 0 &&
        (long long)st.st_size + (long long)line.size() > cfg_.log_rotate_bytes) {
        rename(cfg_.log_path.c_str(), (cfg_.log_path + ".1").c_str());
    }
    FILE* f = fopen(cfg_.log_path.c_str(), "a");
    if (!f) {
        fprintf(stderr, "[dwell] 写滞留记录 %s 失败: %s\n", cfg_.log_path.c_str(), strerror(errno));
        return;
    }
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
}

Json DwellGuard::json() const {
    std::lock_guard<std::mutex> lk(mu_);
    Json active = Json::array();
    for (const auto& kv : active_) active.push(kv.second.to_json(true, 0));
    Json hist = Json::array();
    for (const Json& h : history_) hist.push(h);
    Json j = Json::object();
    j.set("active", active);
    j.set("history", hist);
    Json st = Json::object();
    st.set("sessions", Json::number_value((double)total_sessions_));
    st.set("alarms", Json::number_value((double)total_alarms_));
    st.set("warnings", Json::number_value((double)total_warnings_));
    st.set("left_after_warning", Json::number_value((double)left_after_warning_));
    j.set("stats", st);
    if (!cfg_.log_path.empty()) j.set("log", Json::string_value(cfg_.log_path));
    return j;
}

Json DwellGuard::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    int alarms = 0, counting = 0;
    float longest = 0.0f;
    for (const auto& kv : active_) {
        if (kv.second.level > 0) ++alarms;
        else ++counting;
        longest = std::max(longest, kv.second.seconds);
    }
    Json j = Json::object();
    j.set("staying", Json::number_value(alarms));
    j.set("counting", Json::number_value(counting));
    j.set("longest_sec", Json::number_value(std::round(longest)));
    j.set("sessions", Json::number_value((double)total_sessions_));
    j.set("warnings", Json::number_value((double)total_warnings_));
    j.set("left_after_warning", Json::number_value((double)left_after_warning_));
    return j;
}

int DwellGuard::active_alarms() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (const auto& kv : active_) n += kv.second.level > 0;
    return n;
}

}  // namespace mb
