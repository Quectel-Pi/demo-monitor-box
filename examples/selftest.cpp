// mb-selftest —— 不依赖摄像头和模型, 用合成画面把 "事件引擎 + H.264 + 告警录像" 走一遍。
//
// 用法 (板上):
//   ./build/mb-selftest [--dir /tmp/mb-selftest]
//
// 验证点:
//   1. 带 track 的告警要等 debounce_frames 个推理帧才触发
//   2. 冷却期内同类告警被合并, 计数记在下一条的 suppressed 里
//   3. 轨迹提前消失的告警被去抖丢弃
//   4. 跌倒 (pre_confirmed) 直接触发、分级为 critical
//   5. Info 只进内存环, 不落盘
//   6. 快照 JPEG / 事件 JSON / 前后 N 秒 MP4 都落盘, MP4 从 IDR 开始
//   7. 去抖按规则 liveness 判定: Gone 作废, Occluded 挂起不丢, 不同目标各自告警
//   8. 删除: 录像中拒删 (Busy); 只删录像保留记录; 整条删除后不会被写线程写回
//   9. 重启时清理孤儿文件; 超出容量上限时从最旧事件删起
// 全部通过返回 0。MP4 可再用 ffprobe 看时长。

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "event/event_engine.h"
#include "stream/video_pipeline.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

