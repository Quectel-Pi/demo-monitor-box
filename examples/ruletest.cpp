// mb-ruletest —— 规则引擎 + 跟踪器的确定性回归测试, 不依赖摄像头/模型/NPU。
//
// 用法: ./build/mb-ruletest       (板上或宿主机都能跑, 全部通过返回 0)
//
// 覆盖入侵检测的几类误报/漏报场景:
//   1. 脚点在区域边界来回抖动: 不清零计时、不重复告警
//   2. 真正离开 (超出抖动带 / 抖动带内停留超时) 后再进入, 重新计时再报
//   3. 遮挡 (轨迹暂时消失): 去抖判定为 Occluded, 超时后 Gone
//   4. ID 切换: 新 id 接续旧 id 的区域状态, 不重复告警, 旧事件 liveness 跟到新 id;
//      旧 id 又被跟回来不重复报; 半身遮挡 (脚点跳变) 靠框 IoU 接续
//   5. 同一区域挂两个功能互不影响; 两个人各自告警
//   6. 热加载 adopt_state 后不重复告警; 改了区域形状则重新计时
//   7. 人员滞留 (按目标): 区内走动也计时; stay -> stay_long 升级; 离开/丢失报 stay_end;
//      ID 切换/旧 id 回来不重复报; 多人各自会话; 热加载不重复报; scope=region 兼容旧行为
//   8. 滞留联动: 扬声器按间隔重复且有次数上限、忙时不叠放; 录像随滞留续期并封顶;
//      预警劝离; 会话结束写 JSONL, 路过不记
//   9. 跟踪器: 挨着的两个人各有轨迹; 移动目标漏检几帧后保持同一 id
//  10. 火焰烟雾时序确认: 单帧闪现不报; 持续/偶尔漏检才报且只报一次; 稀疏命中、
//      低分、小框不报; 中断超过 gap_sec 重新计时; clear_sec 后解除并可再报;
//      烟雾确认更久; 类别按名字映射
//  12. 包裹 (按位置去重): 长时间静止只报一次; 框抖动/猫半遮挡/完全盖住/换 ID/跳类别
//      不重报; 有人在旁边不报; 真被挪动才重新计时; 无遮挡消失报 removed; 热加载不重报
//  13. 布防时段: 时段外不报; 布防时人在区内按新进入报; 跨零点; 按星期; 撤防时挂起告警作废;
//      mode=off; 配置错误在加载时报出
//  14. 围栏联动: 确认即喊、按间隔重复有上限、忙时不叠放; 人在区内续录像并封顶;
//      离开/撤防/丢失结束, 遮挡时不喊; 音频开关独立于录像
//  16. 区域用途: role 校验; 只画启用功能用到的区域/线; 忽略区里的目标不参与规则
//  17. 活动热图: 按轨迹进入新格子计数 (停着的车只算一次); 忽略区/非人车类别不统计; 落盘读回
//  20. 摄像头偏移: 合成画面平移/旋转/变暗/遮挡仍能对齐; 持续偏移才跟随; 超范围/对不上只提示;
//      撤销、补充参考、落盘读回; 规则区域和热图跟着搬
//  21. 越线计数: 方向、死区、ID 切换补计、热加载保留
//  22. 越线计数落盘: 按小时攒增量、清零不写负值、跨小时分行
//  23. 画面内人数 (NOW): 未确认/丢失/非统计类别不计, 人站线哪侧都算
//  24. 越线计数自动清零: 按本地整点对齐, 周期 N 小时, 0 = 不清
//  26. 地面标定: N 点最小二乘 / 矩形兼容 / 检查点精度 / 离群点 / 退化 / 地平线 / 远处不确定度
//  27. 二维定位: 人车宠分组; 地面区域越界 + 停留告警 + 统计; 卡尔曼平滑; ID 切换不重报;
//      热加载保留; 画面对齐跟随; 运行期标定质量监控; 配置校验
//  28. 设备经纬度: nmcli / iw / Google / IP 解析; provider 顺序回落; 配置校验

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include "common/json.h"
#include "event/dwell_guard.h"
#include "event/zone_alarm.h"
#include "geo/geo_locator.h"
#include "locate/ground_calib.h"
#include "rules/activity_map.h"
#include "rules/count_store.h"
#include "rules/fire_detector.h"
#include "rules/rule_engine.h"
#include "track/tracker.h"
#include "video/view_guard.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

constexpr int W = 1000, H = 1000;

const char* kConfig = R"JSON({
  "track": {"relink_sec": 2.0, "relink_dist": 0.6},
  "features": {
    "intrusion": {"enabled": true, "rule": "zone", "trigger": "enter", "classes": ["person"],
                  "region_id": "zone-1", "sustain_sec": 0.5, "zone_margin": 0.02, "exit_sec": 1.0,
                  "max_lost_sec": 3},
    "fence": {"enabled": true, "rule": "zone", "trigger": "cross", "classes": ["person"],
              "region_id": "zone-1", "sustain_sec": 0.5}
  },
  "regions": {
    "zone-1": {"name": "A", "points": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]]}
  }
})JSON";

mb::Json config(const char* text = kConfig) {
    mb::Json j;
    std::string err;
    if (!mb::Json::parse(text, &j, &err)) printf("配置解析失败: %s\n", err.c_str());
    return j;
}

std::map<std::string, int> classes() { return {{"person", 0}}; }

// 脚点在 (fx, fy) 像素处、高 200 的人框
mb::Track person(int id, float fx, float fy) {
    mb::Track t;
    t.id = id;
    t.cls_id = 0;
    t.confirmed = true;
    t.x1 = fx - 40;
    t.x2 = fx + 40;
    t.y1 = fy - 200;
    t.y2 = fy;
    return t;
}

int count(const std::vector<mb::RuleEvent>& evs, const char* feature, const char* kind) {
    int n = 0;
    for (const auto& e : evs) n += e.feature == feature && e.kind == kind;
    return n;
}

struct Run {
    mb::RuleEngine rules;
    std::vector<mb::RuleEvent> all;
    uint64_t ts = 100000;

    explicit Run(const mb::Json& cfg = config(),
                 const std::map<std::string, int>& cls = classes()) {
        std::string err;
        if (!rules.load(cfg, cls, &err)) printf("规则加载失败: %s\n", err.c_str());
    }
    // 推进 n 帧 (每帧 40ms)
    std::vector<mb::RuleEvent> step(const std::vector<mb::Track>& tracks, int n = 1) {
        std::vector<mb::RuleEvent> out;
        for (int i = 0; i < n; ++i) {
            ts += 40;
            auto evs = rules.update(tracks, W, H, ts);
            out.insert(out.end(), evs.begin(), evs.end());
        }
        all.insert(all.end(), out.begin(), out.end());
        return out;
    }
};

void test_boundary_jitter() {
    printf("1. 边界抖动\n");
    Run r;
    // 边界 y=800。脚点在 795 / 810 间每帧交替 (出界 10px = 0.01 < margin 0.02)
    for (int i = 0; i < 40; ++i) r.step({person(1, 500, i % 2 ? 810.0f : 795.0f)});
    check(count(r.all, "intrusion", "enter") == 1, "抖动 1.6s 内只报一次入侵");
    check(count(r.all, "fence", "exit") == 0, "抖动不算离开");

    Run r2;
    // 刚进入就开始抖动: 计时不能被抖动清零, 否则永远到不了 sustain (漏报)
    r2.step({person(1, 500, 795)});
    for (int i = 0; i < 20; ++i) r2.step({person(1, 500, i % 3 ? 795.0f : 812.0f)});
    check(count(r2.all, "intrusion", "enter") == 1, "进入后在边界抖动, 仍能在 sustain 后报出");
}

void test_real_exit() {
    printf("2. 真正离开再进入\n");
    Run r;
    r.step({person(1, 500, 500)}, 20);
    check(count(r.all, "intrusion", "enter") == 1, "进入并持续 0.8s 报一次");
    r.step({person(1, 500, 900)}, 2);  // 出界 0.1 > margin, 立即离开
    check(count(r.all, "fence", "exit") == 1, "远离边界立即判离开");
    r.step({person(1, 500, 500)}, 20);
    check(count(r.all, "intrusion", "enter") == 2, "再次进入重新计时后再报");

    Run r3;
    r3.step({person(1, 500, 500)}, 20);
    // 停在边界外 10px (抖动带内) 超过 exit_sec
    r3.step({person(1, 500, 810)}, 30);
    check(count(r3.all, "fence", "exit") == 1, "抖动带内停留超过 exit_sec 判离开");
}

void test_occlusion() {
    printf("3. 遮挡 / 推理停顿\n");
    Run r;
    auto evs = r.step({person(1, 500, 500)}, 20);
    mb::RuleEvent ev;
    for (const auto& e : r.all)
        if (e.feature == "intrusion") ev = e;
    check(r.rules.liveness(ev) == mb::RuleEngine::Liveness::Active, "在区内: Active");
    r.step({}, 10);  // 0.4s 没有任何轨迹
    check(r.rules.liveness(ev) == mb::RuleEngine::Liveness::Occluded, "短暂消失: Occluded");
    r.step({person(1, 500, 500)}, 5);
    check(count(r.all, "intrusion", "enter") == 1, "遮挡后重现不重复告警");
    r.step({}, 100);  // 4s > max_lost_sec
    check(r.rules.liveness(ev) == mb::RuleEngine::Liveness::Gone, "消失超过 max_lost_sec: Gone");

    Run r2;
    r2.step({person(1, 500, 500)}, 20);
    r2.step({person(1, 500, 900)}, 2);
    mb::RuleEvent ev2;
    for (const auto& e : r2.all)
        if (e.feature == "intrusion") ev2 = e;
    check(r2.rules.liveness(ev2) == mb::RuleEngine::Liveness::Gone, "离开区域: Gone (去抖会作废)");
}

void test_id_switch() {
    printf("4. ID 切换\n");
    Run r;
    r.step({person(1, 500, 500)}, 20);
    mb::RuleEvent ev;
    for (const auto& e : r.all)
        if (e.feature == "intrusion") ev = e;
    r.step({}, 5);                      // 跟丢 200ms
    r.step({person(7, 520, 505)}, 20);  // 换成 #7 出现在附近
    check(count(r.all, "intrusion", "enter") == 1, "换 ID 后不重复告警");
    check(r.rules.resolve_id(1) == 7, "旧 id 1 解析到新 id 7");
    check(r.rules.liveness(ev) == mb::RuleEngine::Liveness::Active, "旧事件的 liveness 跟到新轨迹");

    Run r2;
    r2.step({person(1, 500, 500)}, 5);  // 还没到 sustain 就换了 ID
    r2.step({}, 2);
    r2.step({person(9, 510, 500)}, 8);
    int enters = 0;
    uint64_t t = 0;
    for (const auto& e : r2.all)
        if (e.feature == "intrusion" && e.kind == "enter") ++enters, t = e.ts_ms;
    check(enters == 1 && t <= 100000 + 40 * 15, "换 ID 不清零计时, 按原进入时刻满 sustain 就报");

    Run r4;
    r4.step({person(1, 500, 500)}, 20);
    r4.step({}, 3);
    r4.step({person(7, 510, 500)}, 10);  // #1 -> #7
    r4.step({person(1, 500, 500), person(7, 510, 500)}, 10);  // 跟踪器分裂: #1 又回来
    check(count(r4.all, "intrusion", "enter") == 1, "被接续的旧 id 重新出现, 不当新人再报");
    check(r4.rules.resolve_id(1) == 1, "旧 id 回来后不再指向接续者");

    Run r5;
    r5.step({person(1, 500, 600)}, 20);  // 全身框, 脚点 y=600, 高 200
    r5.step({}, 3);
    // 下半身被桌子挡住: 新框顶不变、底边上移 150px (脚点跳了 0.75 个框高)
    mb::Track half = person(8, 500, 450);
    half.y1 = 400;
    r5.step({half}, 20);
    check(count(r5.all, "intrusion", "enter") == 1, "半身遮挡导致脚点跳变, 靠框 IoU 接续不重复报");

    Run r3;
    r3.step({person(1, 300, 300)}, 20);
    r3.step({}, 2);
    r3.step({person(5, 700, 700)}, 20);  // 远处出现的是另一个人
    check(count(r3.all, "intrusion", "enter") == 2, "远处的新目标不接续, 单独告警");
}

void test_isolation_and_multi() {
    printf("5. 功能隔离 / 多人\n");
    Run r;
    r.step({person(1, 500, 500), person(2, 300, 600)}, 20);
    check(count(r.all, "intrusion", "enter") == 2, "两个人各自报入侵");
    check(count(r.all, "fence", "enter") == 2, "同一区域上的围栏功能不受入侵功能影响");
}

void test_reload() {
    printf("6. 热加载\n");
    Run r;
    r.step({person(1, 500, 500)}, 20);
    mb::RuleEngine next;
    std::string err;
    next.load(config(), classes(), &err);
    next.adopt_state(r.rules);
    r.rules = next;
    r.step({person(1, 500, 500)}, 20);
    check(count(r.all, "intrusion", "enter") == 1, "区域没变: 热加载后不重复告警");

    std::string moved = kConfig;
    moved.replace(moved.find("[0.2, 0.2], [0.8, 0.2]"), 22, "[0.1, 0.1], [0.8, 0.1]");
    mb::RuleEngine next2;
    next2.load(config(moved.c_str()), classes(), &err);
    next2.adopt_state(r.rules);
    r.rules = next2;
    r.step({person(1, 500, 500)}, 20);
    check(count(r.all, "intrusion", "enter") == 2, "区域改了形状: 重新计时并告警");

    const mb::RuleEvent old_intrusion = r.all.front();
    mb::Json disabled = config();
    mb::Json features = disabled["features"];
    mb::Json intrusion = features["intrusion"];
    intrusion.set("enabled", mb::Json::bool_value(false));
    features.set("intrusion", intrusion);
    disabled.set("features", features);
    mb::RuleEngine off;
    check(off.load(disabled, classes(), &err), "加载已关闭入侵的配置");
    off.adopt_state(r.rules);
    r.rules = off;
    r.all.clear();
    r.step({person(1, 500, 500), person(2, 400, 500)}, 20);
    check(count(r.all, "intrusion", "enter") == 0, "关闭入侵后新老目标都不产生入侵事件");
    check(r.rules.liveness(old_intrusion) == mb::RuleEngine::Liveness::Gone,
          "关闭入侵后旧告警不再有效");
    check(count(r.all, "fence", "enter") > 0, "关闭入侵不影响共用区域的围栏告警");
}

const char* kDwellConfig = R"JSON({
  "track": {"relink_sec": 2.0, "relink_dist": 0.6},
  "events": {"clip_post_sec": 1, "snapshot_dir": "/tmp"},
  "features": {
    "dwell": {"enabled": true, "rule": "zone", "trigger": "stay", "classes": ["person"],
              "region_id": "zone-1", "dwell_sec": 2, "escalate_sec": 4, "max_lost_sec": 3,
              "actions": {"record": {"enabled": true, "max_sec": 5},
                          "speaker": {"enabled": true, "repeat_sec": 1, "max_times": 3,
                                      "command": "true"},
                          "log": {"enabled": true}}}
  },
  "regions": {
    "zone-1": {"name": "A", "points": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]]}
  }
})JSON";

// 返回拷贝: r.all 之后还会增长, 指针会失效
std::optional<mb::RuleEvent> find_ev(const std::vector<mb::RuleEvent>& evs, const char* kind) {
    for (const auto& e : evs)
        if (e.kind == kind) return e;
    return std::nullopt;
}

// 在区内来回走动的人: 每帧横移 30px (> 旧版 move_threshold_px), 300..700 之间往返
float pace(int i) {
    const int k = (i * 30) % 800;
    return 300.0f + (float)(k < 400 ? k : 800 - k);
}

