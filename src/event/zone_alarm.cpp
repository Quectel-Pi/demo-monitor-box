#include "event/zone_alarm.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace mb {

namespace {

// base_actions: features.<key> 自己有没有 actions。续录默认值看它而不是合并后的参数, 否则
// 某个区域只覆盖了喇叭开关 (actions.speaker) 就会顺带打开续录。
bool parse_actions(const std::string& key, const Json& f, bool zone, bool base_actions,
                   ZoneAlarmActions* out) {
    if (!f["enabled"].boolean(true)) return false;
    const bool one_shot = key == "fall" || key == "fire";
    if (!one_shot && (!zone || f["trigger"].str("") == "stay")) return false;
    const Json& a = f["actions"];
    const bool default_speaker = one_shot || key == "intrusion" || key == "fence";
    if (!a.is_object() && !default_speaker) return false;
    ZoneAlarmActions d;
    d.speaker = default_speaker;
    d.record = !one_shot && base_actions;
    d.record = a["record"]["enabled"].boolean(d.record);
    d.record_max_sec = std::max(0.0, a["record"]["max_sec"].number(d.record_max_sec));
    const Json& sp = a["speaker"];
    d.speaker = sp["enabled"].boolean(d.speaker);
    d.speaker_repeat_sec = std::max(0.0, sp["repeat_sec"].number(d.speaker_repeat_sec));
    d.speaker_max_times = std::max(0, sp["max_times"].integer(d.speaker_max_times));
    d.speaker_file = sp["file"].str("");
    d.speaker_device = sp["device"].str("");
    d.speaker_command = sp["command"].str("");
    *out = d;
    return true;
}

}  // namespace

const ZoneAlarmActions* ZoneAlarmConfig::find(const std::string& feature,
                                              const std::string& region_id) const {
    auto it = features.find(feature + "|" + region_id);
    if (it == features.end()) it = features.find(feature);
    return it == features.end() ? nullptr : &it->second;
}

ZoneAlarmConfig ZoneAlarmConfig::from_json(const Json& cfg) {
    ZoneAlarmConfig c;
    for (const auto& kv : cfg["features"].members()) {
        const std::string rule = kv.second["rule"].str("");
        const bool zone = rule == "zone" ||
                          (rule.empty() && binding_kind(cfg, kv.first) == BindKind::Region);
        const bool base_actions = kv.second["actions"].is_object();
        ZoneAlarmActions d;
        if (parse_actions(kv.first, kv.second, zone, base_actions, &d)) c.features[kv.first] = d;
        for (const RuleBinding& b : feature_bindings(cfg, kv.first))
            if (parse_actions(kv.first, b.params, zone, base_actions, &d))
                c.features[kv.first + "|" + b.target] = d;
    }
    c.clip_post_sec = std::max(0.0, cfg["events"]["clip_post_sec"].number(c.clip_post_sec));
    return c;
}

void ZoneAlarm::configure(const ZoneAlarmConfig& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = cfg;
}

void ZoneAlarm::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    active_.clear();
}

void ZoneAlarm::on_fired(const RuleEvent& ev, const std::string& event_id, bool clip_recording,
                         AlarmTone tone) {
    const bool one_shot = (ev.feature == "fall" && ev.kind == "fall") ||
                         (ev.feature == "fire" && (ev.kind == "fire" || ev.kind == "smoke"));
    if ((!one_shot && ev.kind != "enter") || ev.severity != Severity::Alarm ||
        tone == AlarmTone::Silent) return;
    std::lock_guard<std::mutex> lk(mu_);
    const ZoneAlarmActions* actions = cfg_.find(ev.feature, ev.rule_id);
    if (!actions || (!actions->speaker && (one_shot || !actions->record))) return;
    const std::string key = one_shot ? event_id
        : ev.feature + "|" + ev.rule_id + "|" + std::to_string(ev.track_id);
    if (active_.count(key)) return;  // 同一个人还没离开, 不重新开一轮
    Alert& s = active_[key];
    s.ev = ev;
    s.tone = tone;
    s.event_id = event_id;
    if (clip_recording && !one_shot) {
        s.clip_open = true;
        s.clip_start_ts = ev.ts_ms;
        s.clip_until_ts = ev.ts_ms + (uint64_t)(cfg_.clip_post_sec * 1000.0);
    }
    ++total_alarms_;
}

