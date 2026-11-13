#include "event/event_engine.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "infer/jpeg_io.h"
#include "stream/video_pipeline.h"

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
    char out[64];
    snprintf(out, sizeof(out), "%s.%03d", buf, (int)(wall_ms % 1000));
    return out;
}

bool mkdir_p(const std::string& dir) {
    if (dir.empty()) return false;
    std::string cur;
    size_t pos = 0;
    while (pos != std::string::npos) {
        pos = dir.find('/', pos + 1);
        cur = dir.substr(0, pos);
        if (cur.empty()) continue;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    struct stat st;
    return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool write_file_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
        ::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// 冷却和去抖都按目标区分: 同一个人不重复报, 不同的人各报各的
std::string cooldown_key(const RuleEvent& e) {
    return e.feature + "|" + e.kind + "|" + e.rule_id + "|" + std::to_string(e.track_id);
}

std::string pending_key(const RuleEvent& e) { return cooldown_key(e); }

long long file_bytes(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
}

}  // namespace

const char* level_name(EventLevel l) {
    switch (l) {
        case EventLevel::Critical: return "critical";
        case EventLevel::Warning: return "warning";
        default: return "info";
    }
}

bool parse_level(const std::string& s, EventLevel* out) {
    if (s == "critical") *out = EventLevel::Critical;
    else if (s == "warning") *out = EventLevel::Warning;
    else if (s == "info") *out = EventLevel::Info;
    else return false;
    return true;
}

EventEngineConfig EventEngineConfig::from_json(const Json& cfg) {
    EventEngineConfig c;
    const Json& e = cfg["events"];
    c.debounce_frames = e["debounce_frames"].integer(5);
    c.cooldown_sec = e["cooldown_sec"].number(30.0);
    c.dir = e["snapshot_dir"].str(c.dir);
    c.jpeg_quality = std::min(100, std::max(10, e["snapshot_jpeg_quality"].integer(90)));
    c.clip_pre_sec = std::max(0.0, e["clip_pre_sec"].number(5.0));
    c.clip_post_sec = std::max(0.0, e["clip_post_sec"].number(5.0));
    c.max_events = std::max(1, e["max_events"].integer(300));
    c.max_storage_mb = std::max(0.0, e["max_storage_mb"].number(c.max_storage_mb));
    c.min_free_mb = std::max(0.0, e["min_free_mb"].number(c.min_free_mb));
    for (const auto& kv : cfg["features"].members())
        c.feature_enabled[kv.first] = kv.second["enabled"].boolean(false);

    c.feature_levels["fall"] = EventLevel::Critical;
    c.feature_levels["fire"] = EventLevel::Critical;
    c.feature_levels["dwell/stay_long"] = EventLevel::Critical;
    for (const auto& kv : e["levels"].members()) {
        EventLevel l;
        if (parse_level(kv.second.str(""), &l)) c.feature_levels[kv.first] = l;
        else fprintf(stderr, "[event] events.levels.%s 取值无效, 忽略\n", kv.first.c_str());
    }

    c.routes[(int)EventLevel::Info].persist = false;
    c.routes[(int)EventLevel::Info].snapshot = false;
    c.routes[(int)EventLevel::Info].clip = false;
    for (int i = 0; i < 3; ++i) {
        const Json& r = e["routes"][level_name((EventLevel)i)];
        EventRoute& rt = c.routes[i];
        rt.persist = r["persist"].boolean(rt.persist);
        rt.snapshot = r["snapshot"].boolean(rt.snapshot);
        rt.clip = r["clip"].boolean(rt.clip);
        rt.exec = r["exec"].str(rt.exec);
        // 快照和录像都挂在事件 JSON 上, 不落盘就没法从列表里找到它们
        if (!rt.persist) rt.snapshot = rt.clip = false;
    }
    return c;
}

Json EventRecord::to_json() const {
    Json j = Json::object();
    j.set("id", Json::string_value(id));
    j.set("time", Json::string_value(format_time(wall_ms)));
    j.set("wall_ms", Json::number_value((double)wall_ms));
    j.set("ts_ms", Json::number_value((double)ev.ts_ms));
    j.set("level", Json::string_value(level_name(level)));
    j.set("feature", Json::string_value(ev.feature));
    j.set("kind", Json::string_value(ev.kind));
    j.set("rule_id", Json::string_value(ev.rule_id));
    j.set("name", Json::string_value(ev.name));
    j.set("track_id", Json::number_value(ev.track_id));
    j.set("cls_id", Json::number_value(ev.cls_id));
    j.set("label", Json::string_value(ev.label));
    j.set("x", Json::number_value(ev.x));
    j.set("y", Json::number_value(ev.y));
    if (ev.has_ground) {
        j.set("gx", Json::number_value(ev.gx));
        j.set("gy", Json::number_value(ev.gy));
    }
    if (ev.feature == "locate") j.set("heading_deg", Json::number_value(ev.heading_deg));
    j.set("extra", Json::string_value(ev.extra));
    if (ev.session > 0) {
        j.set("session", Json::number_value(ev.session));
        j.set("duration_sec", Json::number_value(ev.duration_sec));
    }
    j.set("suppressed", Json::number_value(suppressed));
    if (!snapshot.empty()) j.set("snapshot", Json::string_value(snapshot));
    if (!clip.empty()) j.set("clip", Json::string_value(clip));
    if (!clip_state.empty()) j.set("clip_state", Json::string_value(clip_state));
    return j;
}

EventEngine::~EventEngine() { stop(); }

bool EventEngine::start(const EventEngineConfig& cfg, VideoPipeline* video, std::string* err) {
    stop();
    cfg_ = cfg;
    dir_ = cfg.dir;
    video_ = video;
    if (!mkdir_p(dir_)) {
        if (err) *err = "无法创建事件目录 " + dir_ + ": " + strerror(errno);
        return false;
    }
    load_index();
    running_ = true;
    writer_ = std::thread(&EventEngine::writer_loop, this);
    return true;
}

void EventEngine::stop() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lk(wmu_);
        running_ = false;
    }
    wcv_.notify_all();
    if (writer_.joinable()) writer_.join();
}