void test_dwell() {
    printf("7. 人员滞留 (按目标计时)\n");
    {
        Run r(config(kDwellConfig));
        for (int i = 0; i < 45; ++i) r.step({person(1, pace(i), 500)});  // 1.8s
        check(count(r.all, "dwell", "stay") == 0, "未满 dwell_sec 不报");
        for (int i = 45; i < 60; ++i) r.step({person(1, pace(i), 500)});  // 2.4s
        check(count(r.all, "dwell", "stay") == 1, "区内来回走动也计时, 满 2s 报 stay");
        const auto st = find_ev(r.all, "stay");
        check(st && st->severity == mb::Severity::Alarm && st->session > 0 && st->track_id == 1,
              "stay 是告警级, 带会话号和 track id");
        check(st && r.rules.liveness(*st) == mb::RuleEngine::Liveness::Active,
              "去抖期间 liveness 按区内状态判 Active");
        auto dw = r.rules.dwellers();
        check(dw.size() == 1 && dw[0].level == 1 && dw[0].seconds >= 2.0f, "dwellers 给出计时和级别");
        for (int i = 60; i < 110; ++i) r.step({person(1, pace(i), 500)});  // 4.4s
        check(count(r.all, "dwell", "stay") == 1, "持续滞留不重复报 stay");
        check(count(r.all, "dwell", "stay_long") == 1, "满 escalate_sec 升级报 stay_long");
        r.step({person(1, 500, 950)}, 2);  // 远离区域
        const auto end = find_ev(r.all, "stay_end");
        check(end && end->severity == mb::Severity::Info && st && end->session == st->session,
              "离开后报 stay_end (Info), 同一会话号");
        check(end && end->duration_sec > 4.0f && end->duration_sec < 5.0f, "stay_end 带总时长");
        check(r.rules.dwellers().empty(), "离开后不在 dwellers 里");
        check(st && r.rules.liveness(*st) == mb::RuleEngine::Liveness::Gone, "离开后 liveness=Gone");
    }
    {
        Run r(config(kDwellConfig));
        r.step({person(1, 500, 500)}, 25);  // 1s 路过
        r.step({person(1, 500, 950)}, 5);
        check(count(r.all, "dwell", "stay") == 0 && count(r.all, "dwell", "stay_end") == 0,
              "路过不满时长: 不报 stay 也不报 stay_end");
    }
    {
        Run r(config(kDwellConfig));
        r.step({person(1, 500, 500)}, 30);  // 1.2s
        r.step({}, 5);
        r.step({person(7, 515, 505)}, 30);  // 换成 #7, 再 1.2s
        const auto st = find_ev(r.all, "stay");
        check(count(r.all, "dwell", "stay") == 1 && st && st->track_id == 7,
              "ID 切换不清零: 两段合计满 2s 报一次, 报在新 id 上");
        auto dw = r.rules.dwellers();
        check(dw.size() == 1 && dw[0].track_id == 7, "dwellers 只保留接续者");
        r.step({person(1, 500, 500), person(7, 515, 505)}, 5);  // #1 被跟回来
        check(count(r.all, "dwell", "stay") == 1, "旧 id 回来不重复报");
        check(r.rules.dwellers().size() == 1, "同一会话不重复出现在 dwellers");
    }
    {
        Run r(config(kDwellConfig));
        r.step({person(1, 300, 500), person(2, 700, 500)}, 60);
        int s1 = 0, s2 = 0;
        for (const auto& e : r.all) {
            if (e.kind != "stay") continue;
            if (e.track_id == 1) s1 = e.session;
            if (e.track_id == 2) s2 = e.session;
        }
        check(s1 > 0 && s2 > 0 && s1 != s2, "两个人各自计时、各自会话");
    }
    {
        Run r(config(kDwellConfig));
        r.step({person(1, 500, 500)}, 60);
        r.step({}, 100);  // 4s > max_lost_sec
        const auto end = find_ev(r.all, "stay_end");
        check(end && end->extra.find("丢失") != std::string::npos, "滞留中目标丢失超时也结束会话");
    }
    {
        Run r(config(kDwellConfig));
        r.step({person(1, 500, 500)}, 60);
        mb::RuleEngine next;
        std::string err;
        next.load(config(kDwellConfig), classes(), &err);
        next.adopt_state(r.rules);
        r.rules = next;
        r.step({person(1, 500, 500)}, 10);
        check(count(r.all, "dwell", "stay") == 1, "热加载后不重复报 stay");
        r.step({person(2, 300, 300)}, 1);
        auto dw = r.rules.dwellers();
        int max_session = 0;
        for (const auto& d : dw) max_session = std::max(max_session, d.session);
        check(max_session == 2, "热加载后会话号继续递增, 不与旧会话重号");
    }
    {
        std::string legacy = kDwellConfig;
        legacy.replace(legacy.find("\"trigger\": \"stay\""), 17,
                       "\"trigger\": \"stay\", \"scope\": \"region\"");
        Run r(config(legacy.c_str()));
        r.step({person(1, 500, 500)}, 60);
        const auto st = find_ev(r.all, "stay");
        check(st && st->severity == mb::Severity::Info && r.rules.dwellers().empty(),
              "scope=region 保留旧的区域级计时 (Info, 不进 dwellers)");
    }
    {
        auto rejected = [](const char* text, const std::map<std::string, int>& cls) {
            mb::RuleEngine e;
            std::string err;
            return !e.load(config(text), cls, &err) && !err.empty();
        };
        const char* legacy = R"JSON({
          "features": {"dwell": {"enabled": true, "dwell_sec": 60, "region_id": "zone-1"}},
          "regions": {"zone-1": {"points": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]]}}
        })JSON";
        check(rejected(legacy, classes()),
              "dwell 缺 rule/trigger/classes 时拒绝加载, 不退化成 enter + 全类别");
        const char* wrong_trigger = R"JSON({
          "features": {"dwell": {"enabled": true, "rule": "zone", "trigger": "enter",
                                 "classes": ["person"], "region_id": "zone-1"}},
          "regions": {"zone-1": {"points": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]]}}
        })JSON";
        check(rejected(wrong_trigger, classes()), "dwell 使用 enter 触发时拒绝加载");
        const char* wrong_class = R"JSON({
          "features": {"dwell": {"enabled": true, "rule": "zone", "trigger": "stay",
                                 "classes": ["potted plant"], "region_id": "zone-1"}},
          "regions": {"zone-1": {"points": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]]}}
        })JSON";
        check(rejected(wrong_class, {{"person", 0}, {"potted plant", 58}}),
              "dwell 只允许 person 类别");
    }
}

// 滞留联动: 规则引擎 + DwellGuard 逐帧联跑
struct GuardRun {
    Run r{config(kDwellConfig)};
    mb::DwellGuard guard;
    int speaks = 0;
    bool busy = false;
    std::vector<std::pair<std::string, uint64_t>> extends;
    bool extend_ok = true;

    explicit GuardRun(const char* cfg_text = kDwellConfig, const std::string& log = "") {
        r = Run(config(cfg_text));
        mb::DwellGuardConfig gc = mb::DwellGuardConfig::from_json(config(cfg_text));
        gc.log_path = log;
        guard.configure(gc);
        guard.set_speaker([this](const mb::DwellGuard::SpeakRequest&) {
            if (busy) return false;
            ++speaks;
            return true;
        });
        guard.set_clip_extender([this](const std::string& id, uint64_t until) {
            extends.emplace_back(id, until);
            return extend_ok;
        });
    }
    void step(const std::vector<mb::Track>& tracks, int n = 1) {
        for (int i = 0; i < n; ++i) {
            auto evs = r.step(tracks);
            guard.update(r.rules.dwellers(), r.ts);
            for (const auto& e : evs) {
                // 模拟事件引擎: 告警过去抖后触发, stay 告警开始录像。
                if (e.session > 0)
                    guard.on_fired(e.session, "ev-" + e.kind, e.kind, e.kind == "stay", e.ts_ms);
            }
            guard.finish_missing();
        }
    }
};

void test_dwell_guard() {
    printf("8. 滞留联动 (扬声器 / 续录像 / 记录)\n");
    const std::string log = "/tmp/mb-ruletest-dwell.jsonl";
    ::remove(log.c_str());
    {
        GuardRun g;
        mb::RuleEngine::Dweller d;
        d.session = 1;
        d.track_id = 1;
        d.feature = "dwell";
        d.region_name = "A";
        d.level = 1;  // 规则刚发出事件, 事件引擎还没去抖
        d.seconds = 2.0f;
        g.guard.update({d}, 1000);
        check(g.speaks == 0, "原始 stay 尚未去抖确认时不提前喊话");
        g.guard.on_fired(1, "ev-stay", "stay", true, 1000);
        g.guard.update({d}, 1040);
        check(g.speaks == 1, "stay 经事件引擎确认后才喊话");
    }
    {
        GuardRun g(kDwellConfig, log);
        g.step({person(1, 500, 500)}, 45);
        check(g.speaks == 0, "未到告警不喊");
        g.step({person(1, 500, 500)}, 10);  // 2.2s
        check(g.speaks == 1, "stay 时喊第一次");
        g.busy = true;
        g.step({person(1, 500, 500)}, 30);  // 3.4s, 播放器忙
        check(g.speaks == 1, "播放器忙时不叠着放, 也不计次数");
        g.busy = false;
        g.step({person(1, 500, 500)}, 1);
        check(g.speaks == 2, "空闲后立刻补喊");
        g.step({person(1, 500, 500)}, 150);  // 9.4s
        check(g.speaks == 3, "repeat_sec 间隔重复, 不超过 max_times");

        check(!g.extends.empty() && g.extends.front().first == "ev-stay", "人在区内时续录像");
        uint64_t last = 0;
        bool mono = true;
        for (const auto& e : g.extends) {
            mono = mono && e.second > last;
            last = e.second;
        }
        const auto st = find_ev(g.r.all, "stay");
        check(mono && st && last == st->ts_ms + 5000, "续录单调后推, 封顶 record.max_sec");
        const size_t n_ext = g.extends.size();
        g.step({person(1, 500, 950)}, 5);
        check(g.extends.size() == n_ext, "封顶后/离开后不再续");

        mb::Json j = g.guard.json();
        check(j["active"].size() == 0 && j["history"].size() == 1, "离开后会话进历史");
        const mb::Json& h = j["history"][0];
        check(h["warnings"].integer(0) == 3 && h["level"].str("") == "stay_long" &&
                  h["left_after_warning"].boolean(false) && h["clip_event"].str("") == "ev-stay",
              "历史记录: 喊话次数 / 最高级别 / 劝离 / 关联录像");
        bool has_end_event = false;
        for (const mb::Json& e : h["events"].items())
            has_end_event = has_end_event || e.str("") == "ev-stay_end";
        check(has_end_event, "stay_end 结束事件也写入对应的滞留会话");
        check(h["duration_sec"].number(0) > 9.0, "历史记录带停留时长");
    }
    {
        GuardRun g(kDwellConfig, log);
        g.step({person(1, 500, 500)}, 25);  // 路过
        g.step({person(1, 500, 950)}, 5);
        check(g.guard.json()["history"].size() == 0, "路过的不记录");
    }
    {
        // 告警录像已收尾 (extender 返回 false) 后不再反复续
        GuardRun g(kDwellConfig, "");
        g.extend_ok = false;
        g.step({person(1, 500, 500)}, 80);
        check(g.extends.size() == 1, "录像已收尾就停止续期");
    }
    {
        // 预警劝离: start_sec 小于 dwell_sec, 喊完人走了 => 没有告警, 但记一条劝离
        std::string pre = kDwellConfig;
        pre.replace(pre.find("\"repeat_sec\": 1"), 15, "\"start_sec\": 1, \"repeat_sec\": 1");
        GuardRun g(pre.c_str(), log);
        g.step({person(1, 500, 500)}, 30);  // 1.2s
        check(g.speaks == 1, "start_sec 到点先喊 (还没到 stay 告警)");
        g.step({person(1, 500, 950)}, 5);
        check(count(g.r.all, "dwell", "stay") == 0, "喊完离开, 没有滞留告警");
        mb::Json h = g.guard.json()["history"][0];
        check(h["level"].str("") == "counting" && h["left_after_warning"].boolean(false),
              "记一条劝离成功");
    }
    {
        FILE* f = fopen(log.c_str(), "r");
        int lines = 0;
        bool parsed = true;
        char buf[4096];
        while (f && fgets(buf, sizeof(buf), f)) {
            mb::Json j;
            std::string err;
            parsed = parsed && mb::Json::parse(buf, &j, &err) && j["session"].integer(0) > 0;
            ++lines;
        }
        if (f) fclose(f);
        check(lines == 2 && parsed, "JSONL 滞留记录: 每个会话一行, 路过不写");
        ::remove(log.c_str());
    }
}

mb::Detection det(float cx, float cy, float score = 0.9f) {
    mb::Detection d;
    d.cls_id = 0;
    d.score = score;
    d.x1 = cx - 40;
    d.x2 = cx + 40;
    d.y1 = cy - 100;
    d.y2 = cy + 100;
    return d;
}

void test_tracker() {
    printf("9. 跟踪器\n");
    mb::Tracker tr;
    // 两个人并排, 框 IoU ~0.45 (> 关联门槛 0.3): 第二个人也要有自己的轨迹
    std::vector<mb::Track> out;
    for (int f = 0; f < 5; ++f) out = tr.update({det(500, 500), det(530, 500)}, f);
    int confirmed = 0;
    for (const auto& t : out) confirmed += t.confirmed && t.lost == 0;
    check(confirmed == 2, "挨着的两个人各有一条确认轨迹");

    mb::Tracker tr2;
    int id = -1;
    float x = 100;
    for (int f = 0; f < 6; ++f, x += 30) {
        out = tr2.update({det(x, 500)}, f);
        if (!out.empty()) id = out[0].id;
    }
    // 漏检 3 帧, 人继续以 30px/帧 走, 重现时与旧框 IoU 已为 0
    for (int f = 0; f < 3; ++f, x += 30) tr2.update({}, 6 + f);
    out = tr2.update({det(x, 500)}, 9);
    bool same = false;
    for (const auto& t : out) same |= t.id == id && t.lost == 0;
    check(same, "移动目标漏检 3 帧后靠预测框接回同一 id");

    // 检测器在书包和猫之间来回跳时, 保持轨迹和稳定类别; 持续的新类别最终能接管。
    mb::Tracker tr3;
    int stable_id = -1;
    int cls_id = -1;
    for (int f = 0; f < 9; ++f) {
        mb::Detection d = det(500, 500, 0.65f);
        d.cls_id = (f % 3 == 2) ? 15 : 24;  // COCO: 15=cat, 24=backpack
        out = tr3.update({d}, f);
        if (!out.empty()) {
            if (stable_id < 0) stable_id = out[0].id;
            cls_id = out[0].cls_id;
        }
    }
    check(out.size() == 1 && out[0].id == stable_id && cls_id == 24,
          "同类物体类别抖动时不换 id, 类别按多数证据保持 backpack");
    for (int f = 9; f < 18; ++f) {
        mb::Detection d = det(500, 500, 0.65f);
        d.cls_id = 15;
        out = tr3.update({d}, f);
    }
    check(out.size() == 1 && out[0].id == stable_id && out[0].cls_id == 15,
          "新类别持续出现后沿用同一 id 并稳定切换到新类别");

    // 跨类别关联不能把重叠的人和其他目标合并成一条轨迹。
    mb::Tracker tr4;
    for (int f = 0; f < 6; ++f) {
        mb::Detection person = det(500, 500);
        mb::Detection bag = det(530, 500);
        bag.cls_id = 24;
        out = tr4.update({person, bag}, f);
    }
    check(out.size() == 2 && out[0].cls_id == 0 && out[1].cls_id == 24,
          "重叠的不同类别目标保持各自轨迹和类别");
}

// ---- 火焰烟雾 ----
// D-Fire 约定 0=smoke 1=fire; 3fps 推理 → 每 333ms 一次结果
const std::vector<std::string> kFireLabels = {"smoke", "fire"};
constexpr uint64_t kFireStep = 333;

mb::Detection fire_box(int cls, float score, float size = 100.f) {
    mb::Detection d;
    d.cls_id = cls;
    d.score = score;
    d.x1 = 400;
    d.y1 = 400;
    d.x2 = 400 + size;
    d.y2 = 400 + size;
    return d;
}

struct FireRun {
    std::vector<mb::RuleEvent> all;
    int first_alarm_step = -1;
};

// pattern: 每个字符一次推理, 'F'=火焰 'S'=烟雾 'B'=都有 'l'=低分火焰 't'=很小的火焰 '.'=无
void feed(mb::FireDetector* fd, const std::string& pattern, FireRun* r, uint64_t t0 = 1000) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        std::vector<mb::Detection> dets;
        const char c = pattern[i];
        if (c == 'F' || c == 'B') dets.push_back(fire_box(1, 0.8f));
        if (c == 'S' || c == 'B') dets.push_back(fire_box(0, 0.8f));
        if (c == 'l') dets.push_back(fire_box(1, 0.2f));
        if (c == 't') dets.push_back(fire_box(1, 0.9f, 5.f));
        for (const auto& e : fd->update(dets, W, H, t0 + i * kFireStep)) {
            r->all.push_back(e);
            if (e.severity == mb::Severity::Alarm && r->first_alarm_step < 0) r->first_alarm_step = (int)i;
        }
    }
}

int count_kind(const std::vector<mb::RuleEvent>& all, const char* kind) {
    int n = 0;
    for (const auto& e : all) n += e.feature == "fire" && e.kind == kind;
    return n;
}

void test_fire() {
    printf("\n[火焰烟雾]\n");
    const mb::FireParams fp;  // fire 1.0s / smoke 3.0s, hit_ratio 0.6, gap 1.5s, clear 10s

    {
        mb::FireDetector fd(fp, kFireLabels);
        FireRun r;
        feed(&fd, "F.........", &r);
        check(count_kind(r.all, "fire") == 0, "单帧闪现不报");
    }
    {
        mb::FireDetector fd(fp, kFireLabels);
        FireRun r;
        feed(&fd, "FFFFFFFFFFFFFFFFFFFF", &r);
        check(count_kind(r.all, "fire") == 1, "持续火焰只报一次");
        check(r.first_alarm_step == 4, "持续满 1.0s (第 5 次推理, 3*333ms 还差 1ms) 才报");
        check(fd.fire_active() && !fd.smoke_active(), "火焰状态 active, 烟雾不受影响");
    }
    {
        mb::FireDetector fd(fp, kFireLabels);
        FireRun r;
        feed(&fd, "FFF.FFF.FFF", &r);
        check(count_kind(r.all, "fire") == 1, "偶尔漏检 (3/4 命中) 仍能确认");
    }
    {
        mb::FireDetector fd(fp, kFireLabels);
        FireRun r;
        feed(&fd, "F..F..F..F..F..F..", &r);
        check(count_kind(r.all, "fire") == 0, "稀疏命中 (1/3) 投票不过");
    }
    {
        mb::FireDetector fd(fp, kFireLabels);
        FireRun r;
        feed(&fd, "llllllllll", &r);
        check(count_kind(r.all, "fire") == 0 && fd.visible().empty(), "低于 fire_conf 的框忽略");
        feed(&fd, "tttttttttt", &r, 100000);
        check(count_kind(r.all, "fire") == 0, "小于 min_area_ratio 的框忽略");
    }
    {
        mb::FireDetector fd(fp, kFireLabels);
        FireRun r;
        // 命中 2 次 → 中断 5 次 (1.67s > gap 1.5s) → 再命中 2 次: 不应累计成 1s
        feed(&fd, "FF.....FF", &r);
        check(count_kind(r.all, "fire") == 0, "中断超过 gap_sec 重新计时");
    }
    {
        mb::FireDetector fd(fp, kFireLabels);
        FireRun r;
        // 报警 → 30 次无火 (10s) 解除 → 再次持续火焰再报
        feed(&fd, std::string(6, 'F') + std::string(31, '.') + std::string(6, 'F'), &r);
        check(count_kind(r.all, "clear") == 1, "clear_sec 无火后解除 (Info)");
        check(count_kind(r.all, "fire") == 2, "解除后再次起火重新告警");
        bool info = false;
        for (const auto& e : r.all)
            if (e.kind == "clear") info = e.severity == mb::Severity::Info;
        check(info, "解除事件是 Info 级");
    }
    {
        mb::FireDetector fd(fp, kFireLabels);
        FireRun r;
        feed(&fd, std::string(8, 'S'), &r);
        check(count_kind(r.all, "smoke") == 0, "烟雾 2.3s 还不报 (smoke_confirm_sec=3)");
        feed(&fd, std::string(2, 'S'), &r, 1000 + 8 * kFireStep);
        check(count_kind(r.all, "smoke") == 0, "烟雾 3.0s 差 3ms 仍不报");
        feed(&fd, "S", &r, 1000 + 10 * kFireStep);
        check(count_kind(r.all, "smoke") == 1, "烟雾持续满 3s 后报");
    }
    {
        // 类号顺序反过来的模型: 按名字映射, cls 0 才是 fire
        mb::FireDetector fd(fp, {"fire", "smoke"});
        FireRun r;
        feed(&fd, "FFFFFF", &r);  // 'F' 发的是 cls 1 = 这里的 smoke
        check(count_kind(r.all, "fire") == 0 && fd.fire_cls() == 0 && fd.smoke_cls() == 1,
              "类别按名字映射, 不依赖类号顺序");
    }
    {
        mb::FireParams only_fire = fp;
        only_fire.smoke.enabled = false;
        mb::FireDetector fd(only_fire, kFireLabels);
        FireRun r;
        feed(&fd, std::string(12, 'B'), &r);
        check(count_kind(r.all, "fire") == 1 && count_kind(r.all, "smoke") == 0,
              "classes 只勾 fire 时不报烟雾");
    }
}