void ZoneAlarm::maybe_speak(const ZoneAlarmActions& a, Alert* s, uint64_t ts_ms) {
    if (!a.speaker) return;
    if (a.speaker_max_times > 0 && s->warnings >= a.speaker_max_times) return;
    if (s->warnings > 0) {
        if (a.speaker_repeat_sec <= 0.0) return;
        if (ts_ms < s->last_warn_ts + (uint64_t)(a.speaker_repeat_sec * 1000.0)) return;
    }
    SpeakRequest r;
    r.command = speak_fn_ ? a.speaker_command
                          : player_->command_for(a.speaker_file, a.speaker_device,
                                                 a.speaker_command, s->tone);
    r.tone = s->tone;
    r.feature = s->ev.feature;
    r.region_name = s->ev.name;
    r.event_id = s->event_id;
    r.track_id = s->ev.track_id;
    r.count = s->warnings + 1;
    if (s->speak_pending_since == 0) s->speak_pending_since = ts_ms;
    const uint64_t deferred_ms = ts_ms >= s->speak_pending_since
                                     ? ts_ms - s->speak_pending_since
                                     : 0;
    AnnounceResult result = AnnounceResult::Busy;
    if (speak_fn_) {
        result = speak_fn_(r) ? AnnounceResult::Started : AnnounceResult::Busy;
    } else {
        std::vector<std::string> env;
        env.push_back("MB_ALARM_FEATURE=" + r.feature);
        env.push_back("MB_ALARM_REGION=" + r.region_name);
        env.push_back("MB_ALARM_EVENT=" + r.event_id);
        env.push_back("MB_ALARM_TRACK=" + std::to_string(r.track_id));
        env.push_back("MB_ALARM_COUNT=" + std::to_string(r.count));
        env.push_back(std::string("MB_ALARM_LEVEL=") +
                      (r.tone == AlarmTone::Critical ? "critical" : "warning"));
        result = player_->announce(r.command, env, r.tone, deferred_ms);
    }
    if (result == AnnounceResult::Busy || result == AnnounceResult::Failed) return;
    s->speak_pending_since = 0;
    ++s->warnings;
    s->last_warn_ts = ts_ms;
    if (result == AnnounceResult::Started) {
        ++total_warnings_;
        printf("[alarm] 扬声器警报 #%d: %s id%d %s\n", s->warnings, r.feature.c_str(),
               r.track_id, r.region_name.c_str());
        fflush(stdout);
    }
}

void ZoneAlarm::maybe_extend(const ZoneAlarmActions& a, Alert* s, uint64_t ts_ms) {
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
    if (!extend_fn_(s->event_id, until)) {
        s->clip_open = false;  // 录像已经收尾 (或被删), 不再续
        return;
    }
    s->clip_until_ts = until;
    if (until >= cap) s->clip_open = false;
}

void ZoneAlarm::update(uint64_t ts_ms, const LivenessFn& liveness) {
    player_->poll();
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = active_.begin(); it != active_.end();) {
        const ZoneAlarmActions* f = cfg_.find(it->second.ev.feature, it->second.ev.rule_id);
        if (it->second.ev.feature == "fall" || it->second.ev.feature == "fire") {
            if (f && f->speaker) maybe_speak(*f, &it->second, ts_ms);
            if (!f || !f->speaker || it->second.warnings > 0)
                it = active_.erase(it);
            else
                ++it;
            continue;
        }
        const RuleEngine::Liveness l =
            liveness ? liveness(it->second.ev) : RuleEngine::Liveness::Gone;
        if (!f || l == RuleEngine::Liveness::Gone ||
            l == RuleEngine::Liveness::Unknown) {
            it = active_.erase(it);
            continue;
        }
        // 被挡住时不喊 (可能已经翻出去了), 录像照续, 等确认离开/丢失再收尾
        if (l == RuleEngine::Liveness::Active) maybe_speak(*f, &it->second, ts_ms);
        maybe_extend(*f, &it->second, ts_ms);
        ++it;
    }
}

Json ZoneAlarm::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    Json j = Json::object();
    j.set("active", Json::number_value((double)active_.size()));
    j.set("total_alarms", Json::number_value((double)total_alarms_));
    j.set("total_warnings", Json::number_value((double)total_warnings_));
    return j;
}

}  // namespace mb