void EventEngine::reconfigure(const EventEngineConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = cfg;
    }
    // 事件目录是写线程和 Web 共用的, 运行中换目录会让旧事件"消失", 所以要求重启
    if (cfg.dir != dir_)
        fprintf(stderr, "[event] events.snapshot_dir 变更需重启生效 (%s -> %s)\n", dir_.c_str(),
                cfg.dir.c_str());
    for (auto it = pending_.begin(); it != pending_.end();) {
        const auto feature = cfg_.feature_enabled.find(it->second.ev.feature);
        if (feature != cfg_.feature_enabled.end() && !feature->second)
            it = pending_.erase(it);
        else
            ++it;
    }
    pending_n_ = pending_.size();
}

std::string EventEngine::make_id(int64_t wall_ms) {
    const time_t t = (time_t)(wall_ms / 1000);
    tm lt;
    localtime_r(&t, &lt);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &lt);
    char id[64];
    snprintf(id, sizeof(id), "%s-%03d-%02d", buf, (int)(wall_ms % 1000), seq_++ % 100);
    return id;
}

bool EventEngine::valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '-' || c == '_'))
            return false;
    }
    return true;
}

void EventEngine::push(const std::vector<RuleEvent>& evs, bool pre_confirmed) {
    for (const RuleEvent& ev : evs) {
        const auto feature = cfg_.feature_enabled.find(ev.feature);
        if (feature != cfg_.feature_enabled.end() && !feature->second) continue;
        const bool alarm = ev.severity == Severity::Alarm;
        if (!alarm || pre_confirmed || cfg_.debounce_frames <= 0 || ev.track_id <= 0) {
            fire(ev);
            continue;
        }
        const std::string k = pending_key(ev);
        if (pending_.count(k)) continue;
        Pending p;
        p.ev = ev;
        pending_[k] = p;
    }
    pending_n_ = pending_.size();
}