void test_package_time() {
    printf("11. 包裹静止时间设置\n");
    const auto package_config = [](int seconds) {
        return config(("{\"features\":{\"package\":{\"enabled\":true,\"rule\":\"object\","
                       "\"static_sec\":" + std::to_string(seconds) + "}}}").c_str());
    };
    auto package = person(1, 500, 500);
    package.cls_id = 24;
    for (int seconds : {1, 10, 20, 60, 86400}) {
        Run r(package_config(seconds));
        r.step({package});
        const uint64_t deadline = r.ts + static_cast<uint64_t>(seconds) * 1000;
        auto early = r.rules.update({package}, W, H, deadline - 1);
        check(count(early, "package", "abandoned") == 0, "包裹静止时间差 1ms 不告警");
        auto due = r.rules.update({package}, W, H, deadline);
        check(count(due, "package", "abandoned") == 1, "包裹静止恰好满设定时间告警");
        r.ts = deadline;
        r.step({package}, 10);
        check(count(r.all, "package", "abandoned") == 0, "同一次静止不重复告警");
    }

    Run moving(package_config(1));
    moving.step({package}, 20);
    auto moved = package;
    moved.x1 += 100;
    moved.x2 += 100;
    moving.step({moved}, 25);
    check(count(moving.all, "package", "abandoned") == 0, "包裹移动后重新计时");
    moving.step({moved});
    check(count(moving.all, "package", "abandoned") == 1, "移动后静止满设定时间才告警");

    Run reload(package_config(20));
    reload.step({package}, 251);
    Run shorter(package_config(10));
    shorter.rules.adopt_state(reload.rules);
    shorter.ts = reload.ts;
    shorter.step({package});
    check(count(shorter.all, "package", "abandoned") == 1, "热加载缩短门限沿用静止计时");
    Run longer(package_config(60));
    longer.rules.adopt_state(reload.rules);
    longer.ts = reload.ts;
    longer.step({package});
    check(count(longer.all, "package", "abandoned") == 0, "热加载延长门限不会提前告警");
}

// ---- 包裹 ----
const char* kPackageConfig = R"JSON({
  "track": {"relink_sec": 2.0, "relink_dist": 0.6},
  "features": {
    "package": {"enabled": true, "rule": "object", "classes": ["backpack", "handbag"],
                "static_sec": 2, "move_threshold_px": 20, "move_confirm_sec": 0.5,
                "unattended_sec": 1, "unattended_radius": 0.5, "remove_sec": 1,
                "max_lost_sec": 1}
  }
})JSON";

std::map<std::string, int> pkg_classes() {
    return {{"person", 0}, {"cat", 15}, {"backpack", 24}, {"handbag", 26}};
}

// 中心 (cx, cy)、边长 60 的包
mb::Track bag(int id, float cx, float cy, int cls = 24) {
    mb::Track t;
    t.id = id;
    t.cls_id = cls;
    t.confirmed = true;
    t.x1 = cx - 30;
    t.x2 = cx + 30;
    t.y1 = cy - 30;
    t.y2 = cy + 30;
    return t;
}

mb::Track cat(int id, float cx, float cy) {
    mb::Track t = bag(id, cx, cy, 15);
    t.x1 -= 20;
    t.x2 += 20;
    return t;
}

void test_package() {
    printf("12. 包裹 (按位置去重)\n");
    auto pkg = [] { return Run(config(kPackageConfig), pkg_classes()); };
    {
        Run r = pkg();
        r.step({bag(1, 500, 500)}, 45);  // 1.8s
        check(count(r.all, "package", "abandoned") == 0, "未满 static_sec 不报");
        r.step({bag(1, 500, 500)}, 10);
        check(count(r.all, "package", "abandoned") == 1, "静止满 2s 报 abandoned");
        const auto ev = find_ev(r.all, "abandoned");
        check(ev && ev->rule_id.rfind("pkg-", 0) == 0 && ev->severity == mb::Severity::Alarm,
              "abandoned 是告警级, rule_id 是包裹位置号");
        check(ev && r.rules.liveness(*ev) == mb::RuleEngine::Liveness::Active, "包裹在: Active");
        r.step({bag(1, 500, 500)}, 1500);  // 再放 60s
        check(count(r.all, "package", "abandoned") == 1, "长时间放着只报一次");

        // 猫从前面走过, 包框被挡掉下半截 (中心偏 15px、框变形) 3s
        for (int i = 0; i < 75; ++i) {
            mb::Track b = bag(1, 500, 500);
            b.y2 = 485;
            r.step({b, cat(50, 470 + i, 510)});
        }
        // 猫趴在包上, 包检测不到 5s
        r.step({cat(50, 500, 500)}, 125);
        check(ev && r.rules.liveness(*ev) == mb::RuleEngine::Liveness::Occluded,
              "被猫盖住: Occluded");
        // 猫走了, 包以新 id 重新出现; 期间轨迹早已过期
        r.step({bag(9, 503, 498)}, 100);
        check(count(r.all, "package", "abandoned") == 1, "猫半遮挡/完全盖住/换 ID 后不重报");
        check(count(r.all, "package", "removed") == 0, "被遮挡期间不算被拿走");

        // 无遮挡跟丢 0.6s (< remove_sec), 又换 id 且检测器把类别跳成 handbag
        r.step({}, 15);
        r.step({bag(12, 500, 500, 26)}, 100);
        check(count(r.all, "package", "abandoned") == 1 && count(r.all, "package", "removed") == 0,
              "短暂漏检 + 换 ID + 跳类别不重报, 也不报 removed");

        const uint64_t gone_at = r.ts + 40;
        r.step({}, 30);  // 无遮挡消失 1.2s
        check(count(r.all, "package", "removed") == 1, "无遮挡消失满 remove_sec 报 removed");
        const auto rm = find_ev(r.all, "removed");
        check(rm && rm->ts_ms == gone_at && rm->rule_id == ev->rule_id,
              "removed 时刻取消失那一刻 (录像能覆盖拿走过程), 同一位置号");
        check(rm && r.rules.liveness(*rm) == mb::RuleEngine::Liveness::Active,
              "removed 不按轨迹存活去抖");
        r.step({}, 50);
        check(count(r.all, "package", "removed") == 1, "removed 只报一次");
        r.step({bag(30, 500, 500)}, 60);
        check(count(r.all, "package", "abandoned") == 2, "拿走后再放一个: 新包裹重新计时再报");
    }
    {
        Run r = pkg();
        // 框底每隔几帧抖 25px (旧版直接清零计时)
        for (int i = 0; i < 60; ++i) r.step({bag(1, 500, i % 5 == 0 ? 525.0f : 500.0f)});
        check(count(r.all, "package", "abandoned") == 1, "单帧框抖动不清零计时");
    }
    {
        Run r = pkg();
        r.step({bag(1, 500, 500)}, 40);  // 1.6s
        r.step({bag(1, 700, 500)}, 30);  // 真被挪到 200px 外并放了 1.2s
        check(count(r.all, "package", "abandoned") == 0, "报警前被挪动: 重新计时");
        r.step({bag(1, 700, 500)}, 40);
        check(count(r.all, "package", "abandoned") == 1, "新位置静止满 static_sec 再报");
        r.step({bag(1, 300, 500)}, 100);  // 报过之后又被挪
        check(count(r.all, "package", "abandoned") == 1, "已报的包裹被挪动不重报");
    }
    {
        Run r = pkg();
        // 快递员 (人) 站在包旁边 3s, 然后离开
        r.step({bag(1, 500, 500), person(2, 560, 540)}, 75);
        check(count(r.all, "package", "abandoned") == 0, "旁边有人: 不报");
        r.step({bag(1, 500, 500)}, 20);  // 人走 0.8s
        check(count(r.all, "package", "abandoned") == 0, "人走了不满 unattended_sec 不报");
        r.step({bag(1, 500, 500)}, 10);
        const auto ev = find_ev(r.all, "abandoned");
        check(count(r.all, "package", "abandoned") == 1 && ev &&
                  ev->extra.find("周边无人") != std::string::npos,
              "人离开满 unattended_sec 才报, 附带无人时长");
    }
    {
        Run r = pkg();
        // 人站着, 背包在他身上
        r.step({person(2, 500, 700), bag(1, 500, 560)}, 150);
        check(count(r.all, "package", "abandoned") == 0, "人身上的包不报");
    }
    {
        Run r = pkg();
        r.step({bag(1, 500, 500)}, 60);
        mb::RuleEngine next;
        std::string err;
        next.load(config(kPackageConfig), pkg_classes(), &err);
        next.adopt_state(r.rules);
        r.rules = next;
        r.step({bag(1, 500, 500)}, 60);
        check(count(r.all, "package", "abandoned") == 1, "热加载 (如改 static_sec) 后不重报");
    }
}

// 本地时间 2026-11-02 (周一) 加 day_offset 天的 hh:mm
time_t local_time(int day_offset, int hh, int mm) {
    tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;
    t.tm_mday = 2 + day_offset;
    t.tm_hour = hh;
    t.tm_min = mm;
    t.tm_isdst = -1;
    return mktime(&t);
}

const char* kArmConfig = R"JSON({
  "features": {
    "fence": {"enabled": true, "rule": "zone", "trigger": "enter", "classes": ["person"],
              "region_id": "zone-1", "sustain_sec": 0.5,
              "arm": {"mode": "schedule",
                      "schedule": [{"days": [1], "start": "22:00", "end": "06:30"},
                                   {"days": [6, 7], "start": "09:00", "end": "17:00"}]},
              "actions": {"record": {"enabled": true, "max_sec": 6},
                          "speaker": {"enabled": true, "repeat_sec": 1, "max_times": 3}}},
    "intrusion": {"enabled": true, "rule": "zone", "trigger": "enter", "classes": ["person"],
                  "region_id": "zone-1", "sustain_sec": 0.5,
                  "actions": {"record": {"enabled": false}, "speaker": {"enabled": false}}}
  },
  "regions": {
    "zone-1": {"name": "院子", "points": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]]}
  }
})JSON";

std::string arm_variant(const char* arm_json) {
    std::string s = kArmConfig;
    const size_t a = s.find("\"arm\":");
    const size_t b = s.find("\"actions\":");
    s.replace(a, b - a, std::string("\"arm\": ") + arm_json + ",\n              ");
    return s;
}

void test_arm() {
    printf("13. 布防时段\n");
    {
        Run r(config(kArmConfig));
        r.rules.set_wall_time(local_time(0, 12, 0));  // 周一中午
        r.step({person(1, 500, 500)}, 30);
        check(!r.rules.armed("fence") && count(r.all, "fence", "enter") == 0,
              "不在布防时段: 院子里有人也不报");
        check(r.rules.armed("intrusion") && count(r.all, "intrusion", "enter") == 1,
              "没配 arm 的功能保持一直布防");

        r.rules.set_wall_time(local_time(0, 22, 0));  // 周一 22:00 布防
        r.step({person(1, 500, 500)}, 1);
        check(r.rules.armed("fence") && count(r.all, "fence", "enter") == 0,
              "刚布防不立即报, 先走 sustain");
        r.step({person(1, 500, 500)}, 15);
        check(count(r.all, "fence", "enter") == 1, "布防时人还在院子里: 按新进入报一次");
        const auto ev = find_ev(r.all, "enter");
        mb::RuleEvent fence_ev;
        for (const auto& e : r.all)
            if (e.feature == "fence" && e.kind == "enter") fence_ev = e;
        check(ev && r.rules.liveness(fence_ev) == mb::RuleEngine::Liveness::Active,
              "布防中告警 liveness=Active");

        r.rules.set_wall_time(local_time(1, 3, 0));  // 周二 03:00, 周一开始的跨零点时段
        r.step({person(1, 500, 500)}, 1);
        check(r.rules.armed("fence"), "跨零点: 前一天开始的时段延续到次日 end 之前");
        check(count(r.all, "fence", "enter") == 1, "布防期间不重复报");

        r.rules.set_wall_time(local_time(1, 6, 30));  // end 不含
        r.step({person(1, 500, 500)}, 1);
        check(!r.rules.armed("fence") &&
                  r.rules.liveness(fence_ev) == mb::RuleEngine::Liveness::Gone,
              "到 end 自动撤防, 挂起/进行中的告警作废");

        r.rules.set_wall_time(local_time(1, 23, 0));  // 周二晚上不在 days 里
        r.step({person(1, 500, 500)}, 30);
        check(!r.rules.armed("fence") && count(r.all, "fence", "enter") == 1,
              "days 只含周一: 周二晚上不布防");

        r.rules.set_wall_time(local_time(5, 9, 0));  // 周六 09:00
        r.step({person(1, 500, 500)}, 1);
        check(r.rules.armed("fence"), "第二个时段 (周末白天) 生效");
        r.rules.set_wall_time(local_time(6, 16, 59));  // 周日 16:59
        r.step({person(1, 500, 500)}, 1);
        check(r.rules.armed("fence"), "周日同样生效");
    }
    {
        Run r(config(arm_variant(R"({"mode": "off"})").c_str()));
        r.rules.set_wall_time(local_time(0, 23, 0));
        r.step({person(1, 500, 500)}, 30);
        check(!r.rules.armed("fence") && count(r.all, "fence", "enter") == 0, "mode=off 撤防");
    }
    {
        Run r(config(arm_variant(R"({"mode": "schedule", "schedule": [{"start": "00:00", "end": "24:00"}]})").c_str()));
        r.rules.set_wall_time(local_time(3, 12, 0));
        check(r.rules.armed("fence"), "省略 days = 每天; 00:00-24:00 = 全天");
    }
    {
        mb::RuleEngine e;
        std::string err;
        check(!e.load(config(arm_variant(R"({"mode": "schedule"})").c_str()), classes(), &err) &&
                  err.find("schedule") != std::string::npos,
              "schedule 为空: 加载失败");
        err.clear();
        check(!e.load(config(arm_variant(R"({"mode": "schedule", "schedule": [{"start": "25:00", "end": "06:00"}]})").c_str()),
                      classes(), &err) && !err.empty(),
              "时间写错: 加载失败");
        err.clear();
        check(!e.load(config(arm_variant(R"({"mode": "night"})").c_str()), classes(), &err) &&
                  !err.empty(),
              "未知 mode: 加载失败");
        std::string off = arm_variant(R"({"mode": "night"})");
        off.replace(off.find("\"enabled\": true"), 15, "\"enabled\": false");
        check(e.load(config(off.c_str()), classes(), &err), "功能没启用时 arm 配错不挡启动");
    }
}

// 围栏联动: 规则引擎 + ZoneAlarm 逐帧联跑, 告警直接视为已过去抖
struct AlarmRun {
    Run r;
    mb::ZoneAlarm alarm;
    int speaks = 0;
    bool busy = false;
    std::vector<std::pair<std::string, uint64_t>> extends;
    int fired = 0;

    explicit AlarmRun(const std::string& cfg_text) : r(config(cfg_text.c_str())) {
        r.rules.set_wall_time(local_time(0, 23, 0));
        alarm.configure(mb::ZoneAlarmConfig::from_json(config(cfg_text.c_str())));
        alarm.set_speaker([this](const mb::ZoneAlarm::SpeakRequest&) {
            if (busy) return false;
            ++speaks;
            return true;
        });
        alarm.set_clip_extender([this](const std::string& id, uint64_t until) {
            extends.emplace_back(id, until);
            return true;
        });
    }
    void step(const std::vector<mb::Track>& tracks, int n = 1) {
        for (int i = 0; i < n; ++i) {
            for (const auto& e : r.step(tracks)) {
                if (e.severity != mb::Severity::Alarm) continue;
                alarm.on_fired(e, "ev-" + e.feature + "-" + std::to_string(++fired), true);
            }
            alarm.update(r.ts, [this](const mb::RuleEvent& ev) { return r.rules.liveness(ev); });
        }
    }
};