long file_size(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

void paint(mb::ImageRGB* img, int frame) {
    // 灰底 + 一个移动的色块, 让编码器有真实的运动内容
    memset(img->data.data(), 60, img->size_bytes());
    const int bx = (frame * 8) % (img->width - 120);
    for (int y = 200; y < 320; ++y) {
        uint8_t* row = img->row(y);
        for (int x = bx; x < bx + 120; ++x) {
            row[x * 3] = 230;
            row[x * 3 + 1] = (uint8_t)(frame * 3);
            row[x * 3 + 2] = 40;
        }
    }
}

mb::RuleEvent make_event(const char* feature, const char* kind, int track, uint64_t ts,
                         mb::Severity sev) {
    mb::RuleEvent e;
    e.feature = feature;
    e.kind = kind;
    e.rule_id = "zone-1";
    e.name = "test";
    e.track_id = track;
    e.label = "person";
    e.ts_ms = ts;
    e.severity = sev;
    return e;
}

mb::VideoPipelineConfig video_config(int w, int h, int fps) {
    mb::VideoPipelineConfig vc;
    vc.enc.width = w;
    vc.enc.height = h;
    vc.enc.fps = fps;
    vc.enc.gop = fps;
    vc.enc.bitrate_kbps = 2000;
    vc.udp_enabled = false;
    vc.preroll_sec = 1.0;
    return vc;
}

std::string id_of(const mb::EventEngine& ev, int track) {
    const mb::Json all = ev.list(100, "", "");  // 先落到变量里, 临时对象的成员引用会悬空
    for (const mb::Json& e : all["events"].items()) {
        if (e["track_id"].integer(-1) == track) return e["id"].str("");
    }
    return "";
}

void test_delete_and_liveness(const std::string& dir) {
    printf("\n== 删除 / liveness 去抖 / 按目标冷却 ==\n");
    const int W = 640, H = 360, FPS = 25;
    mb::VideoPipeline video;
    std::string err;
    if (!video.start(video_config(W, H, FPS), &err)) {
        check(false, "编码器启动");
        return;
    }
    mb::EventEngineConfig ec;
    ec.dir = dir;
    ec.debounce_frames = 3;
    ec.cooldown_sec = 30.0;
    ec.clip_pre_sec = 0.5;
    ec.clip_post_sec = 1.0;
    ec.routes[0].persist = ec.routes[0].snapshot = ec.routes[0].clip = false;
    mb::EventEngine events;
    if (!events.start(ec, &video, &err)) {
        check(false, "事件引擎启动");
        return;
    }
    mb::ImageRGB img;
    img.width = W;
    img.height = H;
    img.data.resize(img.size_bytes());

    using LV = mb::EventEngine::Liveness;
    int frame = 0;
    // 1/2 一直在区内; 3 已离开; 4 先被遮挡 25 帧再出现
    auto liveness = [&frame](const mb::RuleEvent& ev) {
        switch (ev.track_id) {
            case 1:
            case 2: return LV::Active;
            case 3: return LV::Gone;
            case 4: return frame < 40 ? LV::Occluded : LV::Active;
            default: return LV::Unknown;
        }
    };
    std::vector<mb::Track> tracks;  // 全部依赖 liveness, 不看轨迹
    bool busy_checked = false;
    const uint64_t t0 = 200000;
    for (frame = 0; frame < 3 * FPS; ++frame) {
        const uint64_t ts = t0 + (uint64_t)frame * 1000 / FPS;
        paint(&img, frame);
        std::vector<mb::RuleEvent> evs;
        if (frame == 10) {
            evs.push_back(make_event("intrusion", "enter", 1, ts, mb::Severity::Alarm));
            evs.push_back(make_event("intrusion", "enter", 2, ts, mb::Severity::Alarm));
        }
        if (frame == 12) {
            evs.push_back(make_event("intrusion", "enter", 3, ts, mb::Severity::Alarm));
            evs.push_back(make_event("intrusion", "enter", 4, ts, mb::Severity::Alarm));
        }
        events.push(evs, false);
        events.update(tracks, true, ts, liveness);
        if (frame == 10)
            check(events.highlight_sources().empty(), "去抖未确认的目标不高亮");
        if (frame == 12) {
            const auto sources = events.highlight_sources();
            check(sources.size() >= 2, "同帧两个告警目标都提供高亮触发源");
        }
        events.attach_frame(img);
        if (frame == 13) {
            const auto sources = events.highlight_sources();
            check(std::any_of(sources.begin(), sources.end(),
                              [](const mb::RuleEvent& e) { return e.track_id == 1; }),
                  "快照帧已提交后录像触发源仍保留");
        }
        video.submit(img, ts);
        if (!busy_checked && !id_of(events, 1).empty()) {
            busy_checked = true;
            std::string derr;
            check(events.remove(id_of(events, 1), false, &derr) ==
                      mb::EventEngine::DeleteResult::Busy,
                  "录像生成中拒绝删除 (409)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / FPS));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    video.stop();
    check(events.highlight_sources().empty(), "录像收尾后不再提供持续高亮源");

    const std::string id1 = id_of(events, 1), id2 = id_of(events, 2), id4 = id_of(events, 4);
    check(!id1.empty() && !id2.empty(), "同一区域两个目标各自告警 (冷却按目标区分)");
    check(id_of(events, 3).empty(), "已离开区域 (Gone) 的挂起告警被作废");
    check(!id4.empty(), "遮挡 (Occluded) 超过去抖帧数仍不丢, 重现后触发");

    std::string derr;
    check(events.remove(id1, true, &derr) == mb::EventEngine::DeleteResult::Ok, "只删录像");
    check(file_size(dir + "/" + id1 + ".mp4") < 0 && file_size(dir + "/" + id1 + ".jpg") > 0 &&
              file_size(dir + "/" + id1 + ".json") > 0,
          "录像文件已删, 快照和记录保留");
    mb::Json j;
    events.get(id1, &j);
    check(j["clip_state"].str("") == "deleted" && !j.has("clip"), "记录标记 clip_state=deleted");
    check(events.remove(id1, true, &derr) == mb::EventEngine::DeleteResult::NotFound,
          "重复删录像返回 NotFound");
    check(events.remove(id2, false, &derr) == mb::EventEngine::DeleteResult::Ok, "删除整条事件");
    check(events.remove("20990101-000000-000-00", false, &derr) ==
              mb::EventEngine::DeleteResult::NotFound,
          "不存在的事件返回 NotFound");
    events.stop();
    check(file_size(dir + "/" + id2 + ".json") < 0 && file_size(dir + "/" + id2 + ".jpg") < 0 &&
              file_size(dir + "/" + id2 + ".mp4") < 0,
          "整条删除后文件不会被写线程写回");

    // 孤儿文件: 有快照没 JSON
    const std::string orphan = dir + "/20000101-000000-000-01.jpg";
    if (FILE* f = fopen(orphan.c_str(), "wb")) {
        fputs("x", f);
        fclose(f);
    }
    mb::EventEngine again;
    again.start(ec, nullptr, &err);
    check(file_size(orphan) < 0, "重启时清理孤儿文件");
    check(again.list(100, "", "")["events"].size() == 2, "重启后索引恢复 2 条 (删掉的不回来)");

    // 容量上限: 设成 1KB, 再来一条告警, 旧事件应被删到不超限
    ec.max_storage_mb = 0.001;
    ec.routes[1].clip = false;
    again.reconfigure(ec);
    std::vector<mb::RuleEvent> one{make_event("intrusion", "enter", 9, t0 + 10000, mb::Severity::Alarm)};
    again.push(one, true);
    again.update({}, true, t0 + 10000);
    again.attach_frame(img);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const mb::Json st = again.stats();
    again.stop();
    check(st["stored"].integer(-1) == 0 && st["retention_space"].integer(0) >= 3,
          "超出容量上限时从最旧事件删起");
}

void test_feature_switches() {
    printf("\n== 功能关闭时取消待确认事件 ==\n");
    char tmp[] = "/tmp/mb-events-XXXXXX";
    const char* dir = mkdtemp(tmp);
    if (!dir) {
        check(false, "创建临时事件目录");
        return;
    }
    mb::Json cfg;
    std::string err;
    if (!mb::Json::parse(R"({
        "features": {"intrusion": {"enabled": true}, "fence": {"enabled": true}},
        "events": {"debounce_frames": 3, "cooldown_sec": 0}
    })", &cfg, &err)) {
        check(false, "解析功能开关测试配置");
        rmdir(dir);
        return;
    }
    auto params = [&]() {
        auto ec = mb::EventEngineConfig::from_json(cfg);
        ec.dir = dir;
        for (auto& route : ec.routes) route.snapshot = route.clip = false;
        return ec;
    };
    mb::EventEngine events;
    if (!events.start(params(), nullptr, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        check(false, "启动纯事件引擎");
        rmdir(dir);
        return;
    }
    using LV = mb::EventEngine::Liveness;
    const auto active = [](const mb::RuleEvent&) { return LV::Active; };
    events.push({make_event("intrusion", "enter", 99, 960, mb::Severity::Alarm)}, true);
    auto intrusion = make_event("intrusion", "enter", 1, 1000, mb::Severity::Alarm);
    events.push({intrusion}, false);
    events.update({}, true, 1000, active);
    check(events.stats()["pending_debounce"].integer(-1) == 1, "入侵告警正在等待去抖确认");
    const auto sources = events.highlight_sources();
    check(sources.size() == 1 && sources[0].track_id == 99,
          "高亮只包含已确认告警, 不包含去抖中的目标");
    events.attach_frame(mb::ImageRGB{});
    check(events.highlight_sources().empty(), "没有录像的告警仅在触发帧提供高亮源");

    events.push({make_event("fence", "enter", 2, 1000, mb::Severity::Alarm)}, true);
    mb::Json features = cfg["features"];
    features.set("intrusion", mb::Json::object().set("enabled", mb::Json::bool_value(false)));
    cfg.set("features", features);
    mb::Json settings = cfg["events"];
    settings.set("debounce_frames", mb::Json::number_value(1));
    cfg.set("events", settings);
    events.reconfigure(params());
    check(events.stats()["pending_debounce"].integer(-1) == 0,
          "关闭入侵立即清除待确认告警, 不依赖新推理帧");
    events.update({}, true, 1040, active);
    check(events.list(100, "", "")["events"].size() == 2 && !id_of(events, 99).empty(),
          "关闭功能并降低去抖门限不会确认待处理入侵, 已有历史仍保留");
    intrusion.ts_ms = 1080;
    events.push({intrusion}, true);
    events.push({intrusion}, false);
    events.update({}, true, 1080, active);
    check(events.list(100, "", "")["events"].size() == 2 &&
              events.stats()["pending_debounce"].integer(-1) == 0,
          "关闭后拒绝新入侵事件, 包括跳过去抖的入口");
    events.push({make_event("fence", "enter", 3, 1120, mb::Severity::Alarm)}, false);
    events.update({}, true, 1120, active);
    check(events.list(100, "", "")["events"].size() == 3, "其它功能仍可正常产生告警");

    features.set("intrusion", mb::Json::object().set("enabled", mb::Json::bool_value(true)));
    cfg.set("features", features);
    events.reconfigure(params());
    intrusion.ts_ms = 1160;
    events.push({intrusion}, false);
    events.update({}, true, 1160, active);
    check(events.list(100, "", "")["events"].size() == 4, "重新开启后允许新的入侵告警");
    events.attach_frame(mb::ImageRGB{});
    events.push({make_event("count", "cross", 4, 1200, mb::Severity::Info)}, true);
    events.update({}, true, 1200, active);
    check(events.highlight_sources().empty(), "普通 Info 事件不高亮目标");
    events.attach_frame(mb::ImageRGB{});
    events.stop();
    const mb::Json all = events.list(100, "", "");
    for (const auto& event : all["events"].items())
        check(events.remove(event["id"].str(""), false, &err) ==
                  mb::EventEngine::DeleteResult::Ok, "清理测试事件");
    check(rmdir(dir) == 0, "清理临时事件目录");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--events-only")) {
        test_feature_switches();
        return g_fail ? 1 : 0;
    }
    std::string dir = "/tmp/mb-selftest";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
    }
    std::string cmd = "rm -rf '" + dir + "' '" + dir + "-2'";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "清理 %s 失败\n", dir.c_str());

    const int W = 1280, H = 720, FPS = 25;
    mb::VideoPipelineConfig vc;
    vc.enc.width = W;
    vc.enc.height = H;
    vc.enc.fps = FPS;
    vc.enc.gop = FPS;
    vc.enc.bitrate_kbps = 2000;
    vc.udp_enabled = false;
    vc.preroll_sec = 1.0;

    mb::VideoPipeline video;
    std::string err;
    if (!video.start(vc, &err)) {
        fprintf(stderr, "编码器启动失败: %s\n", err.c_str());
        return 1;
    }

    mb::EventEngineConfig ec;
    ec.dir = dir;
    ec.debounce_frames = 3;
    ec.cooldown_sec = 2.0;
    ec.clip_pre_sec = 1.0;
    ec.clip_post_sec = 1.0;
    ec.feature_levels["fall"] = mb::EventLevel::Critical;
    ec.routes[0].persist = ec.routes[0].snapshot = ec.routes[0].clip = false;
    mb::EventEngine events;
    if (!events.start(ec, &video, &err)) {
        fprintf(stderr, "事件引擎启动失败: %s\n", err.c_str());
        return 1;
    }

    mb::ImageRGB img;
    img.width = W;
    img.height = H;
    img.data.resize(img.size_bytes());

    std::vector<mb::Track> tracks(1);
    tracks[0].id = 1;
    tracks[0].confirmed = true;

    int fired_alarm_frame = -1;
    int fired_count = 0;
    const uint64_t t0 = 100000;
    printf("合成 %d 帧 %dx%d@%d ...\n", 5 * FPS, W, H, FPS);
    for (int f = 0; f < 5 * FPS; ++f) {
        const uint64_t ts = t0 + (uint64_t)f * 1000 / FPS;
        paint(&img, f);

        std::vector<mb::RuleEvent> evs;
        if (f == 40) evs.push_back(make_event("intrusion", "enter", 1, ts, mb::Severity::Alarm));
        if (f == 50) evs.push_back(make_event("intrusion", "enter", 1, ts, mb::Severity::Alarm));
        if (f == 52) evs.push_back(make_event("fence", "enter", 7, ts, mb::Severity::Alarm));
        if (f == 60) evs.push_back(make_event("count", "cross", 1, ts, mb::Severity::Info));
        events.push(evs, false);
        if (f == 70) {
            std::vector<mb::RuleEvent> fe{make_event("fall", "fall", 1, ts, mb::Severity::Alarm)};
            events.push(fe, true);
        }
        // intrusion 第二次 (f=100) 已过 2s 冷却, 应该再出一条且 suppressed=1
        if (f == 100) {
            std::vector<mb::RuleEvent> e2{make_event("intrusion", "enter", 1, ts, mb::Severity::Alarm)};
            events.push(e2, false);
        }

        const std::vector<mb::EventRecord> fired = events.update(tracks, true, ts);
        for (const auto& r : fired) {
            printf("  帧 %3d 触发 %s %s/%s level=%s suppressed=%d\n", f, r.id.c_str(),
                   r.ev.feature.c_str(), r.ev.kind.c_str(), mb::level_name(r.level), r.suppressed);
            if (r.ev.feature == "intrusion" && fired_alarm_frame < 0) fired_alarm_frame = f;
            ++fired_count;
        }
        events.attach_frame(img);
        video.submit(img, ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / FPS));
    }
    // 让最后一段录像的 "后 N 秒" 录满
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    video.stop();
    events.stop();

    printf("检查:\n");
    // 触发当帧算第 1 帧: f40/41/42 三个推理帧都在 -> f42 触发
    check(fired_alarm_frame == 40 + 2, "intrusion 连续 3 个推理帧仍在才触发");
    const mb::Json all = events.list(100, "", "");
    const mb::Json info = events.list(100, "info", "");
    check(all["events"].size() == 3, "落盘 3 条告警 (intrusion x2 + fall)");
    check(info["events"].size() == 1, "Info 事件只进内存环 (1 条)");

    int crit = 0, supp = 0, ok_files = 0;
    for (const mb::Json& e : all["events"].items()) {
        const std::string id = e["id"].str("");
        if (e["level"].str("") == "critical") ++crit;
        if (e["feature"].str("") == "intrusion" && e["suppressed"].integer(0) == 1) ++supp;
        const long jpg = file_size(dir + "/" + id + ".jpg");
        const long mp4 = file_size(dir + "/" + id + ".mp4");
        const long js = file_size(dir + "/" + id + ".json");
        printf("  %s %-9s jpg=%ld mp4=%ld json=%ld clip_state=%s\n", id.c_str(),
               e["feature"].str("").c_str(), jpg, mp4, js, e["clip_state"].str("").c_str());
        if (jpg > 0 && mp4 > 0 && js > 0 && e["clip_state"].str("") == "ready") ++ok_files;
    }
    check(crit == 1, "跌倒分级为 critical");
    check(supp == 1, "冷却期内被合并的那条记进 suppressed");
    check(ok_files == 3, "每条告警都有 JPEG + JSON + MP4");
    const mb::Json st = events.stats();
    check(st["debounce_dropped"].integer(0) == 1, "fence 轨迹不存在, 被去抖丢弃");
    check(st["cooldown_suppressed"].integer(0) == 1, "冷却计数 = 1");

    test_delete_and_liveness(dir + "-2");

    if (g_fail)
        printf("\n结果: %d 项失败\n", g_fail);
    else
        printf("\n结果: 全部通过\n");
    return g_fail ? 1 : 0;
}
