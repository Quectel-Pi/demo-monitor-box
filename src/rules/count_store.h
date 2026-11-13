#ifndef MONITOR_BOX_RULES_COUNT_STORE_H_
#define MONITOR_BOX_RULES_COUNT_STORE_H_

#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string>

#include "common/json.h"

namespace mb {

// 越线计数的按小时累计落盘 (默认 events/counts.jsonl)。
//
// 计数的真相在 RuleEngine (/status、OSD 读的就是它); 这里只把增量追加进历史。
// 主线程每帧 poll(规则里的计数) 一次 (纯内存, 和上次看到的快照比差值), 每 60 秒
// maybe_flush() 把攒下的增量按小时写成 JSONL 行。断电最多丢一分钟的计数。
//
// 每行 {"hour":"2026-11-04T14:00:00","ts":...,"lines":{"gate-1":{"in":3,"out":1}}}。
// 同一小时可能有多行 (每 60 秒一行), 消费端按 hour 求和即可。计数被清零时增量
// 变成负数, 只重设基线不写负值, 所以清零不会污染历史。
class CountStore {
public:
    struct Line {
        int in = 0;
        int out = 0;
    };
    using Lines = std::map<std::string, Line>;

    struct Params {
        std::string log_path;  // 空 = 不落盘
        static Params from_json(const Json& cfg);
    };

    CountStore() = default;
    CountStore(const CountStore& other) { copy_locked(other); }
    CountStore& operator=(const CountStore& other) {
        if (this != &other) copy_locked(other);
        return *this;
    }

    void configure(const Params& p);
    Params params() const;

    // counts: map<string, T>, T 有 in/out 整数字段 (RuleEngine::LineCounts 即可)。
    // 只用主线程调用, 每秒一次足够。
    template <class Counts>
    void poll(const Counts& counts) {
        std::lock_guard<std::mutex> lk(mu_);
        const int64_t hour = hour_start(wall_now());
        for (const auto& kv : counts) {
            Line cur;
            cur.in = (int)kv.second.in;
            cur.out = (int)kv.second.out;
            auto it = last_.find(kv.first);
            if (it == last_.end()) {
                last_[kv.first] = cur;
                continue;
            }
            const Line prev = it->second;
            it->second = cur;
            // 计数变小只可能是被 /counts 清零 (或线被删掉重建), 只重设基线
            if (cur.in < prev.in || cur.out < prev.out) continue;
            if (cur.in == prev.in && cur.out == prev.out) continue;
            Line& d = pending_by_hour_[hour][kv.first];
            d.in += cur.in - prev.in;
            d.out += cur.out - prev.out;
        }
    }

    // 到点 (默认 60 秒, force 时立即) 把攒下的增量写成 JSONL 行。
    bool maybe_flush(uint64_t now_ms, bool force, std::string* err);

    // 还没落盘的增量 (测试用)。
    std::map<int64_t, Lines> pending() const;
    bool empty() const;

    // 测试用: 固定 "现在" 的墙钟时间 (秒), 0 = 用系统时间。
    void set_wall_time(time_t t) { wall_override_ = t; }

private:
    time_t wall_now() const { return wall_override_ ? wall_override_ : time(nullptr); }
    static int64_t hour_start(time_t t);
    static std::string hour_text(int64_t hour);
    void copy_locked(const CountStore& other);
    bool write_locked(int64_t hour, const Lines& delta, std::string* err);

    Params p_;
    mutable std::mutex mu_;
    Lines last_;                                          // 上次 poll 看到的累计值
    std::map<int64_t, Lines> pending_by_hour_;            // 小时起点 -> 待写增量
    uint64_t last_flush_ms_ = 0;
    time_t wall_override_ = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_RULES_COUNT_STORE_H_