void test_zone_alarm() {
    printf("14. 围栏联动 (扬声器 / 续录像)\n");
    {
        AlarmRun g(kArmConfig);
        g.step({person(1, 500, 500)}, 15);  // 0.6s, 两个功能都报 enter
        check(g.speaks == 1, "fence 告警确认即喊; 音频关闭的 intrusion 不喊");
        g.busy = true;
        g.step({person(1, 500, 500)}, 30);  // 1.8s
        check(g.speaks == 1, "播放器忙时不叠放、不计次数");
        g.busy = false;
        g.step({person(1, 500, 500)}, 1);
        check(g.speaks == 2, "空闲后补喊");
        g.step({person(1, 500, 500)}, 100);  // 5.8s
        check(g.speaks == 3, "repeat_sec 间隔重复, 不超过 max_times");
        check(g.alarm.summary()["active"].integer(-1) == 1, "人在区内告警保持进行中");

        bool only_fence = !g.extends.empty();
        uint64_t last = 0;
        for (const auto& e : g.extends) {
            only_fence = only_fence && e.first.rfind("ev-fence-", 0) == 0;
            last = std::max(last, e.second);
        }
        mb::RuleEvent fence_ev;
        for (const auto& e : g.r.all)
            if (e.feature == "fence" && e.kind == "enter") fence_ev = e;
        check(only_fence && last == fence_ev.ts_ms + 6000, "只续 fence 的录像, 封顶 record.max_sec");

        g.step({person(1, 500, 950)}, 2);
        check(g.alarm.summary()["active"].integer(-1) == 0, "离开后告警结束");
        g.step({person(1, 500, 500)}, 15);
        check(g.speaks == 4, "再次闯入重新喊");
    }
    {
        AlarmRun g(kArmConfig);
        g.step({person(1, 500, 500)}, 15);
        check(g.speaks == 1, "布防中闯入: 喊");
        g.r.rules.set_wall_time(local_time(1, 7, 0));  // 撤防
        g.step({person(1, 500, 500)}, 60);
        check(g.speaks == 1 && g.alarm.summary()["active"].integer(-1) == 0,
              "撤防后停止喊话和续录");
    }
    {
        AlarmRun g(kArmConfig);
        g.step({person(1, 500, 500)}, 15);
        g.step({}, 40);  // 被挡住 1.6s (max_lost_sec 3s 内)
        check(g.speaks == 1 && g.alarm.summary()["active"].integer(-1) == 1,
              "目标被遮挡: 不喊但告警不结束");
        g.step({}, 50);
        check(g.alarm.summary()["active"].integer(-1) == 0, "丢失超过 max_lost_sec 结束");
    }
}

void test_event_audio() {
    printf("18. 事件音频等级 / 默认配置 / 独立开关\n");
    const mb::Json cfg = config(R"JSON({"features": {
        "intrusion": {"enabled": true, "rule": "zone", "trigger": "enter"},
        "fence": {"enabled": true, "rule": "zone", "trigger": "cross"},
        "fall": {"enabled": true},
        "fire": {"enabled": true},
        "dwell": {"enabled": true, "rule": "zone", "trigger": "stay"}
    }})JSON");
    auto ac = mb::ZoneAlarmConfig::from_json(cfg);
    auto dc = mb::DwellGuardConfig::from_json(cfg);
    dc.log_path = "";
    check(ac.features.size() == 4 && ac.features.at("intrusion").speaker &&
          ac.features.at("fence").speaker && ac.features.at("fall").speaker &&
          ac.features.at("fire").speaker &&
          dc.features.at("dwell").speaker, "旧配置未写 speaker 时五类事件默认都有音频");
    check(!ac.features.at("intrusion").record && !ac.features.at("fall").record,
          "补音频不改变未配 actions 的入侵/跌倒录像长度");

    mb::ZoneAlarm alarm;
    alarm.configure(ac);
    bool busy = true;
    std::vector<mb::ZoneAlarm::SpeakRequest> played;
    alarm.set_speaker([&](const mb::ZoneAlarm::SpeakRequest& r) {
        if (busy) return false;
        played.push_back(r);
        return true;
    });
    mb::RuleEvent ev;
    ev.feature = "fall";
    ev.kind = "fall";
    ev.severity = mb::Severity::Alarm;
    ev.track_id = 1;
    alarm.on_fired(ev, "fall-1", true, mb::AlarmTone::Critical);
    alarm.update(1000, nullptr);
    check(played.empty() && alarm.summary()["active"].integer(-1) == 1,
          "跌倒确认后播放器忙时保留待播, 不依赖区域 liveness");
    busy = false;
    alarm.update(1040, nullptr);
    alarm.update(50000, nullptr);
    check(played.size() == 1 && played.back().tone == mb::AlarmTone::Critical &&
          alarm.summary()["active"].integer(-1) == 0, "跌倒空闲后补播紧急音且每条事件只播一次");
    alarm.on_fired(ev, "fall-info", false, mb::AlarmTone::Silent);
    alarm.update(50040, nullptr);
    check(played.size() == 1, "映射为信息级的事件不播放音频");
    ac.features.at("fall").speaker = false;
    alarm.configure(ac);
    alarm.on_fired(ev, "fall-off", true, mb::AlarmTone::Critical);
    alarm.update(50100, nullptr);
    check(played.size() == 1 && alarm.summary()["active"].integer(-1) == 0,
          "跌倒音频开关关闭不播放, 不积压");
    ac.features.at("fall").speaker = true;
    alarm.configure(ac);
    busy = true;
    alarm.on_fired(ev, "fall-clear", false, mb::AlarmTone::Critical);
    alarm.update(50200, nullptr);
    alarm.clear();
    busy = false;
    alarm.update(50300, nullptr);
    check(played.size() == 1, "进入待机时取消待播跌倒音频");

    int extended = 0;
    alarm.set_clip_extender([&](const std::string&, uint64_t) {
        ++extended;
        return true;
    });
    const auto active = [](const mb::RuleEvent&) { return mb::RuleEngine::Liveness::Active; };
    for (const std::string feature : {"intrusion", "fence"}) {
        ev.feature = feature;
        ev.kind = "enter";
        ac.features.at(feature).record = true;
        ac.features.at(feature).speaker_repeat_sec = 0;
        alarm.configure(ac);
        alarm.on_fired(ev, feature + "-1", true, mb::AlarmTone::Warning);
        alarm.update(1000, active);
        check(played.back().feature == feature && played.back().tone == mb::AlarmTone::Warning,
              "入侵/围栏事件使用实际告警等级");
        const auto n = played.size();
        ac.features.at(feature).speaker = false;
        alarm.configure(ac);
        alarm.update(20000, active);
        check(played.size() == n && extended > 0, "热加载关闭音频仍保留区域录像联动");
        alarm.clear();
    }

    {
        printf("  火焰烟雾告警音频\n");
        const auto cfg = config(R"JSON({"features": {"fire": {"enabled": true}}})JSON");
        auto ac = mb::ZoneAlarmConfig::from_json(cfg);
        check(ac.features.at("fire").speaker && !ac.features.at("fire").record,
              "火焰烟雾默认启用音频, 不改变录像长度");
        mb::ZoneAlarm alarm;
        alarm.configure(ac);
        bool busy = true;
        std::vector<mb::ZoneAlarm::SpeakRequest> played;
        alarm.set_speaker([&](const mb::ZoneAlarm::SpeakRequest& r) {
            if (busy) return false;
            played.push_back(r);
            return true;
        });
        int extended = 0;
        alarm.set_clip_extender([&](const std::string&, uint64_t) {
            ++extended;
            return true;
        });
        mb::FireDetector detector(mb::FireParams{}, kFireLabels);
        FireRun run;
        feed(&detector, std::string(12, 'B'), &run);
        check(count_kind(run.all, "fire") == 1 && count_kind(run.all, "smoke") == 1,
              "火焰和烟雾经真实时序确认各产出一条告警");
        for (const auto& ev : run.all)
            alarm.on_fired(ev, "event-" + ev.kind, true, mb::AlarmTone::Critical);
        alarm.update(5000, nullptr);
        check(played.empty() && alarm.summary()["active"].integer(-1) == 2,
              "火焰烟雾播放器忙时均保留待播, 不依赖区域存活");
        busy = false;
        alarm.update(5040, nullptr);
        alarm.update(50000, nullptr);
        check(played.size() == 2 &&
              played[0].tone == mb::AlarmTone::Critical &&
              played[1].tone == mb::AlarmTone::Critical &&
              alarm.summary()["active"].integer(-1) == 0 && extended == 0,
              "火焰烟雾各播放一次紧急音, 不重复播放或续录像");
        feed(&detector, std::string(32, '.'), &run, 6000);
        for (const auto& ev : run.all)
            if (ev.kind == "clear") alarm.on_fired(ev, "clear-" + ev.rule_id, false);
        alarm.update(50040, nullptr);
        check(played.size() == 2 && count_kind(run.all, "clear") == 2,
              "火焰和烟雾解除事件均不播放");
        const auto fire = run.all.front();
        ac.features.at("fire").speaker = false;
        alarm.configure(ac);
        alarm.on_fired(fire, "off", false, mb::AlarmTone::Critical);
        alarm.update(50100, nullptr);
        check(played.size() == 2 && alarm.summary()["active"].integer(-1) == 0,
              "关闭火焰烟雾音频后不播放或积压");
        ac.features.at("fire").speaker = true;
        alarm.configure(ac);
        busy = true;
        alarm.on_fired(fire, "pending-off", false, mb::AlarmTone::Critical);
        alarm.update(50200, nullptr);
        ac.features.at("fire").speaker = false;
        alarm.configure(ac);
        busy = false;
        alarm.update(50300, nullptr);
        check(played.size() == 2 && alarm.summary()["active"].integer(-1) == 0,
              "热加载关闭开关取消待播火焰烟雾");
        ac.features.at("fire").speaker = true;
        alarm.configure(ac);
        alarm.on_fired(fire, "info", false, mb::AlarmTone::Silent);
        alarm.update(50400, nullptr);
        check(played.size() == 2, "映射信息级不播放火焰烟雾音频");
        alarm.on_fired(fire, "warning", false, mb::AlarmTone::Warning);
        alarm.update(50500, nullptr);
        check(played.size() == 3 && played.back().tone == mb::AlarmTone::Warning,
              "火焰烟雾遵从事件引擎实际等级");
        const auto muted = mb::ZoneAlarmConfig::from_json(config(R"JSON({"features": {
            "fire": {"enabled": true, "actions": {"speaker": {"enabled": false}}}
        }})JSON"));
        check(!muted.features.at("fire").speaker, "解析保留显式关闭的火焰烟雾音频");
    }

    mb::DwellGuard guard;
    dc.features.at("dwell").speaker_max_times = 1;
    guard.configure(dc);
    std::vector<mb::AlarmTone> tones;
    guard.set_speaker([&](const mb::DwellGuard::SpeakRequest& r) {
        if (busy) return false;
        tones.push_back(r.tone);
        return true;
    });
    mb::RuleEngine::Dweller d;
    d.feature = "dwell";
    d.session = 1;
    d.track_id = 1;
    d.visible = true;
    guard.update({d}, 1000);
    guard.on_fired(1, "stay", "stay", false, 1000, mb::AlarmTone::Warning);
    guard.update({d}, 1040);
    guard.on_fired(1, "stay-long", "stay_long", false, 1100, mb::AlarmTone::Critical);
    busy = true;
    guard.update({d}, 1140);
    check(tones.size() == 1, "滞留升级时播放器忙不计次数");
    busy = false;
    guard.update({d}, 1180);
    guard.update({d}, 50000);
    check(tones == std::vector<mb::AlarmTone>{mb::AlarmTone::Warning, mb::AlarmTone::Critical},
          "滞留升级播放急促音, 即使普通告警已达次数上限");
    dc.features.at("dwell").speaker = false;
    guard.configure(dc);
    d.session = 2;
    guard.update({d}, 50100);
    guard.on_fired(2, "stay-off", "stay", false, 50100);
    guard.update({d}, 50200);
    check(tones.size() == 2, "滞留独立音频开关关闭后不再播放");

    const auto off = mb::ZoneAlarmConfig::from_json(config(R"JSON({"features": {
        "intrusion": {"rule": "zone", "actions": {"speaker": {"enabled": false}}},
        "fence": {"rule": "zone", "actions": {"speaker": {"enabled": false}}},
        "fall": {"actions": {"speaker": {"enabled": false}}}
    }})JSON"));
    check(!off.features.at("intrusion").speaker && !off.features.at("fence").speaker &&
          !off.features.at("fall").speaker, "配置解析保留用户显式关闭的音频开关");
}

void test_alarm_waveform() {
    printf("19. 警笛 WAV 格式 / 节奏\n");
    auto read_tone = [](mb::AlarmTone tone) {
        char path[] = "/tmp/mb-tone-test-XXXXXX";
        const int fd = mkstemp(path);
        if (fd < 0) {
            check(false, "创建音频测试临时文件");
            return std::vector<unsigned char>{};
        }
        close(fd);
        const bool ok = mb::write_alarm_wav(path, 1.5, tone);
        check(ok, "生成 1.5 秒警笛 WAV");
        std::vector<unsigned char> wav(48044);
        FILE* f = fopen(path, "rb");
        const bool read_ok = f && fread(wav.data(), 1, wav.size(), f) == wav.size() &&
                             fgetc(f) == EOF;
        if (f) fclose(f);
        unlink(path);
        check(read_ok, "WAV 长度恰好为 44 字节头 + 24000 个 16bit 采样");
        if (!read_ok) wav.clear();
        return wav;
    };
    const auto warning = read_tone(mb::AlarmTone::Warning);
    const auto critical = read_tone(mb::AlarmTone::Critical);
    if (warning.empty() || critical.empty()) return;
    for (const auto* wav : {&warning, &critical}) {
        check(std::string(wav->begin(), wav->begin() + 4) == "RIFF" &&
              (*wav)[20] == 1 && (*wav)[22] == 1 &&
              (*wav)[24] == 0x80 && (*wav)[25] == 0x3e && (*wav)[34] == 16,
              "警笛为 PCM / 单声道 / 16000Hz / 16bit");
    }
    auto frequency = [](const std::vector<unsigned char>& wav, int start_ms) {
        int crossings = 0;
        int previous = 0;
        const int begin = start_ms * 16;
        for (int i = begin; i < begin + 800; ++i) {
            const int pos = 44 + i * 2;
            const int raw = wav[pos] | (wav[pos + 1] << 8);
            const int sample = raw >= 32768 ? raw - 65536 : raw;
            if (i > begin && previous <= 0 && sample > 0) ++crossings;
            previous = sample;
        }
        return crossings * 20;
    };
    const auto near = [](int actual, int expected) { return std::abs(actual - expected) <= 20; };
    check(near(frequency(warning, 20), 880) && near(frequency(warning, 120), 880) &&
          near(frequency(warning, 270), 660), "告警以 250ms 节奏交替 880/660Hz");
    check(near(frequency(critical, 20), 1320) && near(frequency(critical, 120), 990) &&
          near(frequency(critical, 220), 1320), "紧急以 100ms 节奏交替 1320/990Hz");
    mb::Speaker speaker;
    check(speaker.command_for("custom.wav", "hw:1", "custom-player",
                              mb::AlarmTone::Critical) == "custom-player",
          "自定义命令优先于文件、设备和内置等级音");
    check(speaker.command_for("custom.wav", "hw:1", "", mb::AlarmTone::Critical) ==
          "aplay -q -D 'hw:1' 'custom.wav'", "自定义音频文件和设备仍然生效");
    check(speaker.command_for("", "", "custom-player", mb::AlarmTone::Silent).empty(),
          "信息级即使配置自定义命令也不启动音频");

    const mb::SpeakerConfig parsed = mb::SpeakerConfig::from_json(config(R"JSON({
        "events": {"audio": {
            "warning_coalesce_sec": 8,
            "critical_coalesce_sec": 2,
            "max_delay_sec": 4,
            "preempt_lower_priority": false
        }}
    })JSON"));
    check(parsed.warning_coalesce_sec == 8 && parsed.critical_coalesce_sec == 2 &&
              parsed.max_delay_sec == 4 && !parsed.preempt_lower_priority,
          "事件音频合并窗口 / 过期时间可配置");

    mb::SpeakerConfig crowd_cfg;
    crowd_cfg.warning_coalesce_sec = 10;
    crowd_cfg.critical_coalesce_sec = 3;
    crowd_cfg.max_delay_sec = 5;
    crowd_cfg.preempt_lower_priority = true;
    mb::Speaker crowd;
    crowd.configure(crowd_cfg);
    check(crowd.announce("sleep 2", {}, mb::AlarmTone::Warning) ==
              mb::AnnounceResult::Started,
          "大量事件的首条普通告警立即播放");
    check(crowd.announce("true", {}, mb::AlarmTone::Warning) ==
              mb::AnnounceResult::Coalesced,
          "合并窗口内后续普通告警不排队补播");
    check(crowd.announce("true", {}, mb::AlarmTone::Critical) ==
              mb::AnnounceResult::Started,
          "紧急告警立即打断普通告警");
    check(crowd.announce("true", {}, mb::AlarmTone::Critical) ==
              mb::AnnounceResult::Coalesced,
          "同一波紧急告警只响一次");

    mb::Speaker stale;
    stale.configure(crowd_cfg);
    check(stale.announce("true", {}, mb::AlarmTone::Warning, 5000) ==
              mb::AnnounceResult::Coalesced,
          "等待超时的历史告警直接丢弃, 不延后播报");
}