void EventEngine::fire(const RuleEvent& ev) {
    const uint64_t ts_ms = ev.ts_ms;
    EventLevel level = EventLevel::Info;
    if (ev.severity == Severity::Alarm) {
        // "feature/kind" 优先于 "feature": 如 dwell/stay_long 升级成 critical
        auto it = cfg_.feature_levels.find(ev.feature + "/" + ev.kind);
        if (it == cfg_.feature_levels.end()) it = cfg_.feature_levels.find(ev.feature);
        level = it != cfg_.feature_levels.end() ? it->second : EventLevel::Warning;
    }

    const std::string ck = cooldown_key(ev);
    if (level != EventLevel::Info) {
        auto it = last_fire_.find(ck);
        if (it != last_fire_.end() && ts_ms >= it->second &&
            (double)(ts_ms - it->second) < cfg_.cooldown_sec * 1000.0) {
            ++suppressed_[ck];
            std::lock_guard<std::mutex> lk(mu_);
            ++counters_["cooldown_suppressed"];
            return;
        }
        last_fire_[ck] = ts_ms;
    }

    auto rec = std::make_shared<EventRecord>();
    rec->ev = ev;
    rec->level = level;
    rec->wall_ms = wall_now_ms();
    rec->id = make_id(rec->wall_ms);
    if (level != EventLevel::Info) {
        rec->suppressed = suppressed_[ck];
        suppressed_[ck] = 0;
    }
    const EventRoute& route = cfg_.routes[(int)level];
    rec->want_snapshot = route.persist && route.snapshot;
    rec->want_clip = route.persist && route.clip && video_ && video_->running();
    if (rec->want_snapshot) rec->snapshot = rec->id + ".jpg";
    if (rec->want_clip) {
        rec->clip = rec->id + ".mp4";
        rec->clip_state = "recording";
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        ++counters_["fired"];
        ++counters_[std::string("fired_") + level_name(level)];
        if (route.persist) {
            index_.push_back(rec);
        } else {
            info_.push_back(*rec);
            while ((int)info_.size() > cfg_.info_ring) info_.pop_front();
        }
    }
    this_frame_.push_back(rec);

    if (rec->want_clip) {
        const std::string path = dir_ + "/" + rec->clip;
        video_->request_clip(path, ev.ts_ms, cfg_.clip_pre_sec, cfg_.clip_post_sec,
                             [this, rec](bool ok, const std::string&, const std::string& err) {
                                 {
                                     std::lock_guard<std::mutex> lk(mu_);
                                     rec->clip_state = ok ? "ready" : "failed";
                                     if (!ok) {
                                         rec->clip.clear();
                                         ++counters_["clip_failed"];
                                     }
                                 }
                                 if (!ok)
                                     fprintf(stderr, "[event] %s 录像失败: %s\n", rec->id.c_str(),
                                             err.c_str());
                                 enqueue(WriteJob{rec, nullptr, false});
                             });
    }
}

