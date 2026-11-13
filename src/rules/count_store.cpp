#include "rules/count_store.h"

#include <sys/stat.h>
#include <sys/time.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace mb {
namespace {

constexpr uint64_t kFlushIntervalMs = 60000;

int64_t wall_now_ms() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void mkdir_p(const std::string& path) {
    if (path.empty()) return;
    std::string cur;
    size_t i = 0;
    if (path[0] == '/') {
        cur = "/";
        i = 1;
    }
    while (i <= path.size()) {
        const size_t slash = path.find('/', i);
        const std::string part = path.substr(i, slash == std::string::npos ? std::string::npos
                                                                           : slash - i);
        if (!part.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += part;
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return;
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
}

}  // namespace

CountStore::Params CountStore::Params::from_json(const Json& cfg) {
    Params p;
    const std::string dir = cfg["events"]["snapshot_dir"].str("/userdata/monitor_box/events");
    p.log_path = cfg["events"]["counts_log"].str(dir + "/counts.jsonl");
    return p;
}

void CountStore::configure(const Params& p) {
    std::lock_guard<std::mutex> lk(mu_);
    p_ = p;
}

CountStore::Params CountStore::params() const {
    std::lock_guard<std::mutex> lk(mu_);
    return p_;
}

void CountStore::copy_locked(const CountStore& other) {
    std::lock_guard<std::mutex> lk(other.mu_);
    p_ = other.p_;
    last_ = other.last_;
    pending_by_hour_ = other.pending_by_hour_;
    last_flush_ms_ = other.last_flush_ms_;
    wall_override_ = other.wall_override_;
}

int64_t CountStore::hour_start(time_t t) {
    // 按本地时间取整点 (有的时区不是整小时偏移, 不能拿 epoch 直接模 3600)
    tm lt;
    localtime_r(&t, &lt);
    lt.tm_min = 0;
    lt.tm_sec = 0;
    return (int64_t)mktime(&lt);
}

std::string CountStore::hour_text(int64_t hour) {
    const time_t t = (time_t)hour;
    tm lt;
    localtime_r(&t, &lt);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:00:00", &lt);
    return buf;
}

std::map<int64_t, CountStore::Lines> CountStore::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_by_hour_;
}

bool CountStore::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_by_hour_.empty();
}

bool CountStore::write_locked(int64_t hour, const Lines& delta, std::string* err) {
    if (p_.log_path.empty()) return true;
    const std::string& path = p_.log_path;
    const size_t bar = path.rfind('/');
    if (bar != std::string::npos) mkdir_p(path.substr(0, bar));

    Json lines = Json::object();
    for (const auto& kv : delta) {
        if (kv.second.in == 0 && kv.second.out == 0) continue;
        Json c = Json::object();
        c.set("in", Json::number_value(kv.second.in));
        c.set("out", Json::number_value(kv.second.out));
        lines.set(kv.first, c);
    }
    if (lines.members().empty()) return true;

    Json row = Json::object();
    row.set("hour", Json::string_value(hour_text(hour)));
    row.set("ts", Json::number_value((double)(wall_now_ms() / 1000)));
    row.set("lines", lines);
    const std::string text = row.dump() + "\n";

    FILE* f = fopen(path.c_str(), "a");
    if (!f) {
        if (err) *err = "打开 " + path + " 失败: " + strerror(errno);
        return false;
    }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = fclose(f) == 0;
    if (!ok || !closed) {
        if (err) *err = "写 " + path + " 失败: " + strerror(errno);
        return false;
    }
    return true;
}

bool CountStore::maybe_flush(uint64_t now_ms, bool force, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_by_hour_.empty()) return true;
    if (!force && now_ms < last_flush_ms_ + kFlushIntervalMs) return true;
    last_flush_ms_ = now_ms;
    // 按小时从旧到新写; 写失败就保留, 下个周期重试
    for (auto it = pending_by_hour_.begin(); it != pending_by_hour_.end();) {
        if (!write_locked(it->first, it->second, err)) return false;
        it = pending_by_hour_.erase(it);
    }
    return true;
}

}  // namespace mb