void test_detector_gate() {
    printf("15. 检测模型开关 / 画框类别\n");
    const std::map<std::string, int> cls = {{"person", 0}, {"car", 2}, {"dog", 16}};
    auto load = [&](const char* text) {
        mb::RuleEngine r;
        std::string err;
        if (!r.load(config(text), cls, &err)) printf("规则加载失败: %s\n", err.c_str());
        return r;
    };

    // 物体检测关、跌倒/火焰开 (各自的模型): 不需要 YOLO, 什么框都不画
    mb::RuleEngine off = load(R"JSON({"features": {
        "object": {"enabled": false, "model": "yolo", "classes": ["person", "car", "dog"]},
        "fall": {"enabled": true, "model": "pose", "classes": ["person"]},
        "fire": {"enabled": true, "classes": ["fire", "smoke"]}}})JSON");
    check(!off.needs_detector(), "只开跌倒/火焰时规则引擎不要求 YOLO");
    check(!off.shows_class(0) && !off.shows_class(2), "物体检测关闭后不画框");

    // 物体检测开: 只画它配置的类别
    mb::RuleEngine obj = load(R"JSON({"features": {
        "object": {"enabled": true, "model": "yolo", "classes": ["person", "dog"]}}})JSON");
    check(obj.needs_detector(), "物体检测开启需要 YOLO");
    check(obj.shows_class(0) && obj.shows_class(16) && !obj.shows_class(2),
          "只画 object.classes 里的类别");

    // 物体检测关、定位开: YOLO 仍要跑, 只画定位关心的类别
    mb::RuleEngine loc = load(R"JSON({"features": {
        "object": {"enabled": false, "model": "yolo", "classes": ["person", "car", "dog"]},
        "locate": {"enabled": true, "rule": "homography", "classes": ["car"],
                   "region_id": "g"}},
        "regions": {"g": {"points": [[0,1],[1,1],[1,0],[0,0]], "real_size_m": [5, 5]}}})JSON");
    check(loc.needs_detector(), "其他检测类功能开着时 YOLO 继续跑");
    check(loc.shows_class(2) && !loc.shows_class(0) && !loc.shows_class(16),
          "物体检测关闭后只画其他功能的类别");
}

void test_region_roles() {
    printf("16. 区域用途 / 忽略区\n");
    const char* text = R"JSON({
      "features": {
        "intrusion": {"enabled": true, "rule": "zone", "trigger": "enter", "classes": ["person"],
                      "region_id": "garden", "sustain_sec": 0.2},
        "fence": {"enabled": false, "rule": "zone", "trigger": "cross", "region_id": "road"},
        "count": {"enabled": true, "rule": "line", "classes": ["person"], "line_ids": ["gate"]}
      },
      "regions": {
        "garden": {"role": "private", "points": [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]]},
        "road": {"role": "public", "points": [[0.0, 0.5], [1.0, 0.5], [1.0, 1.0], [0.0, 1.0]]},
        "tree": {"role": "ignore", "points": [[0.6, 0.6], [0.9, 0.6], [0.9, 0.9], [0.6, 0.9]]}
      },
      "lines": {
        "gate": {"p1": [0.1, 0.3], "p2": [0.9, 0.3]},
        "unused": {"p1": [0.1, 0.7], "p2": [0.9, 0.7]}
      }
    })JSON";
    Run r(config(text));
    const auto& regs = r.rules.regions();
    auto region = [&](const char* id) -> const mb::NamedPolygon& {
        for (const auto& g : regs)
            if (g.id == id) return g;
        return regs.front();
    };
    check(region("garden").role == "private" && region("tree").role == "ignore", "读取 role");
    check(r.rules.region_color(region("garden")) == 0xFF30D158u &&
              r.rules.region_color(region("road")) == 0xFFFFB020u &&
              r.rules.region_color(region("tree")) == 0xFF00A5FFu,
          "区域独立配色与 APP 调色板一致, 不按用途重复颜色");
    check(r.rules.region_active(region("garden")), "被启用功能引用的区域要画");
    check(!r.rules.region_active(region("road")), "只被关闭功能引用的区域不画");
    check(r.rules.region_active(region("tree")), "忽略区总是画出来");
    bool gate_on = false, unused_on = true;
    for (const auto& l : r.rules.lines()) {
        if (l.id == "gate") gate_on = r.rules.line_active(l);
        if (l.id == "unused") unused_on = r.rules.line_active(l);
    }
    check(gate_on && !unused_on, "只画启用功能用到的线");

    check(r.rules.ignored({750, 750}, W, H) && !r.rules.ignored({300, 750}, W, H),
          "脚点落在忽略区内才算忽略");
    r.step({person(1, 750, 750)}, 20);
    check(count(r.all, "intrusion", "enter") == 0, "忽略区里的人不触发入侵");
    r.step({person(2, 300, 750)}, 20);
    check(count(r.all, "intrusion", "enter") == 1, "忽略区外照常告警");

    auto rejects = [&](const char* bad, const char* what) {
        mb::RuleEngine e;
        std::string err;
        check(!e.load(config(bad), classes(), &err) && !err.empty(), what);
    };
    rejects(R"JSON({"regions": {"a": {"role": "garden", "points": [[0,0],[1,0],[1,1]]}}})JSON",
            "未知 role 被拒绝");
    rejects(R"JSON({"features": {"intrusion": {"enabled": true, "rule": "zone", "trigger": "enter",
                    "region_id": "t"}},
                    "regions": {"t": {"role": "ignore", "points": [[0,0],[1,0],[1,1]]}}})JSON",
            "启用的功能不能指向忽略区");
}

void test_activity_map() {
    printf("17. 活动热图\n");
    const std::map<std::string, int> cls = {{"person", 0}, {"car", 2}, {"dog", 16}};
    mb::ActivityMap map;
    mb::ActivityMap::Params p;
    p.cols = 10;
    p.rows = 10;
    p.path.clear();
    map.configure(p, cls);

    auto with_cls = [](mb::Track t, int c) {
        t.cls_id = c;
        return t;
    };
    uint64_t ts = 1000;
    // 停着的车: 200 帧都在同一格, 只计一次
    for (int i = 0; i < 200; ++i) map.update({with_cls(person(1, 550, 550), 2)}, W, H, ts += 40, nullptr);
    // 人从左走到右, 穿过 10 个格子
    for (int x = 50; x < 1000; x += 20) map.update({person(2, (float)x, 250)}, W, H, ts += 40, nullptr);
    // 狗不统计
    map.update({with_cls(person(3, 50, 950), 16)}, W, H, ts += 40, nullptr);

    mb::Json j = map.json();
    auto cell = [&](const char* g, int cx, int cy) { return j[g][(size_t)(cy * 10 + cx)].integer(-1); };
    check(cell("vehicle", 5, 5) == 1, "停着的车只计一次");
    check(j["tracks"]["vehicle"].integer(0) == 1 && j["tracks"]["person"].integer(0) == 1,
          "按轨迹计目标数");
    int row = 0;
    for (int x = 0; x < 10; ++x) row += cell("person", x, 2);
    check(row == 10, "行人穿过的每个格子各计一次");
    check(cell("person", 0, 9) == 0 && cell("vehicle", 0, 9) == 0, "非人/车类别不统计");

    // 同一 id 消失超过 10 秒再出现在同一格: 当作新目标
    map.update({with_cls(person(1, 550, 550), 2)}, W, H, ts += 20000, nullptr);
    check(map.json()["vehicle"][55].integer(0) == 2, "长时间消失后再出现重新计数");

    // 忽略区里的目标不统计
    Run r(config(R"JSON({"regions": {"tree": {"role": "ignore",
          "points": [[0.0, 0.8], [0.3, 0.8], [0.3, 1.0], [0.0, 1.0]]}}})JSON"));
    map.update({person(9, 100, 950)}, W, H, ts += 40, &r.rules);
    check(map.json()["person"][90].integer(-1) == 0, "忽略区里的目标不进热图");

    map.reset();
    check(map.json()["tracks"]["person"].integer(-1) == 0 && map.json()["person"][25].integer(-1) == 0,
          "reset 清零");

    // 落盘 / 读回
    char tmpl[] = "/tmp/mb-activity-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    p.path = tmpl;
    map.configure(p, cls);
    map.update({person(4, 50, 50)}, W, H, ts += 40, nullptr);
    std::string err;
    check(map.maybe_save(ts, true, &err), "热图写盘");
    mb::ActivityMap back;
    back.configure(p, cls);
    check(back.load(&err) && back.json()["person"][0].integer(0) == 1, "热图读回");
    mb::ActivityMap::Params q = p;
    q.cols = 12;
    mb::ActivityMap other;
    other.configure(q, cls);
    check(!other.load(&err), "网格尺寸变了不读旧数据");
    unlink(tmpl);

    // 方向 / 出现消失 / 夜间
    mb::ActivityMap fm;
    mb::ActivityMap::Params fp;
    fp.cols = 10;
    fp.rows = 10;
    fp.path.clear();
    fm.configure(fp, cls);
    fm.set_wall_time(local_time(0, 23, 0));
    uint64_t ft = 1000;
    // 从 (5,5) 格向下走到 (5,8) 格, 然后消失
    for (int y = 550; y < 900; y += 20) fm.update({person(7, 550, (float)y)}, W, H, ft += 40, nullptr);
    fm.update({}, W, H, ft += 11000, nullptr);
    mb::Json fj = fm.json();
    check(fj["version"].integer(0) == 2, "热图 version 2");
    check(fj["births"]["person"][55].integer(0) == 1 && fj["deaths"]["person"][85].integer(0) == 1,
          "记录轨迹出现 / 消失的格子");
    check(fj["night"]["person"][65].integer(0) == 1, "23 点算夜间通行");
    bool down = false;
    for (size_t i = 0; i < fj["flow"]["person"].size(); ++i) {
        const mb::Json& row = fj["flow"]["person"][i];
        if (row[0].integer(-1) == 65) down = row[1 + 2].integer(0) == 1;  // 方向 2 = 向下
    }
    check(down, "方向按格子走向计 (向下 = 2), 只写有数据的格子");
    fm.set_wall_time(local_time(0, 12, 0));
    fm.update({person(8, 150, 150)}, W, H, ft += 40, nullptr);
    check(fm.json()["night"]["person"][11].integer(-1) == 0 && fm.json()["person"][11].integer(0) == 1,
          "白天不计入夜间");
    {
        // 走到树后 (忽略区) 待 15 秒再出来: 不在忽略区边上记消失 / 出现
        mb::ActivityMap im;
        im.configure(fp, cls);
        Run tree(config(R"JSON({"regions": {"tree": {"role": "ignore",
              "points": [[0.0, 0.8], [0.3, 0.8], [0.3, 1.0], [0.0, 1.0]]}}})JSON"));
        uint64_t it = 1000;
        im.update({person(9, 150, 650)}, W, H, it += 40, &tree.rules);
        im.update({person(9, 150, 750)}, W, H, it += 40, &tree.rules);
        for (int i = 0; i < 400; ++i) im.update({person(9, 150, 900)}, W, H, it += 40, &tree.rules);
        im.update({person(9, 150, 750)}, W, H, it += 40, &tree.rules);
        int births = 0, deaths = 0;
        for (int c = 0; c < 100; ++c) {
            births += im.json()["births"]["person"][(size_t)c].integer(0);
            deaths += im.json()["deaths"]["person"][(size_t)c].integer(0);
        }
        check(births == 1 && deaths == 0 && im.json()["tracks"]["person"].integer(0) == 1,
              "在忽略区里待着不算消失, 出来也不算新出现");
    }

    char tmpl2[] = "/tmp/mb-activity-XXXXXX";
    const int fd2 = mkstemp(tmpl2);
    if (fd2 >= 0) close(fd2);
    fp.path = tmpl2;
    fm.configure(fp, cls);
    check(fm.maybe_save(ft, true, &err), "version 2 写盘");
    mb::ActivityMap fb;
    fb.configure(fp, cls);
    check(fb.load(&err) && fb.json()["deaths"]["person"][85].integer(0) == 1 &&
              fb.json()["flow"]["person"].size() == fm.json()["flow"]["person"].size(),
          "version 2 读回 (出现消失 / 方向)");
    {
        // 旧版文件只有通行量: 照常读, 新统计从零开始
        FILE* f = fopen(tmpl2, "w");
        std::string v1 = R"({"version": 1, "cols": 10, "rows": 10, "tracks": {"person": 3, "vehicle": 0},
                             "person": [)";
        for (int i = 0; i < 100; ++i) v1 += i ? (i == 42 ? ",5" : ",0") : "0";
        v1 += R"(], "vehicle": [)";
        for (int i = 0; i < 100; ++i) v1 += i ? ",0" : "0";
        v1 += "]}";
        if (f) {
            fputs(v1.c_str(), f);
            fclose(f);
        }
        mb::ActivityMap old;
        old.configure(fp, cls);
        check(old.load(&err) && old.json()["person"][42].integer(0) == 5 &&
                  old.json()["births"]["person"][42].integer(-1) == 0,
              "version 1 文件照常读回");
    }
    unlink(tmpl2);
}

// 合成场景: 一堆不同灰度的矩形 (画面外也有, 摄像头转过去能看到新东西)。
// 坐标是 320x180 分析分辨率的像素。
struct Scene {
    struct Box {
        float x1, y1, x2, y2;
        uint8_t v;
    };
    std::vector<Box> boxes;
    Scene() {
        uint32_t s = 12345;
        auto rnd = [&s]() {
            s = s * 1103515245u + 12345u;
            return (float)((s >> 8) & 0xFFFF) / 65535.0f;
        };
        for (int i = 0; i < 260; ++i) {
            const float x = -160 + rnd() * 640, y = -90 + rnd() * 360;
            const float w = 8 + rnd() * 40, h = 8 + rnd() * 40;
            boxes.push_back({x, y, x + w, y + h, (uint8_t)(20 + rnd() * 215)});
        }
    }
    uint8_t at(float x, float y) const {
        uint8_t v = (uint8_t)(100 + 40 * std::sin(x * 0.01f) * std::cos(y * 0.013f));
        for (const Box& b : boxes)
            if (x >= b.x1 && x < b.x2 && y >= b.y1 && y < b.y2) v = b.v;
        return v;
    }
    // 摄像头动了: 当前画面像素 q = R*s*p + t (p 是参考画面像素)
    mb::GrayImage render(float deg = 0, float scale = 1, float tx = 0, float ty = 0,
                         float gain = 1, float bias = 0) const {
        mb::GrayImage g;
        g.width = 320;
        g.height = 180;
        g.data.resize(320 * 180);
        const float a = std::cos(deg * 3.14159265f / 180) * scale;
        const float b = std::sin(deg * 3.14159265f / 180) * scale;
        const float s2 = a * a + b * b;
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                const float qx = x - tx, qy = y - ty;
                const float px = (a * qx + b * qy) / s2, py = (-b * qx + a * qy) / s2;
                const float v = at(px, py) * gain + bias;
                g.data[y * 320 + x] = (uint8_t)std::max(0.0f, std::min(255.0f, v));
            }
        return g;
    }
};

mb::Affine2 expected_affine(float deg, float scale, float tx, float ty) {
    const float a = std::cos(deg * 3.14159265f / 180) * scale;
    const float b = std::sin(deg * 3.14159265f / 180) * scale;
    mb::Affine2 t;
    t.m[0] = a;
    t.m[1] = -b * 180 / 320;
    t.m[2] = tx / 320;
    t.m[3] = b * 320 / 180;
    t.m[4] = a;
    t.m[5] = ty / 180;
    return t;
}