std::vector<EventRecord> EventEngine::update(const std::vector<Track>& tracks, bool tracks_updated,
                                             uint64_t ts_ms, const LivenessFn& liveness) {
    now_ts_ = ts_ms;
    if (tracks_updated && !pending_.empty()) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            Pending& p = it->second;
            Liveness lv = liveness ? liveness(p.ev) : Liveness::Unknown;
            if (lv == Liveness::Unknown) {
                lv = Liveness::Gone;
                for (const Track& t : tracks) {
                    if (t.id == p.ev.track_id && t.lost == 0) {
                        lv = Liveness::Active;
                        break;
                    }
                }
            }
            if (lv == Liveness::Active) {
                ++p.hits;
                p.misses = 0;
            } else if (lv == Liveness::Gone) {
                ++p.misses;
            }
            if (p.hits >= cfg_.debounce_frames) {
                // 录像以原始事件时刻为准 (前 N 秒要覆盖"刚进入"那一下), 冷却也按它算
                const RuleEvent ev = p.ev;
                it = pending_.erase(it);
                fire(ev);
            } else if (p.misses >= cfg_.debounce_frames ||
                       ts_ms > p.ev.ts_ms + 30000) {
                std::lock_guard<std::mutex> lk(mu_);
                ++counters_["debounce_dropped"];
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        pending_n_ = pending_.size();
    }

    std::vector<EventRecord> out;
    out.reserve(this_frame_.size());
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : this_frame_) out.push_back(*r);
    return out;
}

std::vector<RuleEvent> EventEngine::highlight_sources() const {
    std::vector<RuleEvent> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& rec : this_frame_)
        if (rec->level != EventLevel::Info || rec->want_clip) out.push_back(rec->ev);
    for (const auto& rec : index_)
        if (!rec->deleted && rec->clip_state == "recording") out.push_back(rec->ev);
    return out;
}

void EventEngine::attach_frame(const ImageRGB& annotated) {
    if (this_frame_.empty()) return;
    std::shared_ptr<ImageRGB> frame;
    for (const auto& rec : this_frame_) {
        const bool persist = cfg_.routes[(int)rec->level].persist;
        const bool has_exec = !cfg_.routes[(int)rec->level].exec.empty();
        if (!persist && !has_exec) continue;
        if (rec->want_snapshot && !frame) frame = std::make_shared<ImageRGB>(annotated);
        enqueue(WriteJob{rec, rec->want_snapshot ? frame : nullptr, true});
    }
    this_frame_.clear();
}

void EventEngine::enqueue(WriteJob job) {
    {
        std::lock_guard<std::mutex> lk(wmu_);
        // 写线程卡住 (存储满/坏) 时别把内存吃光: 每帧 6MB
        if (wq_.size() > 32 && job.frame) job.frame.reset();
        wq_.push_back(std::move(job));
    }
    wcv_.notify_one();
}

void EventEngine::write_json(const EventRecord& rec) {
    const std::string path = dir_ + "/" + rec.id + ".json";
    if (!write_file_atomic(path, rec.to_json().dump(2) + "\n")) {
        std::lock_guard<std::mutex> lk(mu_);
        ++counters_["write_errors"];
        fprintf(stderr, "[event] 写 %s 失败: %s\n", path.c_str(), strerror(errno));
    }
}

void EventEngine::run_exec(const EventRecord& rec, const std::string& cmd) {
    const std::string json_path = dir_ + "/" + rec.id + ".json";
    const pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        // 二次 fork, 让孙进程被 init 收养, 主进程不用管僵尸
        if (fork() != 0) _exit(0);
        setsid();
        setenv("MB_EVENT_ID", rec.id.c_str(), 1);
        setenv("MB_EVENT_LEVEL", level_name(rec.level), 1);
        setenv("MB_EVENT_FEATURE", rec.ev.feature.c_str(), 1);
        setenv("MB_EVENT_KIND", rec.ev.kind.c_str(), 1);
        setenv("MB_EVENT_LABEL", rec.ev.label.c_str(), 1);
        setenv("MB_EVENT_JSON", json_path.c_str(), 1);
        setenv("MB_EVENT_DIR", dir_.c_str(), 1);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    waitpid(pid, nullptr, 0);
}