void test_view_guard() {
    printf("20. 摄像头偏移检测 / 区域跟随\n");
    const float aspect = 320.0f / 180.0f;
    const Scene scene;
    const mb::GrayImage ref = scene.render();

    mb::AlignResult same = mb::align_views(ref, scene.render(0, 1, 0, 0, 0.9f, 8), 0.25f, {}, {});
    check(same.confident && mb::transform_distance(same.transform, mb::Affine2(), aspect) < 0.003f,
          "没动: 亮度变了也对得上, 偏移约为 0");

    const mb::GrayImage moved = scene.render(2.0f, 1.0f, 16, -9, 0.6f, 10);
    const mb::Affine2 want = expected_affine(2.0f, 1.0f, 16, -9);
    mb::AlignResult a = mb::align_views(ref, moved, 0.25f, {}, {});
    check(a.confident && mb::transform_distance(a.transform, want, aspect) < 0.006f,
          "平移 5% + 旋转 2° + 变暗: 估计出正确的偏移");
    const mb::ViewShift sh = mb::describe_shift(a.transform, aspect);
    check(std::fabs(sh.rotation_deg - 2.0f) < 0.5f && std::fabs(sh.scale - 1.0f) < 0.02f,
          "旋转角和缩放换算正确");

    mb::GrayImage blocked = moved;
    for (int y = 60; y < 150; ++y)
        for (int x = 120; x < 190; ++x) blocked.data[y * 320 + x] = 250;
    mb::AlignResult b = mb::align_views(ref, blocked, 0.25f, {},
                                        {mb::NormBox{120 / 320.f, 60 / 180.f, 190 / 320.f, 150 / 180.f}});
    check(b.confident && mb::transform_distance(b.transform, want, aspect) < 0.006f,
          "画面里有人车遮挡一块: 仍按背景对齐");

    mb::GrayImage flat = ref;
    std::fill(flat.data.begin(), flat.data.end(), 90);
    check(!mb::align_views(ref, flat, 0.25f, {}, {}).confident, "画面被挡住/没有纹理: 不可靠, 不乱估计");

    char dir[] = "/tmp/mb-view-XXXXXX";
    if (!mkdtemp(dir)) {
        check(false, "建临时目录");
        return;
    }
    mb::ViewGuard::Params p;
    p.dir = dir;
    p.interval_sec = 2;
    p.confirm_sec = 4;
    p.settle_sec = 0;
    p.alert_sec = 6;
    auto make = [&](mb::ViewGuard& g, const mb::ViewGuard::Params& pp) {
        g.set_synchronous(true);
        g.set_wall_time(1700000000);
        g.configure(pp);
    };
    uint64_t now = 1000;
    {
        mb::ViewGuard g;
        make(g, p);
        check(g.wants_frame(now), "没有参考画面时马上要一帧");
        g.offer(ref, {}, now);
        check(g.json()["state"].str("") == "ok" && g.json()["references"].integer(0) == 1,
              "首帧存为参考画面");
        // 一次偏移不算数 (可能是有人在镜头前晃)
        g.offer(moved, {}, now += 2000);
        mb::ViewGuard::Change ch;
        check(!g.take_change(&ch) && g.json()["state"].str("") == "checking", "偏移要持续确认");
        g.offer(ref, {}, now += 2000);
        check(g.json()["state"].str("") == "ok", "又对上了: 当作误报, 不跟随");
        for (int i = 0; i < 3; ++i) g.offer(moved, {}, now += 2000);
        check(g.take_change(&ch) && ch.warp_heat &&
                  mb::transform_distance(ch.applied, want, aspect) < 0.006f,
              "持续偏移: 自动跟随, 热图一起搬");
        mb::Json j = g.json();
        check(j["notice"].str("") == "corrected" && !j["acked"].boolean(true) &&
                  j["corrected"].boolean(false),
              "提示「已自动校正」");
        std::string err;
        check(g.command("ack", &err) && g.json()["acked"].boolean(false), "ack 隐藏提示");
        g.offer(moved, {}, now += 2000);
        check(!g.take_change(&ch) && g.json()["state"].str("") == "corrected", "校正后保持稳定");

        mb::ViewGuard back;
        make(back, p);
        check(back.load(&err) && mb::transform_distance(back.applied(), want, aspect) < 0.006f,
              "重启后读回偏移和参考画面");

        check(g.command("undo", &err) && g.take_change(&ch) && !ch.warp_heat &&
                  ch.applied.is_identity(),
              "撤销: 区域按配置原样使用");
        g.offer(moved, {}, now += 2000);
        check(g.json()["state"].str("") == "ok" && g.applied().is_identity(),
              "撤销后以当前画面为新参考");
        g.offer(scene.render(0, 1, 0, 0, 0.03f, 0), {}, now += 2000);
        check(g.json()["state"].str("") == "dark", "太暗不判断");
    }
    {
        mb::ViewGuard::Params q = p;
        q.max_rotation_deg = 1.0f;
        mb::ViewGuard g;
        make(g, q);
        g.offer(ref, {}, now += 2000);
        for (int i = 0; i < 3; ++i) g.offer(moved, {}, now += 2000);
        mb::ViewGuard::Change ch;
        mb::Json j = g.json();
        check(!g.take_change(&ch) && g.applied().is_identity() && j["state"].str("") == "moved" &&
                  j["notice"].str("") == "moved",
              "超出自动范围: 区域不动, 提示确认");
    }
    {
        mb::ViewGuard g;
        make(g, p);
        g.offer(ref, {}, now += 2000);
        g.offer(flat, {}, now += 2000);
        check(g.json()["state"].str("") == "uncertain" && g.json()["notice"].str("x").empty(),
              "短时对不上不提示");
        for (int i = 0; i < 3; ++i) g.offer(flat, {}, now += 2000);
        check(g.json()["notice"].str("") == "unmatched", "持续对不上才提示");
        std::string err;
        check(g.command("accept", &err), "accept 命令");
        g.offer(flat, {}, now += 2000);
        check(g.json()["notice"].str("x").empty() && g.json()["references"].integer(0) == 2,
              "确认画面正常: 记为补充参考, 提示消失");
    }
    std::string cmd = std::string("rm -rf ") + dir;
    if (system(cmd.c_str()) != 0) printf("  (清理 %s 失败)\n", dir);

    // 规则引擎跟随
    Run r;
    mb::Affine2 t;
    t.m[2] = 0.1f;
    r.rules.set_view_transform(t);
    check(std::fabs(r.rules.regions()[0].points[0].x - 0.3f) < 1e-5f, "区域按偏移平移");
    r.step({person(1, 850, 500)}, 20);
    check(count(r.all, "intrusion", "enter") == 1, "原区域外、平移后区域内的人触发入侵");
    Run fresh;
    check(r.rules.same_geometry(fresh.rules), "对齐不改变配置几何");
    Run other(config(R"JSON({"features": {}, "regions": {"zone-1": {"name": "A",
          "points": [[0.25, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]]}}})JSON"));
    check(!other.rules.same_geometry(fresh.rules), "重画区域可以识别出来");

    // 热图跟随
    mb::ActivityMap map;
    mb::ActivityMap::Params ap;
    ap.cols = 10;
    ap.rows = 10;
    ap.path.clear();
    map.configure(ap, classes());
    map.update({person(1, 250, 250)}, W, H, 1000, nullptr);
    map.warp(t);
    check(map.json()["person"][23].integer(-1) == 1 && map.json()["person"][22].integer(-1) == 0,
          "热图按偏移搬格子");
}

void test_line_count() {
    printf("21. 人数统计 (越线计数)\n");
    // 线 y=0.3, p1 在左、p2 在右: 脚点在线上方 (y<300) 时 side<0, 下方 side>0。
    const char* text = R"JSON({
      "features": {
        "count": {"enabled": true, "rule": "line", "classes": ["person"],
                  "line_ids": ["gate"], "line_margin": 0.03}
      },
      "lines": {
        "gate": {"name": "门口", "p1": [0.1, 0.3], "p2": [0.9, 0.3]}
      }
    })JSON";
    auto gate = [](const mb::RuleEngine& e) {
        const mb::RuleEngine::LineCounts counts = e.line_counts();
        auto it = counts.find("gate");
        return it == counts.end() ? mb::RuleEngine::LineCount{} : it->second;
    };

    {
        Run r(config(text));
        check(gate(r.rules).in == 0 && gate(r.rules).out == 0, "未过线时计数为 0");
        r.step({person(1, 500, 250)}, 5);
        check(gate(r.rules).total() == 0, "第一次看到线只记方向, 不计数");
        r.step({person(1, 500, 350)}, 5);
        check(gate(r.rules).in == 1 && gate(r.rules).out == 0, "p1→p2 左侧跨到右侧记 in");
        r.step({person(1, 500, 250)}, 5);
        check(gate(r.rules).in == 1 && gate(r.rules).out == 1, "反向跨回记 out");
        check(count(r.all, "count", "cross") == 2, "每次穿越各出一条 cross 事件");
    }

    {
        // 线附近死区: ±20px 在线上抖, 不计数; 出了死区才按原方向判翻转
        Run r(config(text));
        r.step({person(1, 500, 350)}, 5);
        for (int i = 0; i < 40; ++i) r.step({person(1, 500, i % 2 ? 320.0f : 280.0f)});
        check(gate(r.rules).total() == 0, "死区内来回抖动不计数");
        r.step({person(1, 500, 250)}, 5);
        check(gate(r.rules).in == 0 && gate(r.rules).out == 1, "死区内抖动不改变已记方向");
    }

    {
        // 走到线前又退回去: 没有符号翻转, 不算穿越
        Run r(config(text));
        r.step({person(1, 500, 400)}, 5);
        r.step({person(1, 500, 310)}, 5);
        r.step({person(1, 500, 400)}, 5);
        check(gate(r.rules).total() == 0, "靠近线后又退回, 不计数");
    }

    {
        Run r(config(text));
        r.step({person(1, 500, 350)}, 10);
        check(gate(r.rules).total() == 0, "第一次出现已经在线上方一侧, 不算穿越");
    }

    {
        // ID 切换: 人越过线时丢了 120ms, 新 ID 在另一侧出现, 接续后补计一次
        Run r(config(text));
        r.step({person(1, 500, 250)}, 5);
        r.step({}, 3);
        r.step({person(7, 500, 355)}, 5);
        check(gate(r.rules).in == 1 && gate(r.rules).out == 0,
              "ID 切换: 丢失期间越过的线补计一次, 不多计");
        r.step({person(7, 500, 650)}, 10);
        check(gate(r.rules).total() == 1, "接续后继续走不重复计数");
        check(count(r.all, "count", "cross") == 1, "接续不重复产生 cross 事件");
    }

    {
        // 热加载: 线没动 -> 计数保留; 线改了几何 -> 计数清零
        Run r(config(text));
        r.step({person(1, 500, 250)}, 5);
        r.step({person(1, 500, 350)}, 5);
        mb::RuleEngine same;
        std::string err;
        check(same.load(config(text), classes(), &err), "重新加载同一份配置");
        same.adopt_state(r.rules);
        check(gate(same).in == 1 && gate(same).out == 0, "线没动: 热加载后计数保留");

        std::string moved = text;
        moved.replace(moved.find("[0.9, 0.3]"), 10, "[0.9, 0.35]");
        mb::RuleEngine other;
        check(other.load(config(moved.c_str()), classes(), &err), "加载线改过的配置");
        other.adopt_state(r.rules);
        check(gate(other).total() == 0, "线改了位置: 计数清零, 不再沿用旧值");
    }
}

void test_count_store() {
    printf("22. 越线计数按小时落盘\n");
    char tmpl[] = "/tmp/mb-counts-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd < 0) {
        check(false, "创建临时文件");
        return;
    }
    close(fd);
    ::remove(tmpl);  // CountStore 自己创建

    mb::CountStore::Params p;
    p.log_path = tmpl;
    mb::CountStore store;
    store.configure(p);

    // 固定的墙钟时间: 整点后 10 分钟, 避免跨小时边界
    const time_t t1 = ((time_t)1700000000 / 3600) * 3600 + 600;
    store.set_wall_time(t1);

    mb::RuleEngine::LineCounts counts;
    counts["gate"].in = 2;
    counts["gate"].out = 1;
    store.poll(counts);
    check(store.empty(), "第一次 poll 只记基线, 不产生增量");

    counts["gate"].in = 5;  // +3
    counts["gate"].out = 1;
    store.poll(counts);
    check(!store.empty(), "计数增加产生待写增量");

    std::string err;
    check(store.maybe_flush(1000, true, &err), "写 counts.jsonl");

    {
        FILE* f = fopen(tmpl, "r");
        char buf[4096];
        int lines = 0;
        bool parsed = true;
        bool values = false;
        while (f && fgets(buf, sizeof(buf), f)) {
            mb::Json j;
            std::string verr;
            parsed = parsed && mb::Json::parse(buf, &j, &verr);
            if (parsed && lines == 0)
                values = j["lines"]["gate"]["in"].integer(0) == 3 &&
                         j["lines"]["gate"]["out"].integer(0) == 0 &&
                         j["hour"].str("").size() == 19;
            ++lines;
        }
        if (f) fclose(f);
        check(lines == 1 && parsed, "JSONL: 一轮只写一行");
        check(values, "JSONL: 只记增量 (in=3), 不重复记基线");
    }

    // 清零: 计数变小时只重设基线, 不写负值
    counts["gate"].in = 0;
    counts["gate"].out = 0;
    store.poll(counts);
    check(store.maybe_flush(2000, true, &err), "清零后 flush");
    {
        FILE* f = fopen(tmpl, "r");
        char buf[4096];
        int lines = 0;
        while (f && fgets(buf, sizeof(buf), f)) ++lines;
        if (f) fclose(f);
        check(lines == 1, "清零不写负增量");
    }
    counts["gate"].in = 1;
    store.poll(counts);
    check(store.maybe_flush(3000, true, &err), "清零后重新计数");
    {
        FILE* f = fopen(tmpl, "r");
        char buf[4096];
        mb::Json last;
        bool ok = true;
        while (f && fgets(buf, sizeof(buf), f)) {
            std::string verr;
            ok = ok && mb::Json::parse(buf, &last, &verr);
        }
        if (f) fclose(f);
        check(ok && last["lines"]["gate"]["in"].integer(0) == 1, "清零后从新基线重新累计");
    }

    // 跨小时: 增量按小时分桶, 各写各的行
    store.set_wall_time(t1 + 3600);
    counts["gate"].in = 4;
    store.poll(counts);
    const auto buckets = store.pending();
    check(buckets.size() == 1, "跨小时只攒新小时的增量");
    check(store.maybe_flush(4000, true, &err), "跨小时后 flush");
    {
        FILE* f = fopen(tmpl, "r");
        char buf[4096];
        int lines = 0;
        std::vector<std::string> hours;
        while (f && fgets(buf, sizeof(buf), f)) {
            mb::Json j;
            std::string verr;
            if (mb::Json::parse(buf, &j, &verr)) hours.push_back(j["hour"].str(""));
            ++lines;
        }
        if (f) fclose(f);
        check(lines == 3 && hours.size() == 3 && hours[1] != hours[2],
              "新一小时单独一行, hour 不同");
    }

    ::remove(tmpl);
}

void test_people_in_frame() {
    printf("23. 画面内人数 (OSD 的 NOW)\n");
    const char* text = R"JSON({
      "features": {
        "count": {"enabled": true, "rule": "line", "classes": ["person"],
                  "line_ids": ["gate"], "line_margin": 0.03}
      },
      "lines": {
        "gate": {"name": "门口", "p1": [0.1, 0.3], "p2": [0.9, 0.3]}
      }
    })JSON";
    auto cfg = config(text);

    {
        Run r(cfg);
        check(r.rules.people_in_frame() == 0, "还没 update 过时为 0");
        r.step({person(1, 300, 500), person(2, 700, 500)});
        check(r.rules.people_in_frame() == 2, "两个已确认的人, 画面内人数为 2");

        mb::Track lost = person(2, 700, 500);
        lost.lost = 3;  // 暂时看不到, 不算在画面里
        r.step({person(1, 300, 500), lost});
        check(r.rules.people_in_frame() == 1, "暂时丢失的人不计入");

        mb::Track unconfirmed = person(3, 500, 600);
        unconfirmed.confirmed = false;
        r.step({person(1, 300, 500), unconfirmed});
        check(r.rules.people_in_frame() == 1, "未确认的轨迹不计入");

        mb::Track car = person(4, 600, 600);
        car.cls_id = 1;  // 类别不在 count 的 classes 里
        r.step({person(1, 300, 500), car});
        check(r.rules.people_in_frame() == 1, "不属于统计类别的不计入");

        // 人站在线两边都算 (不是看越没越线)
        r.step({person(1, 300, 200), person(5, 500, 800)});
        check(r.rules.people_in_frame() == 2, "人站在线的哪一侧都算在画面内");

        r.step({});
        check(r.rules.people_in_frame() == 0, "人走光后回到 0");
    }

    {
        // 没开越线计数时按 person 类别兜底
        Run r(config());
        mb::Track car = person(4, 600, 600);
        car.cls_id = 1;
        r.step({person(1, 300, 500), car});
        check(r.rules.people_in_frame() == 1, "没开 count 时按 person 类别统计");
    }
}

void test_count_reset() {
    printf("24. 越线计数按小时自动清零\n");
    auto mkcfg = [](int hours) {
        const std::string text =
            std::string(R"JSON({
      "features": {
        "count": {"enabled": true, "rule": "line", "classes": ["person"],
                  "line_ids": ["gate"], "line_margin": 0.03, "reset_hours": )JSON") +
            std::to_string(hours) + R"JSON(}
      },
      "lines": {
        "gate": {"name": "门口", "p1": [0.1, 0.3], "p2": [0.9, 0.3]}
      }
    })JSON";
        return text;
    };
    auto gate_in = [](const mb::RuleEngine& e) {
        const mb::RuleEngine::LineCounts counts = e.line_counts();
        auto it = counts.find("gate");
        return it == counts.end() ? 0 : it->second.in + it->second.out;
    };

    {
        const std::string text = mkcfg(1);
        Run r(config(text.c_str()));
        r.rules.set_wall_time(local_time(0, 10, 30));
        r.step({person(1, 500, 250)}, 5);
        r.step({person(1, 500, 350)}, 5);
        check(gate_in(r.rules) == 1, "10:30 过线, 计数 1");

        r.rules.set_wall_time(local_time(0, 10, 59));
        r.step({person(1, 500, 350)}, 3);
        check(gate_in(r.rules) == 1, "同一小时内不清零");

        r.rules.set_wall_time(local_time(0, 11, 0));
        r.step({person(1, 500, 350)}, 3);
        check(gate_in(r.rules) == 0, "跨到 11:00 自动清零");

        r.step({person(1, 500, 250)}, 5);
        check(gate_in(r.rules) == 1, "清零后重新累计");

        r.rules.set_wall_time(local_time(0, 11, 30));
        r.step({person(1, 500, 250)}, 3);
        check(gate_in(r.rules) == 1, "下一个整点前不再清");

        // 热加载 (周期没变) 不把当前这一格当成新周期, 计数保留
        mb::RuleEngine same;
        std::string err;
        check(same.load(config(text.c_str()), classes(), &err), "重新加载带清零周期的配置");
        same.adopt_state(r.rules);
        check(gate_in(same) == 1, "热加载周期没变: 计数保留");
    }

    {
        // 2 小时一个周期: 从本地日 0 点对齐, 10:30 落在第 5 格, 12:00 换第 6 格
        const std::string text = mkcfg(2);
        Run r(config(text.c_str()));
        r.rules.set_wall_time(local_time(0, 10, 30));
        r.step({person(1, 500, 250)}, 5);
        r.step({person(1, 500, 350)}, 5);
        r.rules.set_wall_time(local_time(0, 11, 59));
        r.step({person(1, 500, 350)}, 3);
        check(gate_in(r.rules) == 1, "2 小时周期: 11:59 还没到换格");
        r.rules.set_wall_time(local_time(0, 12, 0));
        r.step({person(1, 500, 350)}, 3);
        check(gate_in(r.rules) == 0, "2 小时周期: 12:00 清零");
    }

    {
        Run r(config(mkcfg(0).c_str()));
        r.rules.set_wall_time(local_time(0, 10, 30));
        r.step({person(1, 500, 250)}, 5);
        r.step({person(1, 500, 350)}, 5);
        r.rules.set_wall_time(local_time(0, 13, 0));
        r.step({person(1, 500, 350)}, 3);
        check(gate_in(r.rules) == 1, "reset_hours = 0: 跨小时也不清零");
    }
}