void EventEngine::writer_loop() {
    while (true) {
        WriteJob job;
        {
            std::unique_lock<std::mutex> lk(wmu_);
            wcv_.wait(lk, [&] { return !running_ || !wq_.empty(); });
            if (wq_.empty()) return;  // 已停止且队列清空
            job = std::move(wq_.front());
            wq_.pop_front();
        }

        std::lock_guard<std::mutex> flk(fmu_);
        EventRecord rec;
        EventRoute route;
        int quality = 90;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (job.rec->deleted) continue;
            rec = *job.rec;
            route = cfg_.routes[(int)rec.level];
            quality = cfg_.jpeg_quality;
        }

        if (job.frame && !rec.snapshot.empty()) {
            const std::string path = dir_ + "/" + rec.snapshot;
            const std::string tmp = path + ".part";
            std::string err;
            if (!jpeg_encode_file(tmp, *job.frame, quality, &err) ||
                rename(tmp.c_str(), path.c_str()) != 0) {
                ::remove(tmp.c_str());
                fprintf(stderr, "[event] 快照写入失败 %s: %s\n", path.c_str(), err.c_str());
                std::lock_guard<std::mutex> lk(mu_);
                job.rec->snapshot.clear();
                rec.snapshot.clear();
                ++counters_["write_errors"];
            }
        }

        if (route.persist) write_json(rec);

        if (job.first && !route.exec.empty()) run_exec(rec, route.exec);

        enforce_retention();
    }
}

void EventEngine::load_index() {
    std::vector<std::string> ids;
    std::vector<std::string> media;  // 快照/录像文件名, 用来找没有 JSON 的孤儿
    DIR* d = opendir(dir_.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0) {
            ::remove((dir_ + "/" + n).c_str());  // 上次异常退出留下的半截文件
            continue;
        }
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) {
            const std::string id = n.substr(0, n.size() - 5);
            if (valid_id(id)) ids.push_back(id);
        } else if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".jpg") == 0 ||
                                    n.compare(n.size() - 4, 4, ".mp4") == 0)) {
            media.push_back(n);
        }
    }
    closedir(d);
    std::sort(ids.begin(), ids.end());

    // 删事件时中途断电等留下的孤儿文件: 没人能从列表里找到它们, 只会白占空间
    for (const std::string& n : media) {
        const std::string id = n.substr(0, n.size() - 4);
        if (!valid_id(id) || std::binary_search(ids.begin(), ids.end(), id)) continue;
        if (::remove((dir_ + "/" + n).c_str()) == 0)
            fprintf(stderr, "[event] 清理孤儿文件 %s\n", n.c_str());
    }

    std::lock_guard<std::mutex> lk(mu_);
    index_.clear();
    for (const std::string& id : ids) {
        Json j;
        std::string err;
        if (!Json::parse_file(dir_ + "/" + id + ".json", &j, &err)) continue;
        auto rec = std::make_shared<EventRecord>();
        rec->id = id;
        parse_level(j["level"].str("warning"), &rec->level);
        rec->wall_ms = (int64_t)j["wall_ms"].number(0);
        rec->ev.ts_ms = (uint64_t)j["ts_ms"].number(0);
        rec->ev.feature = j["feature"].str("");
        rec->ev.kind = j["kind"].str("");
        rec->ev.rule_id = j["rule_id"].str("");
        rec->ev.name = j["name"].str("");
        rec->ev.track_id = j["track_id"].integer(-1);
        rec->ev.cls_id = j["cls_id"].integer(-1);
        rec->ev.label = j["label"].str("");
        rec->ev.x = (float)j["x"].number(0);
        rec->ev.y = (float)j["y"].number(0);
        rec->ev.has_ground = j.has("gx");
        rec->ev.gx = (float)j["gx"].number(0);
        rec->ev.gy = (float)j["gy"].number(0);
        rec->ev.heading_deg = (float)j["heading_deg"].number(0);
        rec->ev.extra = j["extra"].str("");
        rec->ev.session = j["session"].integer(0);
        rec->ev.duration_sec = (float)j["duration_sec"].number(0);
        rec->suppressed = j["suppressed"].integer(0);
        rec->snapshot = j["snapshot"].str("");
        rec->clip = j["clip"].str("");
        rec->clip_state = j["clip_state"].str("");
        // 上次没录完就退出了: 文件在就算成功, 不在就标失败
        if (rec->clip_state == "recording") {
            const bool exists = !rec->clip.empty() && file_exists(dir_ + "/" + rec->clip);
            rec->clip_state = exists ? "ready" : "failed";
            if (!exists) rec->clip.clear();
        }
        index_.push_back(rec);
    }
}