// 规则挂在区域 / 线上: 同一功能多个区域, 各自覆盖参数
const char* kMultiConfig = R"JSON({
  "features": {
    "intrusion": {"enabled": true, "rule": "zone", "trigger": "enter", "classes": ["person"],
                  "sustain_sec": 0.5,
                  "actions": {"speaker": {"enabled": true, "repeat_sec": 0}}},
    "dwell": {"enabled": true, "rule": "zone", "trigger": "stay", "scope": "track",
              "classes": ["person"], "dwell_sec": 60},
    "count": {"enabled": true, "rule": "line", "classes": ["person"], "line_margin": 0.03}
  },
  "regions": {
    "door": {"name": "门口", "points": [[0.0, 0.0], [0.3, 0.0], [0.3, 1.0], [0.0, 1.0]],
             "rules": {"intrusion": {"sustain_sec": 2.0,
                                     "actions": {"speaker": {"enabled": false}}},
                       "dwell": {"dwell_sec": 1}}},
    "yard": {"name": "院子", "points": [[0.35, 0.0], [0.65, 0.0], [0.65, 1.0], [0.35, 1.0]],
             "rules": {"intrusion": {}, "dwell": {}}},
    "garage": {"name": "车库", "points": [[0.7, 0.0], [1.0, 0.0], [1.0, 1.0], [0.7, 1.0]],
               "rules": {"intrusion": {"enabled": false}}}
  },
  "lines": {
    "gate-a": {"name": "前门", "p1": [0.0, 0.3], "p2": [0.3, 0.3], "rules": {"count": {}}},
    "gate-b": {"name": "后门", "p1": [0.7, 0.7], "p2": [1.0, 0.7], "rules": {"count": {}}}
  }
})JSON";

int count_at(const std::vector<mb::RuleEvent>& evs, const char* feature, const char* kind,
             const char* rule_id) {
    int n = 0;
    for (const auto& e : evs) n += e.feature == feature && e.kind == kind && e.rule_id == rule_id;
    return n;
}

void test_multi_region() {
    printf("25. 多区域 / 多进出线 (规则挂在区域上, 参数各自覆盖)\n");
    {
        Run r(config(kMultiConfig));
        int intrusion = 0;
        for (const auto& f : r.rules.features()) intrusion += f.key == "intrusion";
        check(intrusion == 2, "intrusion 展开成 2 个实例 (车库 enabled=false 不算)");
        const mb::NamedPolygon* door = nullptr;
        const mb::NamedPolygon* garage = nullptr;
        for (const auto& g : r.rules.regions()) {
            if (g.id == "door") door = &g;
            if (g.id == "garage") garage = &g;
        }
        check(door && r.rules.region_active(*door) && garage && !r.rules.region_active(*garage),
              "被启用规则挂着的区域才在 OSD 显示");

        r.step({person(1, 150, 500), person(2, 500, 500), person(3, 850, 500)}, 15);  // 0.6s
        check(count_at(r.all, "intrusion", "enter", "yard") == 1, "院子按默认 0.5s 报入侵");
        check(count_at(r.all, "intrusion", "enter", "door") == 0, "门口覆盖为 2s, 0.6s 时不报");
        check(count(r.all, "intrusion", "enter") == 1, "车库停用, 不报");
        r.step({person(1, 150, 500), person(2, 500, 500), person(3, 850, 500)}, 40);  // 2.2s
        check(count_at(r.all, "intrusion", "enter", "door") == 1, "门口满 2s 报入侵");
        check(count_at(r.all, "dwell", "stay", "door") == 1, "门口滞留覆盖为 1s");
        check(count_at(r.all, "dwell", "stay", "yard") == 0, "院子滞留仍是默认 60s");
        check(r.rules.dwellers().size() == 2, "两个区域各自计时");

        mb::RuleEvent door_ev;
        for (const auto& e : r.all)
            if (e.feature == "intrusion" && e.rule_id == "door") door_ev = e;
        check(r.rules.liveness(door_ev) == mb::RuleEngine::Liveness::Active, "门口告警 Active");

        // 热加载: 去掉门口的入侵, 院子不动
        std::string next = kMultiConfig;
        const std::string from = R"("rules": {"intrusion": {"sustain_sec": 2.0,
                                     "actions": {"speaker": {"enabled": false}}},
                       "dwell")";
        next.replace(next.find(from), from.size(), R"("rules": {"dwell")");
        mb::RuleEngine reloaded;
        std::string err;
        check(reloaded.load(config(next.c_str()), classes(), &err), "热加载配置有效");
        reloaded.adopt_state(r.rules);
        check(reloaded.liveness(door_ev) == mb::RuleEngine::Liveness::Gone,
              "门口去掉入侵后, 挂起的门口告警作废");
        mb::RuleEvent yard_ev = door_ev;
        yard_ev.rule_id = "yard";
        yard_ev.track_id = 2;
        check(reloaded.liveness(yard_ev) == mb::RuleEngine::Liveness::Active,
              "院子的入侵状态被热加载保留");
    }
    {
        Run r(config(kMultiConfig));
        r.step({person(1, 150, 200)}, 3);
        r.step({person(1, 150, 400)}, 3);  // 穿过前门 (y=0.3)
        r.step({person(2, 850, 800)}, 3);
        r.step({person(2, 850, 600)}, 3);  // 反向穿过后门 (y=0.7)
        const auto lc = r.rules.line_counts();
        check(lc.at("gate-a").total() == 1 && lc.at("gate-b").total() == 1 &&
                  lc.at("gate-a").in != lc.at("gate-b").in,
              "两条进出线各自计数, 方向各自判断");
    }
    {
        // 旧写法 region_id + 新写法 rules 取并集, 同一目标不重复
        Run r(config(R"JSON({
          "features": {"intrusion": {"enabled": true, "rule": "zone", "classes": ["person"],
                                     "region_id": "a", "region_ids": ["a", "b"], "sustain_sec": 0.2}},
          "regions": {
            "a": {"points": [[0, 0], [0.5, 0], [0.5, 1], [0, 1]], "rules": {"intrusion": {}}},
            "b": {"points": [[0.5, 0], [1, 0], [1, 1], [0.5, 1]]}
          }})JSON"));
        r.step({person(1, 250, 500), person(2, 750, 500)}, 10);
        check(count_at(r.all, "intrusion", "enter", "a") == 1 &&
                  count_at(r.all, "intrusion", "enter", "b") == 1,
              "旧 region_id / region_ids 与 rules 取并集, 不重复报");
    }
    {
        // 每个区域自己的布防时段
        Run r(config(R"JSON({
          "features": {"fence": {"enabled": true, "rule": "zone", "trigger": "cross",
                                 "classes": ["person"], "sustain_sec": 0.2, "arm": {"mode": "always"}}},
          "regions": {
            "front": {"points": [[0, 0], [0.5, 0], [0.5, 1], [0, 1]], "rules": {"fence": {}}},
            "back": {"points": [[0.5, 0], [1, 0], [1, 1], [0.5, 1]],
                     "rules": {"fence": {"arm": {"mode": "schedule",
                               "schedule": [{"start": "22:00", "end": "06:00"}]}}}}
          }})JSON"));
        r.rules.set_wall_time(local_time(0, 12, 0));
        r.step({person(1, 250, 500), person(2, 750, 500)}, 10);
        check(r.rules.armed("fence", "front") && !r.rules.armed("fence", "back") &&
                  r.rules.armed("fence"),
              "前院一直布防, 后院白天撤防; 按功能看 = 任一处布防");
        check(count_at(r.all, "fence", "enter", "front") == 1 &&
                  count_at(r.all, "fence", "enter", "back") == 0,
              "撤防的区域不报");
    }
    {
        // 配置错误要在加载时报出来
        auto bad = [](const char* text) {
            mb::RuleEngine e;
            std::string err;
            return !e.load(config(text), classes(), &err) && !err.empty();
        };
        check(bad(R"({"features": {}, "regions": {"a": {"points": [[0,0],[1,0],[1,1]],
                     "rules": {"nope": {}}}}})"),
              "rules 引用不存在的功能");
        check(bad(R"({"features": {"count": {"enabled": true, "rule": "line"}},
                     "regions": {"a": {"points": [[0,0],[1,0],[1,1]], "rules": {"count": {}}}}})"),
              "计数不能挂在区域上");
        check(bad(R"({"features": {"intrusion": {"enabled": true, "rule": "zone"}},
                     "lines": {"l": {"p1": [0,0], "p2": [1,1], "rules": {"intrusion": {}}}}})"),
              "入侵不能挂在线上");
        check(bad(R"({"features": {"intrusion": {"enabled": true, "rule": "zone"}},
                     "regions": {"a": {"role": "ignore", "points": [[0,0],[1,0],[1,1]],
                                       "rules": {"intrusion": {}}}}})"),
              "启用的功能不能挂在忽略区上");
        check(bad(R"({"features": {"locate": {"enabled": true, "rule": "homography"}},
                     "regions": {
                       "g1": {"role": "ground", "points": [[0,1],[1,1],[1,0],[0,0]],
                              "real_size_m": [4, 8], "rules": {"locate": {}}},
                       "g2": {"role": "ground", "points": [[0,1],[1,1],[1,0],[0,0]],
                              "real_size_m": [4, 8], "rules": {"locate": {}}}}})"),
              "定位只能挂一个地面标定区");

        mb::Json crowded = config(R"JSON({"features": {}, "regions": {}})JSON");
        mb::Json regions = mb::Json::object();
        for (int i = 0; i < mb::kMaxRegions + 1; ++i) {
            mb::Json region = mb::Json::object();
            mb::Json points = mb::Json::array();
            for (const auto& p : {mb::Point{0, 0}, mb::Point{1, 0}, mb::Point{1, 1}}) {
                mb::Json point = mb::Json::array();
                point.push(mb::Json::number_value(p.x));
                point.push(mb::Json::number_value(p.y));
                points.push(point);
            }
            region.set("points", points);
            regions.set("r" + std::to_string(i), region);
        }
        crowded.set("regions", regions);
        mb::RuleEngine too_many;
        std::string too_many_err;
        check(!too_many.load(crowded, classes(), &too_many_err) &&
                  too_many_err.find("12") != std::string::npos,
              "区域数量超过 12 个时拒绝加载");
    }
    {
        // 联动 (喇叭) 也按区域覆盖
        AlarmRun g(kMultiConfig);
        g.step({person(2, 500, 500)}, 20);
        check(g.speaks == 1, "院子沿用功能默认: 喊话");
        g.step({person(2, 500, 500), person(1, 150, 500)}, 60);
        check(count_at(g.r.all, "intrusion", "enter", "door") == 1 && g.speaks == 1,
              "门口关了喇叭: 报警但不喊");
        const auto dc = mb::DwellGuardConfig::from_json(config(kMultiConfig));
        check(dc.find("dwell", "door") != nullptr && dc.find("dwell", "nowhere") != nullptr,
              "滞留联动按区域查, 找不到回落到功能默认");
    }
    {
        // 旧配置功能没写 actions (不续录): 某个区域只关喇叭, 不能顺带打开续录
        const auto ac = mb::ZoneAlarmConfig::from_json(config(R"JSON({
          "features": {"intrusion": {"enabled": true, "rule": "zone", "trigger": "enter"}},
          "regions": {
            "a": {"points": [[0, 0], [0.5, 0], [0.5, 1]], "rules": {"intrusion": {}}},
            "b": {"points": [[0.5, 0], [1, 0], [1, 1]],
                  "rules": {"intrusion": {"actions": {"speaker": {"enabled": false}}}}}
          }})JSON"));
        const mb::ZoneAlarmActions* a = ac.find("intrusion", "a");
        const mb::ZoneAlarmActions* b = ac.find("intrusion", "b");
        check(a && b && a->speaker && !a->record && !b->speaker && !b->record,
              "只覆盖喇叭开关不改变续录默认值");
    }
}

// ---------------------------------------------------------------------------
// 26-28. 二维视觉定位 / 设备经纬度
// ---------------------------------------------------------------------------

// 合成"真实相机": 地面 (米) -> 归一化画面, 近处在画面下方, 越远越小
mb::Point synth_image(mb::Point g) {
    const float w = 0.15f * g.y + 1.0f;
    return mb::Point{(0.08f * g.x + 0.2f) / w, (0.9f - 0.02f * g.y) / w};
}

bool has_warning(const mb::CalibQuality& q, const char* needle) {
    for (const std::string& w : q.warnings)
        if (w.find(needle) != std::string::npos) return true;
    return false;
}

void test_ground_calib() {
    printf("26. 地面标定\n");
    std::vector<mb::GroundPair> calib, checks;
    for (float x : {0.0f, 3.0f, 6.0f})
        for (float y : {0.0f, 10.0f}) calib.push_back({synth_image({x, y}), {x, y}});
    checks.push_back({synth_image({1.5f, 4.0f}), {1.5f, 4.0f}});
    checks.push_back({synth_image({4.5f, 7.0f}), {4.5f, 7.0f}});
    mb::GroundCalib gc;
    std::string err;
    check(gc.fit(calib, checks, &err), "6 个标定点 + 2 个检查点拟合成功");
    check(gc.quality().rmse_m < 0.01f && gc.quality().has_check &&
              gc.quality().check_rmse_m < 0.01f,
          "无噪声时拟合残差和检查点误差都接近 0");
    check(gc.quality().grade == "good", "精度好且覆盖够 -> good");
    mb::Point g;
    check(gc.to_ground(synth_image({2.2f, 8.8f}), &g) && std::fabs(g.x - 2.2f) < 0.01f &&
              std::fabs(g.y - 8.8f) < 0.01f,
          "标定范围内任意点画面 -> 地面往返误差 < 1cm");
    mb::Point back;
    check(gc.to_image({2.2f, 8.8f}, &back) && std::fabs(back.x - synth_image({2.2f, 8.8f}).x) < 1e-4f,
          "地面 -> 画面反投影");
    check(!gc.to_ground({0.5f, -0.5f}, &g), "地平线以上的画面点不换算");
    const float near = gc.sigma_m(synth_image({3, 1}), 3.0f, 1920, 1080);
    const float far = gc.sigma_m(synth_image({3, 9}), 3.0f, 1920, 1080);
    check(near > 0.0f && far > near * 2.0f, "同样 3px 脚点误差, 远处的地面不确定度大得多");
    check(gc.in_hull({3, 5}) && !gc.in_hull({8, 5}), "标定范围 (凸包) 判断");

    // 一个点量错 1m: 拟合质量下降并点名
    std::vector<mb::GroundPair> bad = calib;
    bad.push_back({synth_image({1.5f, 5.0f}), {1.5f, 5.0f}});
    bad.push_back({synth_image({4.5f, 5.0f}), {4.5f, 5.0f}});
    bad.push_back({synth_image({3.0f, 2.5f}), {3.0f, 3.5f}});  // 量错
    mb::GroundCalib gb;
    check(gb.fit(bad, {}, &err) && has_warning(gb.quality(), "可能点错") &&
              gb.quality().grade != "good",
          "离群标定点被点名, 等级降级");

    // 4 点无检查点: 能用但提示无法评估
    std::vector<mb::GroundPair> four(calib.begin(), calib.begin() + 2);
    four.push_back(calib[4]);
    four.push_back(calib[5]);
    mb::GroundCalib g4;
    check(g4.fit(four, {}, &err) && g4.quality().grade == "fair" &&
              has_warning(g4.quality(), "无法评估精度"),
          "只有 4 个点且没有检查点: fair + 提示加检查点");

    // 共线 / 不够
    std::vector<mb::GroundPair> line;
    for (float y : {0.0f, 2.0f, 4.0f, 6.0f, 8.0f}) line.push_back({synth_image({3, y}), {3, y}});
    mb::GroundCalib gl;
    check(!gl.fit(line, {}, &err) && !gl.valid() && gl.quality().grade == "invalid",
          "标定点共线 -> 退化, 不可用");
    check(!gl.fit({calib[0], calib[1], calib[2]}, {}, &err), "少于 4 个点 -> 失败");

    // 配置解析: 矩形模式兼容旧配置
    mb::Json r = config(R"({"points": [[0.15,0.95],[0.85,0.95],[0.85,0.45],[0.15,0.45]],
                            "real_size_m": [4, 8]})");
    std::string mode;
    check(mb::GroundCalib::parse_region(r, &calib, &checks, &mode, &err) && mode == "rect" &&
              calib.size() == 4,
          "旧配置 (4 点 + real_size_m) 按矩形模式解析");
    mb::GroundCalib gr;
    check(gr.fit(calib, checks, &err) && gr.to_ground({0.5f, 0.95f}, &g) &&
              std::fabs(g.x - 2.0f) < 1e-3f && std::fabs(g.y) < 1e-3f &&
              gr.to_ground({0.15f, 0.45f}, &g) && std::fabs(g.y - 8.0f) < 1e-3f,
          "矩形模式: 近边中点 -> (2, 0), 远角 -> (0, 8)");
    mb::Json rp = config(R"({"points": [[0.1,0.9],[0.9,0.9],[0.5,0.4]],
                             "calib_points": [[0.1,0.9,0,0],[0.9,0.9,6,0],[0.7,0.4,6,10],[0.3,0.4,0,10],
                                              [0.5,0.6,3,4]],
                             "check_points": [[0.5,0.9,3,0]]})");
    check(mb::GroundCalib::parse_region(rp, &calib, &checks, &mode, &err) && mode == "points" &&
              calib.size() == 5 && checks.size() == 1,
          "N 点模式: calib_points 优先, check_points 单独解析");
    mb::Json rbad = config(R"({"calib_points": [[0.1,0.9,0],[0.9,0.9,6,0]]})");
    check(!mb::GroundCalib::parse_region(rbad, &calib, &checks, &mode, &err) && !err.empty(),
          "calib_points 写错报出原因");
    mb::Json rnone = config(R"({"points": [[0,0],[1,0],[1,1]]})");
    check(!mb::GroundCalib::parse_region(rnone, &calib, &checks, &mode, &err) && err.empty(),
          "没有标定信息: 不可用但不算配置错误");
}

const char* kLocateConfig = R"JSON({
  "track": {"relink_sec": 2.0, "relink_dist": 0.6},
  "features": {
    "locate": {"enabled": true, "rule": "homography", "classes": ["person", "car", "dog"],
               "report_interval_sec": 5, "max_lost_sec": 3}
  },
  "regions": {
    "ground-1": {"role": "ground", "name": "院子", "type": "quad",
                 "points": [[0.2, 0.9], [0.8, 0.9], [0.7, 0.5], [0.3, 0.5]],
                 "real_size_m": [6, 10], "rules": {"locate": {}}}
  },
  "ground_zones": {
    "door": {"name": "门口", "points_m": [[1, 1], [3, 1], [3, 4], [1, 4]],
             "alarm_enter": true, "dwell_sec": 2, "groups": ["person"]},
    "lane": {"name": "车道", "points_img": [[0.62, 0.88], [0.78, 0.88], [0.68, 0.6], [0.6, 0.6]]}
  }
})JSON";