void EventEngine::remove_files(const std::string& id, std::initializer_list<const char*> exts) {
    for (const char* ext : exts) {
        const std::string p = dir_ + "/" + id + ext;
        if (::remove(p.c_str()) != 0 && errno != ENOENT) {
            fprintf(stderr, "[event] 删除 %s 失败: %s\n", p.c_str(), strerror(errno));
            std::lock_guard<std::mutex> lk(mu_);
            ++counters_["delete_errors"];
        }
    }
}

// 调用方必须持有 fmu_。
void EventEngine::enforce_retention() {
    size_t max_events;
    long long max_bytes, min_free;
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        max_events = (size_t)cfg_.max_events;
        max_bytes = (long long)(cfg_.max_storage_mb * 1024.0 * 1024.0);
        min_free = (long long)(cfg_.min_free_mb * 1024.0 * 1024.0);
        for (const auto& r : index_) ids.push_back(r->id);
    }

    long long total = 0;
    std::map<std::string, long long> sizes;
    if (max_bytes > 0) {
        for (const std::string& id : ids) {
            long long b = 0;
            for (const char* ext : {".json", ".jpg", ".mp4"}) b += file_bytes(dir_ + "/" + id + ext);
            sizes[id] = b;
            total += b;
        }
    }
    auto free_bytes = [&]() -> long long {
        struct statvfs vs;
        if (statvfs(dir_.c_str(), &vs) != 0) return -1;
        return (long long)vs.f_bavail * (long long)vs.f_frsize;
    };
    long long avail = min_free > 0 ? free_bytes() : -1;

    while (true) {
        std::string victim;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const bool over_count = index_.size() > max_events;
            const bool over_bytes = max_bytes > 0 && total > max_bytes;
            const bool low_space = min_free > 0 && avail >= 0 && avail < min_free;
            if (!over_count && !over_bytes && !low_space) break;
            // 从最旧的删起; 还在录像的跳过, 否则回调里会重新写出一个孤儿 JSON
            for (auto it = index_.begin(); it != index_.end(); ++it) {
                if ((*it)->clip_state == "recording") continue;
                victim = (*it)->id;
                (*it)->deleted = true;
                index_.erase(it);
                ++counters_[over_count ? "retention_count" : "retention_space"];
                break;
            }
        }
        if (victim.empty()) break;  // 只剩录像中的事件
        remove_files(victim, {".json", ".jpg", ".mp4"});
        total -= sizes[victim];
        if (min_free > 0) avail = free_bytes();
    }
}

bool EventEngine::extend_clip(const std::string& id, uint64_t until_ts_ms) {
    if (!video_) return false;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = index_.rbegin(); it != index_.rend(); ++it) {
            if ((*it)->id != id) continue;
            if ((*it)->clip_state != "recording" || (*it)->clip.empty()) return false;
            path = dir_ + "/" + (*it)->clip;
            break;
        }
    }
    return !path.empty() && video_->extend_clip(path, until_ts_ms);
}