std::map<std::string, int> locate_classes() { return {{"person", 0}, {"car", 2}, {"dog", 16}}; }

// 地面 (米) 处的目标: 脚点经当前标定反投影到像素
mb::Track at_ground(const mb::RuleEngine& rules, int id, int cls, mb::Point g, float dx = 0.0f) {
    mb::Point n;
    rules.locator().calib().to_image(g, &n);
    mb::Track t = person(id, n.x * W + dx, n.y * H);
    t.cls_id = cls;
    return t;
}

const mb::Json* find_by_id(const mb::Json& arr, const std::string& id) {
    for (const mb::Json& x : arr.items())
        if (x["id"].str("") == id || (x["id"].is_number() && std::to_string(x["id"].integer(-1)) == id))
            return &x;
    return nullptr;
}

// 返回拷贝: Run::all 之后还会追加, 指针会失效
std::optional<mb::RuleEvent> first(const std::vector<mb::RuleEvent>& evs, const char* kind,
                                   const char* rule_id = nullptr) {
    for (const auto& e : evs)
        if (e.feature == "locate" && e.kind == kind && (!rule_id || e.rule_id == rule_id)) return e;
    return std::nullopt;
}

void test_ground_locate() {
    printf("27. 二维视觉定位\n");
    Run r(config(kLocateConfig), locate_classes());
    check(r.rules.locator().enabled() && r.rules.locator().calib().valid(), "定位加载成功");

    // 人进门口: 过 sustain 报越界 (告警), 停满 2s 报停留
    r.step({at_ground(r.rules, 1, 0, {2.0f, 2.5f})}, 20);
    const auto enter = first(r.all, "zone_enter", "door");
    check(enter && enter->severity == mb::Severity::Alarm && enter->has_ground &&
              std::fabs(enter->gx - 2.0f) < 0.05f && std::fabs(enter->gy - 2.5f) < 0.05f,
          "人进入门口: zone_enter 告警, 带地面坐标");
    check(enter && r.rules.liveness(*enter) == mb::RuleEngine::Liveness::Active,
          "仍在区内: 去抖判定 Active");
    check(count(r.all, "locate", "zone_enter") == 1, "只报一次进入");
    check(first(r.all, "position") && first(r.all, "position")->extra.find("±") != std::string::npos,
          "周期位置上报带不确定度");
    r.step({at_ground(r.rules, 1, 0, {2.0f, 2.5f})}, 40);
    check(count(r.all, "locate", "dwell") == 1, "停留超过 dwell_sec 报一次 dwell");

    // 离开到远处
    r.step({at_ground(r.rules, 1, 0, {5.0f, 8.0f})}, 40);
    const auto ex = first(r.all, "zone_exit", "door");
    check(ex && ex->severity == mb::Severity::Info && ex->duration_sec > 2.0f &&
              ex->duration_sec < 3.5f,
          "离开: zone_exit 带本次停留时长");
    check(enter && r.rules.liveness(*enter) == mb::RuleEngine::Liveness::Gone, "离开后旧告警 Gone");

    mb::Json j = r.rules.locator().to_json(r.ts);
    const mb::Json* door = find_by_id(j["zones"], "door");
    check(door && (*door)["stats"]["visits"].integer(0) == 1 &&
              (*door)["stats"]["count_now"].integer(-1) == 0 &&
              (*door)["stats"]["max_dwell_sec"].number(0) > 2.0 &&
              (*door)["stats"]["by_group"]["person"].integer(0) == 1,
          "区域停留统计: 到访次数 / 当前人数 / 最长停留 / 分组");
    const mb::Json* t1 = find_by_id(j["targets"], "1");
    check(t1 && (*t1)["group"].str("") == "person" && (*t1)["trail"].size() >= 2 &&
              (*t1)["distance_m"].number(0) > 5.0 && (*t1)["in_calib"].boolean(false),
          "目标: 分组 / 轨迹 / 累计路程 / 在标定范围内");
    check(j["calib"]["mode"].str("") == "rect" && j["calib"]["hull_m"].size() == 4,
          "鸟瞰图数据带标定范围");

    // 宠物进门口: 门口只管人
    const size_t before = r.all.size();
    r.step({at_ground(r.rules, 3, 16, {2.0f, 2.5f})}, 30);
    std::vector<mb::RuleEvent> dog(r.all.begin() + before, r.all.end());
    check(!first(dog, "zone_enter", "door"), "宠物进只管人的区域不报");
    j = r.rules.locator().to_json(r.ts);
    check(find_by_id(j["targets"], "3") && (*find_by_id(j["targets"], "3"))["group"].str("") == "pet",
          "dog 归到宠物组");

    // 车进画面上画的车道 (points_img 按标定换算成米)
    const mb::Json* lane = find_by_id(j["zones"], "lane");
    check(lane && (*lane)["source"].str("") == "image" && (*lane)["valid"].boolean(false) &&
              (*lane)["points_m"].size() == 4,
          "画面上画的区域自动换算成米制");
    mb::Point c{0, 0};
    if (lane)
        for (const mb::Json& p : (*lane)["points_m"].items()) {
            c.x += (float)p[0].number(0) / 4.0f;
            c.y += (float)p[1].number(0) / 4.0f;
        }
    r.step({at_ground(r.rules, 2, 2, c)}, 20);
    const auto car = first(r.all, "zone_enter", "lane");
    check(car && car->severity == mb::Severity::Info && car->label == "car",
          "车进车道: alarm_enter=false 只记 Info");

    // 静止目标 + 脚点噪声: 平滑后位置更稳
    Run s(config(kLocateConfig), locate_classes());
    float worst = 0.0f;
    for (int i = 0; i < 60; ++i) {
        s.step({at_ground(s.rules, 5, 0, {3.0f, 6.0f}, i % 2 ? 6.0f : -6.0f)});
        if (i < 20) continue;
        const mb::Json snap = s.rules.locator().to_json(s.ts);
        const mb::Json* t = find_by_id(snap["targets"], "5");
        if (t) worst = std::max(worst, (float)std::fabs((*t)["x"].number(0) - 3.0));
    }
    mb::Point raw_g;
    mb::Point n0;
    s.rules.locator().calib().to_image({3.0f, 6.0f}, &n0);
    s.rules.locator().calib().to_ground({(n0.x * W + 6.0f) / W, n0.y}, &raw_g);
    check(worst > 0.0f && worst < std::fabs(raw_g.x - 3.0f) * 0.6f,
          "卡尔曼平滑: 静止目标抖动明显小于原始脚点抖动");

    // ID 切换: 在区内换 id 不重报进入
    Run k(config(kLocateConfig), locate_classes());
    k.step({at_ground(k.rules, 10, 0, {2.0f, 2.0f})}, 20);
    k.step({}, 5);
    k.step({at_ground(k.rules, 11, 0, {2.05f, 2.0f})}, 20);
    check(count(k.all, "locate", "zone_enter") == 1, "ID 切换后不重复报进入");
    j = k.rules.locator().to_json(k.ts);
    check(find_by_id(j["targets"], "11") && (*find_by_id(j["targets"], "11"))["zones"].size() == 1 &&
              !find_by_id(j["targets"], "10"),
          "新 id 继承区域停留, 旧 id 不再出现在鸟瞰图");

    // 热加载: 区域没变不重报; 统计保留
    mb::RuleEngine next;
    std::string err;
    next.load(config(kLocateConfig), locate_classes(), &err);
    next.adopt_state(k.rules);
    k.rules = next;
    k.step({at_ground(k.rules, 11, 0, {2.05f, 2.0f})}, 20);
    check(count(k.all, "locate", "zone_enter") == 1, "热加载后不重复报进入");
    check((*find_by_id(k.rules.locator().to_json(k.ts)["zones"], "door"))["stats"]["visits"].integer(0) == 1,
          "热加载保留区域统计");
    k.rules.reset_ground_stats();
    check((*find_by_id(k.rules.locator().to_json(k.ts)["zones"], "door"))["stats"]["visits"].integer(0) == 1,
          "清零统计时区内的人算作重新到访一次");

    // 丢失: 结束停留
    k.step({}, 100);
    check(count(k.all, "locate", "zone_exit") == 1 &&
              first(k.all, "zone_exit")->extra.find("目标丢失") != std::string::npos,
          "目标丢失超过 max_lost_sec: 结束停留");

    // 画面对齐: 摄像头平移, 标定跟着走
    Run v(config(kLocateConfig), locate_classes());
    mb::Point before_g;
    v.rules.locator().calib().to_ground({0.5f, 0.7f}, &before_g);
    mb::Affine2 shift;
    shift.m[2] = 0.05f;
    v.rules.set_view_transform(shift);
    mb::Point after_g;
    v.rules.locator().calib().to_ground({0.55f, 0.7f}, &after_g);
    check(std::fabs(before_g.x - after_g.x) < 1e-3f && std::fabs(before_g.y - after_g.y) < 1e-3f,
          "画面平移后, 平移后的同一脚点仍落在同一地面位置");
    check(v.rules.locator().to_json(v.ts)["calib"]["view_corrected"].boolean(false),
          "画面跟随状态会报给 APP");

    // 运行期质量监控: 大多数目标在标定范围外
    Run h(config(kLocateConfig), locate_classes());
    for (int i = 0; i < 40; ++i)
        h.step({at_ground(h.rules, 20, 0, {9.0f, 5.0f}, (float)(i % 2))});
    mb::Json rt = h.rules.locator().to_json(h.ts)["runtime"];
    bool reason = false;
    for (const mb::Json& x : rt["reasons"].items())
        reason = reason || x.str("").find("标定范围外") != std::string::npos;
    check(rt["health"].str("") == "degraded" && rt["out_of_calib_ratio"].number(0) > 0.9 && reason,
          "运行期监控: 目标多在标定范围外 -> degraded 并说明原因");
    check(rt["observations"].integer(0) == 40, "只统计新的测量, 重复帧不算");

    // 配置校验
    auto bad = [](const char* text) {
        mb::RuleEngine e;
        std::string err;
        return !e.load(config(text), locate_classes(), &err) && !err.empty();
    };
    check(bad(R"({"ground_zones": {"z": {"points_m": [[0,0],[1,0],[1,1]],
                                         "points_img": [[0,0],[1,0],[1,1]]}}})"),
          "地面区域 points_m / points_img 同时给 -> 拒绝");
    check(bad(R"({"ground_zones": {"z": {"points_m": [[0,0],[1,0]]}}})"), "地面区域少于 3 点 -> 拒绝");
    check(bad(R"({"ground_zones": {"z": {"points_m": [[0,0],[1,0],[1,1]], "groups": ["robot"]}}})"),
          "地面区域分组名不存在 -> 拒绝");
    check(bad(R"({"regions": {"g": {"role": "ground", "points": [[0,0],[1,0],[1,1]],
                                    "calib_points": [[0,0,0,0],[1,0,1,0],[1,1,1,1]]}}})"),
          "地面标定 calib_points 少于 4 个 -> 拒绝 (定位关着也校验)");
    check(!bad(R"({"ground_zones": {"z": {"points_m": [[0,0],[1,0],[1,1]], "groups": ["vehicle"]}}})"),
          "合法地面区域在定位关着时也能加载");
}

void test_geo() {
    printf("28. 设备经纬度\n");
    const auto aps = mb::GeoLocator::parse_nmcli(
        "AA\\:BB\\:CC\\:DD\\:EE\\:01:80:6\nAA\\:BB\\:CC\\:DD\\:EE\\:02:40:149\nbad line\n");
    check(aps.size() == 2 && aps[0].mac == "aa:bb:cc:dd:ee:01" && aps[0].signal_dbm == -60 &&
              aps[1].channel == 149,
          "nmcli -t 输出: 转义的 BSSID / 信号换算 dBm / 信道");
    const auto iw = mb::GeoLocator::parse_iw(
        "BSS 11:22:33:44:55:66(on wlan0) -- associated\n\tfreq: 2437\n\tsignal: -48.00 dBm\n"
        "BSS 11:22:33:44:55:77(on wlan0)\n\tfreq: 5180\n\tsignal: -71.00 dBm\n"
        "\t * primary channel: 36\n");
    check(iw.size() == 2 && iw[0].signal_dbm == -48 && iw[0].channel == 6 && iw[1].channel == 36,
          "iw scan 输出: 信号 / 频率换信道");
    check(mb::GeoLocator::google_request(aps).find("\"considerIp\":false") != std::string::npos ||
              mb::GeoLocator::google_request(aps).find("considerIp") != std::string::npos,
          "Google 请求体");
    mb::GeoFix f;
    std::string err;
    check(mb::GeoLocator::parse_google(R"({"location":{"lat":31.2,"lng":121.5},"accuracy":35})", &f, &err) &&
              f.valid && std::fabs(f.lon - 121.5) < 1e-9 && f.accuracy_m == 35 && f.source == "wifi_google",
          "Google 响应解析");
    check(!mb::GeoLocator::parse_google(R"({"error":{"code":400,"message":"API key not valid"}})", &f, &err) &&
              err.find("API key") != std::string::npos,
          "Google 错误带原因");
    check(mb::GeoLocator::parse_ip(R"({"status":"success","lat":22.5,"lon":113.9,"city":"Shenzhen","country":"China"})",
                                   &f, &err) &&
              f.source == "ip" && f.accuracy_m >= 1000 && f.address.find("Shenzhen") != std::string::npos,
          "ip-api 响应解析 (城市级精度)");
    check(mb::GeoLocator::parse_ip(R"({"loc":"39.9,116.4","city":"Beijing"})", &f, &err) &&
              std::fabs(f.lat - 39.9) < 1e-9,
          "ipinfo 风格响应解析");
    check(!mb::GeoLocator::parse_ip(R"({"status":"fail","message":"private range"})", &f, &err),
          "IP 定位失败");

    // provider 顺序回落: Google 报错 -> IP 成功
    std::vector<std::vector<std::string>> calls;
    mb::GeoLocator geo([&](const std::vector<std::string>& argv, int, std::string* out) {
        calls.push_back(argv);
        if (argv[0] == "nmcli") {
            *out = "AA\\:BB\\:CC\\:DD\\:EE\\:01:80:6\nAA\\:BB\\:CC\\:DD\\:EE\\:02:40:11\n";
            return 0;
        }
        if (argv[0] == "curl" && argv.back().find("googleapis") != std::string::npos) {
            *out = R"({"error":{"message":"API key not valid"}})";
            return 0;
        }
        if (argv[0] == "curl") {
            *out = R"({"status":"success","lat":30.5,"lon":114.3,"city":"Wuhan"})";
            return 0;
        }
        return 127;
    });
    mb::GeoParams p = mb::GeoParams::from_json(config(R"({"geo": {"google_api_key": "k;rm -rf /",
        "manual": {"lat": 1, "lon": 2}}})"));
    geo.configure(p);
    check(geo.locate_once() && geo.fix().source == "ip" && std::fabs(geo.fix().lat - 30.5) < 1e-9,
          "Google 失败回落到 IP 定位");
    mb::Json gj = geo.json();
    check(gj["attempts"]["wifi_google"].str("").find("API key") != std::string::npos &&
              gj["attempts"]["ip"].str("") == "ok" && gj.dump().find("k;rm") == std::string::npos,
          "每个 provider 的结果可查, key 不出现在状态里");
    bool key_in_url_only = false;
    for (const auto& c : calls)
        for (const auto& a : c)
            if (a.find("k;rm -rf /") != std::string::npos)
                key_in_url_only = a.compare(0, 8, "https://") == 0;
    check(key_in_url_only, "命令不经过 shell, key 只作为 URL 参数传给 curl");

    mb::GeoLocator offline([](const std::vector<std::string>&, int, std::string* out) {
        *out = "curl: (6) Could not resolve host";
        return 6;
    });
    offline.configure(p);
    check(offline.locate_once() && offline.fix().source == "manual" && offline.fix().lat == 1.0,
          "没网时回落到手动坐标");
    mb::GeoParams nomanual = p;
    nomanual.has_manual = false;
    offline.configure(nomanual);
    check(!offline.locate_once() && offline.fix().source == "manual",
          "全部失败时保留上一次的结果");

    check(mb::GeoParams::validate(config(R"({"providers": ["ip", "manual"],
                                             "manual": {"lat": 31, "lon": 121}})"), &err),
          "合法 geo 配置");
    check(!mb::GeoParams::validate(config(R"({"providers": ["gps"]})"), &err), "未知 provider -> 拒绝");
    check(!mb::GeoParams::validate(config(R"({"manual": {"lat": 100, "lon": 0}})"), &err),
          "纬度越界 -> 拒绝");
    check(!mb::GeoParams::validate(config(R"({"manual": {"lat": 10}})"), &err),
          "只填了纬度 -> 拒绝");
    check(!mb::GeoParams::validate(config(R"({"ip_url": "file:///etc/passwd"})"), &err),
          "ip_url 只能是 http(s)");
}

}  // namespace

int main() {
    test_boundary_jitter();
    test_real_exit();
    test_occlusion();
    test_id_switch();
    test_isolation_and_multi();
    test_reload();
    test_dwell();
    test_dwell_guard();
    test_tracker();
    test_fire();
    test_package_time();
    test_package();
    test_arm();
    test_zone_alarm();
    test_detector_gate();
    test_region_roles();
    test_activity_map();
    test_view_guard();
    test_event_audio();
    test_alarm_waveform();
    test_line_count();
    test_count_store();
    test_people_in_frame();
    test_count_reset();
    test_multi_region();
    test_ground_calib();
    test_ground_locate();
    test_geo();
    if (g_fail)
        printf("\n结果: %d 项失败\n", g_fail);
    else
        printf("\n结果: 全部通过\n");
    return g_fail ? 1 : 0;
}