EventEngine::DeleteResult EventEngine::remove(const std::string& id, bool clip_only,
                                              std::string* err) {
    if (!valid_id(id)) {
        if (err) *err = "bad event id";
        return DeleteResult::NotFound;
    }
    std::lock_guard<std::mutex> flk(fmu_);
    std::shared_ptr<EventRecord> rec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = index_.begin(); it != index_.end(); ++it) {
            if ((*it)->id != id) continue;
            rec = *it;
            if (rec->clip_state == "recording") {
                if (err) *err = "录像生成中, 稍后再删";
                return DeleteResult::Busy;
            }
            if (clip_only) {
                if (rec->clip.empty()) {
                    if (err) *err = "这条事件没有录像";
                    return DeleteResult::NotFound;
                }
                rec->clip.clear();
                rec->clip_state = "deleted";
            } else {
                rec->deleted = true;
                index_.erase(it);
            }
            ++counters_[clip_only ? "deleted_clips" : "deleted_events"];
            break;
        }
    }
    if (!rec) {
        if (err) *err = "no such event";
        return DeleteResult::NotFound;
    }

    if (clip_only) {
        // 先改 JSON 再删文件: 中途断电最多留一个孤儿 MP4, 重启时会被清理
        EventRecord copy;
        {
            std::lock_guard<std::mutex> lk(mu_);
            copy = *rec;
        }
        const std::string jp = dir_ + "/" + id + ".json";
        if (!write_file_atomic(jp, copy.to_json().dump(2) + "\n")) {
            if (err) *err = std::string("写 JSON 失败: ") + strerror(errno);
            std::lock_guard<std::mutex> lk(mu_);
            ++counters_["write_errors"];
            return DeleteResult::Error;
        }
        remove_files(id, {".mp4"});
    } else {
        // JSON 最先删: 之后断电只会留下孤儿媒体文件, 不会留下指向不存在文件的记录
        remove_files(id, {".json", ".jpg", ".mp4"});
    }
    printf("[event] 已删除 %s%s\n", id.c_str(), clip_only ? " 的录像" : "");
    return DeleteResult::Ok;
}

Json EventEngine::list(int limit, const std::string& level, const std::string& before_id) const {
    if (limit <= 0) limit = 50;
    if (limit > 1000) limit = 1000;
    Json arr = Json::array();
    std::lock_guard<std::mutex> lk(mu_);
    size_t total = 0;
    if (level == "info") {
        total = info_.size();
        for (auto it = info_.rbegin(); it != info_.rend() && (int)arr.size() < limit; ++it) {
            if (!before_id.empty() && it->id >= before_id) continue;
            arr.push(it->to_json());
        }
    } else {
        EventLevel min_level = EventLevel::Info;
        if (!level.empty()) parse_level(level, &min_level);
        total = index_.size();
        for (auto it = index_.rbegin(); it != index_.rend() && (int)arr.size() < limit; ++it) {
            const EventRecord& r = **it;
            if ((int)r.level < (int)min_level) continue;
            if (!before_id.empty() && r.id >= before_id) continue;
            arr.push(r.to_json());
        }
    }
    Json out = Json::object();
    out.set("total", Json::number_value((double)total));
    out.set("events", arr);
    return out;
}

bool EventEngine::get(const std::string& id, Json* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = index_.rbegin(); it != index_.rend(); ++it) {
        if ((*it)->id == id) {
            *out = (*it)->to_json();
            return true;
        }
    }
    return false;
}

std::string EventEngine::file_path(const std::string& id, const std::string& ext) const {
    if (!valid_id(id)) return "";
    if (ext != "jpg" && ext != "mp4" && ext != "json") return "";
    const std::string p = dir_ + "/" + id + "." + ext;
    return file_exists(p) ? p : "";
}

Json EventEngine::stats() const {
    Json j = Json::object();
    std::lock_guard<std::mutex> lk(mu_);
    j.set("dir", Json::string_value(dir_));
    j.set("stored", Json::number_value((double)index_.size()));
    j.set("info_buffered", Json::number_value((double)info_.size()));
    j.set("pending_debounce", Json::number_value((double)pending_n_));
    j.set("debounce_frames", Json::number_value(cfg_.debounce_frames));
    j.set("cooldown_sec", Json::number_value(cfg_.cooldown_sec));
    for (const auto& kv : counters_) j.set(kv.first, Json::number_value((double)kv.second));
    if (!index_.empty()) j.set("last", index_.back()->to_json());
    return j;
}

}  // namespace mb
