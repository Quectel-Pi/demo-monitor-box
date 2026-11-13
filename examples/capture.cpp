// mb-capture —— 第一个能跑起来的版本: 摄像头进, 框 + 规则事件出。
//
// 用法 (板上, 项目根目录):
//   sudo ./build/mb-capture --config config/monitor_box.json
//   sudo ./build/mb-capture --config config/monitor_box.json --device /dev/video73
//   ./build/mb-capture --list                     # 只列采集节点
//   sudo ./build/mb-capture --config ... --frames 30 --save /tmp/mb
//
// 每帧的流程 (和 docs/architecture.zh-CN.md 2 的数据流一致):
//
//   V4L2 采集 -> JPEG/YUYV 解码 -> letterbox -> yolo.rknn -> NMS
//        -> ByteTrack 跟踪 -> RuleEngine (入侵/围栏/越线/滞留/遗留/定位)
//        -> [可选] pose.rknn -> 跌倒判定
//        -> [可选] fire.rknn (整帧, models.fire.fps 低频) -> 火焰烟雾时序确认
//        -> 事件引擎 (去抖/冷却/分级 -> 快照 + 事件 JSON + 前后 N 秒 MP4)
//        -> OSD 画框/多边形/事件文字
//        -> H.264 (MPP) -> RTP/UDP 127.0.0.1:15004 (APP) + 告警录像环形缓冲
//        -> MJPEG 小窗 -> HTTP :8080/stream (浏览器 / APP 兜底)
//
// 为什么把检测/跟踪/规则拆成三个模块各跑各的:
//   检测和姿态模型是 NPU 上最贵的两步 (实测 19.7ms / 59.5ms), 状态类判断
//   (滞留计时、越线、跌倒帧计数) 必须按每帧节奏走, 不能跟着推理降频。所以这里
//   推理按 infer_fps 抽帧, 规则每帧都拿到最新轨迹。推理放在独立线程 (InferWorker),
//   视频通路 (解码 -> OSD -> 编码/预览) 不等 NPU, 开了姿态也能保持采集帧率。
//
// 注意 OSD 的 draw_text 使用内嵌的 Cantarell 字形 (与设置页英文字体一致), 只支持
// ASCII; 所以画面上的文字用英文/ID, 中文名留给网页和事件日志。

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "common/json.h"
#include "event/dwell_guard.h"
#include "event/speaker.h"
#include "event/zone_alarm.h"
#include "event/event_engine.h"
#include "infer/infer_worker.h"
#include "infer/jpeg_io.h"
#include "infer/preprocess.h"
#include "infer/rknn_engine.h"
#include "infer/pose_post.h"
#include "infer/types.h"
#include "infer/yolo_post.h"
#include "geo/geo_locator.h"
#include "rules/fall_detector.h"
#include "rules/activity_map.h"
#include "video/view_guard.h"
#include "rules/fire_detector.h"
#include "rules/rule_engine.h"
#include "rules/count_store.h"
#include "rules/rule_types.h"
#include "stream/video_pipeline.h"
#include "track/tracker.h"
#include "video/v4l2_source.h"
#include "video/diagnostics_osd.h"
#include "video/region_overlay.h"
#include "video/alarm_osd.h"
#include "video/bev_osd.h"
#include "web/http_server.h"
#include "web/web_api.h"

extern "C" {
#include "image_drawing.h"
}

namespace {

struct Options {
    std::string config = "config/monitor_box.json";
    std::string device;     // 覆盖配置里的 video.device
    std::string model;      // 覆盖 infer.model
    std::string pose_model; // 覆盖 models.pose.path
    std::string fire_model; // 覆盖 models.fire.path
    std::string labels;     // 覆盖 infer.labels
    std::string save_dir;   // 非空则每帧存一张标注图 (调试用)
    int frames = -1;        // 采多少帧后退出, -1 = 一直跑
    bool list = false;
    bool no_pose = false;
    bool no_fire = false;
    bool no_web = false;
    bool no_h264 = false;
    bool fall_debug = false;
};

void usage(const char* argv0) {
    printf(
        "用法: %s --config <monitor_box.json> [选项]\n"
        "\n"
        "  --config <path>   配置文件, 默认 config/monitor_box.json\n"
        "  --device <path>   覆盖 video.device, 如 /dev/video73\n"
        "  --model <path>    覆盖 infer.model (检测模型)\n"
        "  --pose <path>     覆盖 models.pose.path (姿态模型)\n"
        "  --fire <path>     覆盖 models.fire.path (火焰烟雾模型)\n"
        "  --labels <path>   覆盖 infer.labels\n"
        "  --save <dir>      每帧存标注图 (调试), 例 --save /tmp/mb\n"
        "  --frames <n>      采集 n 帧后退出, 默认一直跑 (Ctrl+C)\n"
        "  --no-pose         即使配置里开了跌倒也不跑姿态模型\n"
        "  --no-fire         即使配置里开了火焰烟雾也不跑火焰模型\n"
        "  --no-web          不起 HTTP 服务 (覆盖 web.enabled)\n"
        "  --no-h264         不起 H.264 编码/UDP 推流/告警录像 (覆盖 stream.h264.enabled)\n"
        "  --fall-debug      每次姿态推理打印每人的跌倒判定特征 (调阈值用)\n"
        "  --list            列出可用的采集节点后退出\n",
        argv0);
}

bool parse_args(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "缺少参数: %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            exit(0);
        } else if (a == "--config") {
            const char* v = next("--config");
            if (!v) return false;
            o->config = v;
        } else if (a == "--device") {
            const char* v = next("--device");
            if (!v) return false;
            o->device = v;
        } else if (a == "--model") {
            const char* v = next("--model");
            if (!v) return false;
            o->model = v;
        } else if (a == "--pose") {
            const char* v = next("--pose");
            if (!v) return false;
            o->pose_model = v;
        } else if (a == "--fire") {
            const char* v = next("--fire");
            if (!v) return false;
            o->fire_model = v;
        } else if (a == "--no-fire") {
            o->no_fire = true;
        } else if (a == "--labels") {
            const char* v = next("--labels");
            if (!v) return false;
            o->labels = v;
        } else if (a == "--save") {
            const char* v = next("--save");
            if (!v) return false;
            o->save_dir = v;
        } else if (a == "--frames") {
            const char* v = next("--frames");
            if (!v) return false;
            o->frames = atoi(v);
        } else if (a == "--no-pose") {
            o->no_pose = true;
        } else if (a == "--no-web") {
            o->no_web = true;
        } else if (a == "--no-h264") {
            o->no_h264 = true;
        } else if (a == "--fall-debug") {
            o->fall_debug = true;
        } else if (a == "--list") {
            o->list = true;
        } else {
            fprintf(stderr, "未知参数: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

unsigned int color_for_class(int cls) {
    // 和 mb-infer 同一套配色, 两边截图对照方便。
    static const unsigned int palette[] = {
        0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF,
        0xFF00FFFF, 0xFFFF8000, 0xFF8000FF, 0xFF00FF80, 0xFF80FF00,
    };
    return palette[(cls < 0 ? 0 : cls) % (int)(sizeof(palette) / sizeof(palette[0]))];
}

// 把 ImageRGB 包成 zoo 的 image_buffer_t, 让 image_drawing 能往上画。
image_buffer_t wrap_osd(mb::ImageRGB* img) {
    image_buffer_t b;
    memset(&b, 0, sizeof(b));
    b.width = img->width;
    b.height = img->height;
    b.width_stride = img->width;
    b.height_stride = img->height;
    b.format = IMAGE_FORMAT_RGB888;
    b.virt_addr = img->data.data();
    b.size = (int)img->size_bytes();
    b.fd = -1;
    return b;
}

void draw_polyline(image_buffer_t* osd, const std::vector<mb::Point>& pts, unsigned int color,
                   int thickness) {
    if (pts.size() < 2) return;
    for (size_t i = 0; i < pts.size(); ++i) {
        const mb::Point& a = pts[i];
        const mb::Point& b = pts[(i + 1) % pts.size()];
        draw_line(osd, (int)a.x, (int)a.y, (int)b.x, (int)b.y, color, thickness);
    }
}

// 计数线 + 指向 "进" 一侧的箭头。方向与 RuleEngine::write_line_events 的 in 一致:
// 从 p1 看向 p2, 右手边 (图像坐标 y 向下) 是 "进"。
void draw_count_line(image_buffer_t* osd, mb::Point a, mb::Point b, unsigned int color) {
    draw_line(osd, (int)a.x, (int)a.y, (int)b.x, (int)b.y, color, 3);
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    const float nx = -dy / len, ny = dx / len;
    const float arrow = std::min(60.0f, std::max(24.0f, len * 0.08f));
    const mb::Point m{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    const mb::Point tip{m.x + nx * arrow, m.y + ny * arrow};
    draw_line(osd, (int)m.x, (int)m.y, (int)tip.x, (int)tip.y, color, 3);
    const float hx = -nx * arrow * 0.35f, hy = -ny * arrow * 0.35f;
    draw_line(osd, (int)tip.x, (int)tip.y, (int)(tip.x + hx - ny * arrow * 0.25f),
              (int)(tip.y + hy + nx * arrow * 0.25f), color, 3);
    draw_line(osd, (int)tip.x, (int)tip.y, (int)(tip.x + hx + ny * arrow * 0.25f),
              (int)(tip.y + hy - nx * arrow * 0.25f), color, 3);
    draw_text(osd, "IN", (int)(tip.x + nx * 8.0f) - 8, (int)(tip.y + ny * 8.0f) - 8, color, 10);
}

struct Stats {
    int64_t frames = 0;
    int64_t infers = 0;          // 这一统计窗口内的推理次数
    double ai_total_ms = 0.0;    // 这一窗口内 AI 总耗时
    double infer_ms = 0.0;       // 最近一次 AI 耗时 (检测 + 姿态)
    double decode_ms = 0.0;      // 窗口内 JPEG 解码总耗时
    double loop_ms = 0.0;        // 窗口内单帧处理 (解码 -> 送出编码/预览) 总耗时
    double ai_consume_ms = 0.0;  // 主线程接收推理结果、跟踪、业务状态处理
    double infer_copy_ms = 0.0;  // 向推理线程拷贝当前 RGB 帧
    double rules_ms = 0.0;       // RuleEngine 每帧更新
    double osd_ms = 0.0;         // 区域填充、框、文字、BEV
    double osd_shapes_ms = 0.0;  // 区域填充与边界
    double osd_objects_ms = 0.0; // 目标框与告警框
    double osd_text_ms = 0.0;    // 诊断/横幅/计数/时钟
    double osd_bev_ms = 0.0;     // 鸟瞰图画中画
    double encode_fill_ms = 0.0; // H.264 输入 RGA 转换
    double latency_ms = 0.0;     // 窗口内 "驱动出帧 -> 送进编码器" 总时延
    int64_t latency_n = 0;
    std::chrono::steady_clock::time_point t_last;
};

// 窗口平均值, 给 /status 和 [stat] 日志
struct StatSnapshot {
    double fps = 0, ai_ms = 0, decode_ms = 0, loop_ms = 0, latency_ms = 0;
    double ai_consume_ms = 0, infer_copy_ms = 0, rules_ms = 0, osd_ms = 0, encode_fill_ms = 0;
    double osd_shapes_ms = 0, osd_objects_ms = 0, osd_text_ms = 0, osd_bev_ms = 0;
};

std::atomic<bool> g_running{true};

void on_signal(int) { g_running = false; }

uint64_t steady_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 配置文件的 "版本": APP 用 tempfile + os.replace 原子替换, inode 会变; 手工编辑只变 mtime。
struct FileStamp {
    long long mtime_ns = 0;
    unsigned long long ino = 0;
    bool operator!=(const FileStamp& o) const { return mtime_ns != o.mtime_ns || ino != o.ino; }
};

FileStamp stamp_of(const std::string& path) {
    FileStamp s;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        s.mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
        s.ino = (unsigned long long)st.st_ino;
    }
    return s;
}

mb::FallParams fall_params_from(const mb::Json& cfg) {
    mb::FallParams fp;
    fp.torso_angle_threshold = (float)cfg.path("features.fall.torso_angle_threshold").number(55.0);
    fp.height_ratio_threshold =
        (float)cfg.path("features.fall.height_ratio_threshold").number(1.2);
    fp.aspect_ratio_threshold =
        (float)cfg.path("features.fall.aspect_ratio_threshold").number(1.05);
    fp.min_kpt_conf = (float)cfg.path("features.fall.min_kpt_conf").number(0.40);
    fp.kpt_visible_ratio = (float)cfg.path("features.fall.kpt_visible_ratio").number(0.70);
    fp.min_trigger_frames = cfg.path("features.fall.min_trigger_frames").integer(15);
    fp.recover_frames = cfg.path("features.fall.recover_frames").integer(30);
    fp.use_velocity = cfg.path("features.fall.use_velocity").boolean(true);
    fp.velocity_drop_px = (float)cfg.path("features.fall.velocity_drop_px").number(110.0);
    fp.velocity_window = cfg.path("features.fall.velocity_window").integer(5);
    fp.min_person_score = (float)cfg.path("features.fall.min_person_score").number(0.40);
    fp.cooldown_s = (float)cfg.path("features.fall.cooldown_s").number(15.0);
    return fp;
}

mb::FireParams fire_params_from(const mb::Json& cfg) {
    mb::FireParams p;
    const mb::Json& f = cfg.path("features.fire");
    // classes 决定报哪几类; 没写时 fire/smoke 都报
    const mb::Json& cls = f["classes"];
    if (cls.is_array()) {
        p.fire.enabled = p.smoke.enabled = false;
        for (size_t i = 0; i < cls.size(); ++i) {
            const std::string n = cls[i].str("");
            if (n == "fire") p.fire.enabled = true;
            if (n == "smoke") p.smoke.enabled = true;
        }
    }
    p.fire.conf = (float)f["fire_conf"].number(p.fire.conf);
    p.fire.confirm_sec = (float)f["confirm_sec"].number(p.fire.confirm_sec);
    p.smoke.conf = (float)f["smoke_conf"].number(p.smoke.conf);
    p.smoke.confirm_sec = (float)f["smoke_confirm_sec"].number(p.smoke.confirm_sec);
    p.min_area_ratio = (float)f["min_area_ratio"].number(p.min_area_ratio);
    p.hit_ratio = (float)f["hit_ratio"].number(p.hit_ratio);
    p.min_hits = f["min_hits"].integer(p.min_hits);
    p.gap_sec = (float)f["gap_sec"].number(p.gap_sec);
    p.clear_sec = (float)f["clear_sec"].number(p.clear_sec);
    return p;
}

// 功耗模式 (power.mode), 可热加载:
//   normal  按 video/infer/models 里的帧率全速跑
//   eco     降采集帧率 + 降 NPU 推理频率, 检测/告警/录像照常, 只是更"慢"
//   standby 关摄像头 (STREAMOFF + close, UVC 可进 USB 挂起), 不解码/推理/编码;
//           Web / 配置 / 事件查询照常, 改回 normal/eco 后自动重新打开摄像头
enum class PowerMode { Normal, Eco, Standby };

struct PowerParams {
    PowerMode mode = PowerMode::Normal;
    int eco_capture_fps = 10;
    int eco_infer_fps = 4;
    int eco_pose_fps = 3;
    int eco_fire_fps = 1;
};

const char* power_mode_name(PowerMode m) {
    switch (m) {
        case PowerMode::Eco: return "eco";
        case PowerMode::Standby: return "standby";
        default: return "normal";
    }
}

PowerParams power_params_from(const mb::Json& cfg) {
    PowerParams p;
    const std::string mode = cfg.path("power.mode").str("normal");
    if (mode == "eco") {
        p.mode = PowerMode::Eco;
    } else if (mode == "standby") {
        p.mode = PowerMode::Standby;
    } else if (mode != "normal") {
        fprintf(stderr, "[mb-capture] power.mode=%s 不认识, 按 normal 处理\n", mode.c_str());
    }
    p.eco_capture_fps = cfg.path("power.eco.capture_fps").integer(p.eco_capture_fps);
    p.eco_infer_fps = cfg.path("power.eco.infer_fps").integer(p.eco_infer_fps);
    p.eco_pose_fps = cfg.path("power.eco.pose_fps").integer(p.eco_pose_fps);
    p.eco_fire_fps = cfg.path("power.eco.fire_fps").integer(p.eco_fire_fps);
    return p;
}

// 节能模式下取 "配置值" 和 "节能上限" 中较低的那个; 配置值 <=0 表示不限, 此时直接用上限。
int capped_fps(int base, int cap) {
    if (cap <= 0) return base;
    return base <= 0 ? cap : std::min(base, cap);
}

void print_events(const std::vector<mb::RuleEvent>& events) {
    for (const mb::RuleEvent& e : events) {
        const char* sev = e.severity == mb::Severity::Alarm ? "ALARM" : "info ";
        printf("[%s] %-9s %-9s track=%-3d %-8s %s\n", sev, e.feature.c_str(), e.kind.c_str(),
               e.track_id, e.label.c_str(), e.extra.c_str());
        fflush(stdout);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) {
        usage(argv[0]);
        return 2;
    }

    if (opt.list) {
        std::string err;
        const std::vector<std::string> devs = mb::V4l2Source::list_capture_devices(&err);
        if (devs.empty()) {
            fprintf(stderr, "[mb-capture] 没有找到采集节点: %s\n", err.c_str());
            return 1;
        }
        printf("可用的采集节点:\n");
        for (const std::string& d : devs) printf("  %s\n", d.c_str());
        return 0;
    }

    std::string err;

    // ---- 1. 配置 ----
    mb::Json cfg;
    if (!mb::Json::parse_file(opt.config, &cfg, &err)) {
        fprintf(stderr, "[mb-capture] 读配置失败 (%s): %s\n", opt.config.c_str(), err.c_str());
        return 1;
    }
    FileStamp cfg_stamp = stamp_of(opt.config);

    const std::string dev =
        !opt.device.empty() ? opt.device : cfg.path("video.device").str("/dev/video0");
    const int cam_w = cfg.path("video.width").integer(1920);
    const int cam_h = cfg.path("video.height").integer(1080);
    const int cam_fps = cfg.path("video.fps").integer(30);
    const std::string pixfmt = cfg.path("video.pixel_format").str("MJPG");
    const int nbuf = cfg.path("video.buffers").integer(4);

    const std::string model_path =
        !opt.model.empty() ? opt.model
                           : cfg.path("infer.model").str(
                                 "/opt/monitor_box/models/yolov8s-640-640.rknn");
    const std::string label_path =
        !opt.labels.empty() ? opt.labels
                            : cfg.path("infer.labels").str(
                                  "/opt/monitor_box/models/coco_80_labels_list.txt");
    const int core_mask = cfg.path("infer.core_mask").integer(0);
    float conf = (float)cfg.path("infer.conf_threshold").number(0.25);
    float nms = (float)cfg.path("infer.nms_threshold").number(0.45);
    const bool use_rga = cfg.path("infer.use_rga").boolean(true);
    int infer_fps = cfg.path("infer.infer_fps").integer(12);
    PowerParams power = power_params_from(cfg);

    // ---- 2. 模型 ----
    mb::RknnEngine det;
    if (!det.load(model_path, core_mask, &err)) {
        fprintf(stderr, "[mb-capture] 检测模型加载失败: %s\n", err.c_str());
        return 1;
    }
    det.print_versions();

    std::vector<std::string> labels = mb::load_labels(label_path, &err);
    if (labels.empty()) fprintf(stderr, "[mb-capture] 读标签失败(%s), 只显示类号\n", err.c_str());

    // 标签名 -> 类号, 规则引擎用它把配置里的 "person" 解析成 0
    std::map<std::string, int> class_map;
    for (size_t i = 0; i < labels.size(); ++i) class_map[labels[i]] = (int)i;
    if (class_map.empty()) class_map["person"] = 0;  // 没有标签文件时至少保住 person

    // 姿态模型 (跌倒用)。配置里没开就完全不加载, 省 14MB 内存和 NPU 时间;
    // 运行中通过热加载把跌倒打开时再按需加载。
    mb::RknnEngine pose;
    bool pose_ok = false;
    std::string pose_failed_path;  // 加载失败过的路径, 热加载时不反复重试同一个坏文件
    auto ensure_pose = [&](const mb::Json& c) {
        if (pose_ok || opt.no_pose || !c.path("features.fall.enabled").boolean(false)) return;
        const std::string path =
            !opt.pose_model.empty() ? opt.pose_model : c.path("models.pose.path").str("");
        if (path.empty()) {
            fprintf(stderr, "[mb-capture] 跌倒开了但 models.pose.path 为空, 跳过\n");
            return;
        }
        if (path == pose_failed_path) return;
        std::string perr;
        if (!pose.load(path, c.path("models.pose.core_mask").integer(0), &perr)) {
            fprintf(stderr, "[mb-capture] 姿态模型加载失败 (%s): %s\n", path.c_str(), perr.c_str());
            pose_failed_path = path;
            return;
        }
        const mb::PoseModelKind kind = mb::pose_model_kind(pose);
        if (kind == mb::PoseModelKind::Unknown) {
            fprintf(stderr,
                    "[mb-capture] 姿态模型 %s 输出格式不认识 (支持 yolov8-pose 56 通道 / RTMPose "
                    "SimCC), 跌倒不可用\n",
                    path.c_str());
            pose.release();
            pose_failed_path = path;
            return;
        }
        pose_ok = true;
        printf("[mb-capture] 姿态模型 %s 输入 %dx%d 类型 %s\n", path.c_str(), pose.input_width(),
               pose.input_height(), mb::pose_model_kind_name(kind));
    };
    ensure_pose(cfg);

    // 火焰烟雾模型。和姿态一样按需加载 (热加载打开时再加载)。类别名按名字找 fire/smoke,
    // 不依赖类号顺序 (D-Fire 系模型是 0=smoke 1=fire, 别的数据集可能相反)。
    mb::RknnEngine fire_model;
    bool fire_ok = false;
    std::string fire_failed_path;
    std::vector<std::string> fire_labels;
    auto ensure_fire = [&](const mb::Json& c) {
        if (fire_ok || opt.no_fire || !c.path("features.fire.enabled").boolean(false)) return;
        const std::string path =
            !opt.fire_model.empty() ? opt.fire_model : c.path("models.fire.path").str("");
        if (path.empty()) {
            fprintf(stderr, "[mb-capture] 火焰烟雾开了但 models.fire.path 为空, 跳过\n");
            return;
        }
        if (path == fire_failed_path) return;
        std::string ferr;
        if (!fire_model.load(path, c.path("models.fire.core_mask").integer(0), &ferr)) {
            fprintf(stderr, "[mb-capture] 火焰模型加载失败 (%s): %s\n", path.c_str(), ferr.c_str());
            fire_failed_path = path;
            return;
        }
        if (mb::yolo_head_kind(fire_model) != mb::YoloHead::YoloV8) {
            fprintf(stderr,
                    "[mb-capture] 火焰模型 %s 输出格式不认识 (需要 airockchip yolov8 RKNN 优化格式, "
                    "见 tools/fire_model/), 火焰烟雾不可用\n",
                    path.c_str());
            fire_model.release();
            fire_failed_path = path;
            return;
        }
        const int nc = mb::yolo_num_classes(fire_model);
        const std::string lpath = c.path("models.fire.labels").str("");
        std::string lerr;
        fire_labels = lpath.empty() ? std::vector<std::string>() : mb::load_labels(lpath, &lerr);
        if (fire_labels.empty() && nc == 2) {
            fire_labels = {"smoke", "fire"};
            fprintf(stderr, "[mb-capture] 火焰标签 %s 读不到, 按 D-Fire 约定 0=smoke 1=fire\n",
                    lpath.c_str());
        }
        if ((int)fire_labels.size() != nc) {
            fprintf(stderr, "[mb-capture] 火焰标签 %zu 类与模型 %d 类不一致, 火焰烟雾不可用\n",
                    fire_labels.size(), nc);
            fire_model.release();
            fire_failed_path = path;
            return;
        }
        fire_ok = true;
        std::string names;
        for (const std::string& n : fire_labels) names += (names.empty() ? "" : ",") + n;
        printf("[mb-capture] 火焰模型 %s 输入 %dx%d 类别 [%s]\n", path.c_str(),
               fire_model.input_width(), fire_model.input_height(), names.c_str());
    };
    ensure_fire(cfg);

    // ---- 3. 规则引擎 ----
    mb::RuleEngine rules;
    if (!rules.load(cfg, class_map, &err)) {
        fprintf(stderr, "[mb-capture] 规则配置错误: %s\n", err.c_str());
        return 1;
    }
    auto print_features = [&]() {
        int enabled_features = 0;
        for (const mb::RuleEngine::Feature& f : rules.features()) {
            printf("[mb-capture] 功能 %-10s %s  rule=%s\n", f.id.c_str(),
                   f.enabled ? "开" : "关", f.rule.empty() ? "-" : f.rule.c_str());
            if (f.enabled) ++enabled_features;
        }
        printf("[mb-capture] 共启用 %d 个规则功能\n", enabled_features);
    };
    print_features();

    // ---- 3b. 活动热图 (APP 按它推荐马路/通道区域) ----
    mb::ActivityMap activity;
    activity.configure(mb::ActivityMap::Params::from_json(cfg), class_map);
    if (activity.params().enabled && !activity.params().path.empty()) {
        std::string aerr;
        if (activity.load(&aerr))
            printf("[mb-capture] 活动热图: 接着 %s 统计\n", activity.params().path.c_str());
        else
            printf("[mb-capture] 活动热图: 从头统计 (%s)\n", aerr.c_str());
    }

    // ---- 3b2. 越线计数按小时落盘 (真相在 RuleEngine, 这里只记增量) ----
    mb::CountStore counts;
    counts.configure(mb::CountStore::Params::from_json(cfg));
    if (!counts.params().log_path.empty())
        printf("[mb-capture] 越线计数: 累计写入 %s\n", counts.params().log_path.c_str());

    // ---- 3c. 画面对齐 (摄像头被碰歪时区域自动跟随) ----
    mb::ViewGuard view;
    view.configure(mb::ViewGuard::Params::from_json(cfg));
    if (view.params().enabled) {
        std::string verr;
        if (view.load(&verr))
            printf("[mb-capture] 画面对齐: 读取参考画面 (%s)\n", view.params().dir.c_str());
        else
            printf("[mb-capture] 画面对齐: 稍后把当前画面存为参考 (%s)\n", verr.c_str());
    }
    view.set_ignore(rules.base_ignore_polygons());
    rules.set_view_transform(view.applied());
    view.start();

    // ---- 4. 跟踪 ----
    mb::TrackerParams tp;
    tp.enabled = cfg.path("track.enabled").boolean(true);
    tp.track_buffer = cfg.path("track.track_buffer").integer(90);
    mb::Tracker tracker(tp);

    // ---- 5. 跌倒 ----
    mb::FallDetector fall(fall_params_from(cfg));
    bool fall_enabled = cfg.path("features.fall.enabled").boolean(false);
    mb::PoseParams pp;
    pp.conf_threshold = (float)cfg.path("features.fall.pose_conf").number(0.35);
    mb::PoseRoiParams proi;
    proi.enabled = cfg.path("features.fall.pose_roi").boolean(true);
    proi.max_persons = cfg.path("features.fall.pose_roi_max_persons").integer(3);
    proi.expand = (float)cfg.path("features.fall.pose_roi_expand").number(1.25);

    // ---- 5b. 火焰烟雾 ----
    mb::FireDetector fire(fire_params_from(cfg), fire_labels);
    bool fire_enabled = cfg.path("features.fire.enabled").boolean(false);

    // ---- 5c. 检测模型是否需要跑 ----
    // "物体检测"只是吃 YOLO 结果的功能之一, 入侵/围栏/越线/滞留/包裹/定位都吃同一份结果,
    // 跌倒也要靠人框做姿态 ROI 和轨迹关联。只有它们全关时才停掉检测, 省 NPU。
    // 活动热图在 "学习" 时也要检测结果, 哪怕功能全关 (先学几小时再按热图画区域)。
    auto detector_needed = [&]() {
        return rules.needs_detector() || fall_enabled ||
               (activity.params().enabled && activity.params().learning);
    };
    bool det_running = detector_needed();
    printf("[mb-capture] 检测模型 %s\n", det_running ? "运行" : "停用 (没有功能需要)");

    // ---- 6. 摄像头 ----
    // video.controls: 按 v4l2-ctl 的控件名设置。没写这一段时默认关掉
    // exposure_dynamic_framerate —— 这颗 UVC 默认开着, 光线一暗就把帧率从 30 降到 15,
    // 推流和录像跟着卡一半。
    std::vector<std::pair<std::string, int>> cam_controls;
    if (cfg.path("video").has("controls")) {
        for (const auto& kv : cfg.path("video.controls").members()) {
            const mb::Json& v = kv.second;
            cam_controls.emplace_back(kv.first, v.type() == mb::Json::Type::Bool
                                                    ? (v.boolean(false) ? 1 : 0)
                                                    : v.integer(0));
        }
    } else {
        cam_controls.emplace_back("exposure_dynamic_framerate", 0);
    }

    // 摄像头不在/被占/中途掉线都不退出: Web、推流、事件服务照常, 采集按退避重试。
    // 配置里的节点打不开时自动找一个 uvcvideo 节点 (拔插换口后编号会变)。
    mb::V4l2Source cam;
    cam.set_hardware_jpeg(cfg.path("video.hw_jpeg").boolean(false));
    std::atomic<bool> capture_watchdog_enabled{false};
    std::atomic<uint64_t> capture_heartbeat_ms{steady_ms()};
    std::string cam_error;
    int cam_reconnects = 0;
    // 节能模式向摄像头要更低的帧率 (USB 带宽、MJPEG 解码、编码都跟着降);
    // 不支持该帧率的摄像头会被下面主循环里的软件抽帧兜住。
    auto want_cam_fps = [&]() -> int {
        return power.mode == PowerMode::Eco ? capped_fps(cam_fps, power.eco_capture_fps) : cam_fps;
    };
    int cam_open_fps = 0;  // 当前摄像头是按多少帧率打开的
    auto open_camera = [&]() -> bool {
        std::string oerr;
        const int fps = want_cam_fps();
        if (cam.open(dev, cam_w, cam_h, fps, pixfmt, nbuf, &oerr)) {
            cam_error.clear();
        } else {
            const std::string found = opt.device.empty() ? mb::V4l2Source::find_uvc_device() : "";
            std::string ferr2;
            if (found.empty() || found == dev ||
                !cam.open(found, cam_w, cam_h, fps, pixfmt, nbuf, &ferr2)) {
                cam_error = oerr + (ferr2.empty() ? "" : "; " + found + ": " + ferr2);
                return false;
            }
            printf("[mb-capture] %s 打不开 (%s), 改用自动找到的 %s\n", dev.c_str(), oerr.c_str(),
                   found.c_str());
            cam_error.clear();
        }
        std::vector<std::string> warns;
        cam.apply_controls(cam_controls, &warns);
        for (const std::string& w : warns) printf("[mb-capture] 摄像头控件: %s\n", w.c_str());
        cam_open_fps = fps;
        capture_heartbeat_ms.store(steady_ms(), std::memory_order_relaxed);
        capture_watchdog_enabled.store(true, std::memory_order_relaxed);
        printf("[mb-capture] %s %dx%d %s fps<=%d (%s)\n", cam.device().c_str(), cam.width(),
               cam.height(), cam.pixel_format_name(), fps, power_mode_name(power.mode));
        fflush(stdout);
        return true;
    };
    if (power.mode == PowerMode::Standby) {
        printf("[mb-capture] power.mode=standby: 摄像头保持关闭, 改为 normal/eco 后自动打开\n");
    } else if (!open_camera()) {
        fprintf(stderr, "[mb-capture] 打开摄像头失败 (%s): %s\n", dev.c_str(), cam_error.c_str());
        if (opt.frames > 0) return 1;  // 一次性调试跑就别等了
        fprintf(stderr, "           继续启动其余服务, 后台每隔几秒重试; "
                        "用 --list 看可用节点, 自带的 cam-server 可能占着设备\n");
    }

    // ---- 7. H.264 编码 + UDP 推流 + 告警录像缓冲 ----
    mb::VideoPipeline video;
    const bool want_h264 = !opt.no_h264 && cfg.path("stream.h264.enabled").boolean(true);
    if (want_h264) {
        const mb::VideoPipelineConfig vc =
            mb::VideoPipelineConfig::from_json(cfg, cam.started() ? cam.width() : cam_w,
                                               cam.started() ? cam.height() : cam_h, cam_fps);
        if (!video.start(vc, &err)) {
            fprintf(stderr, "[mb-capture] H.264 编码器启动失败, 推流/录像不可用: %s\n", err.c_str());
        } else {
            printf("[mb-capture] H.264 %dx%d %d kbps gop=%d -> udp://%s:%d%s\n", vc.enc.width,
                   vc.enc.height, vc.enc.bitrate_kbps, vc.enc.gop, vc.udp_host.c_str(),
                   vc.udp_port, vc.udp_enabled ? "" : " (UDP 已关)");
        }
    }

    // ---- 8. 事件引擎 ----
    mb::EventEngine events;
    bool events_ok = false;
    {
        const mb::EventEngineConfig ec = mb::EventEngineConfig::from_json(cfg);
        if (!events.start(ec, video.running() ? &video : nullptr, &err)) {
            fprintf(stderr, "[mb-capture] 事件引擎启动失败, 告警不落盘: %s\n", err.c_str());
        } else {
            events_ok = true;
            printf("[mb-capture] 事件: 去抖 %d 推理帧, 冷却 %.0fs, 存储 %s\n", ec.debounce_frames,
                   ec.cooldown_sec, ec.dir.c_str());
        }
    }

    // ---- 8.1 人员滞留联动 (扬声器 / 持续录像 / 滞留记录) ----
    // 滞留喊话和围栏警报共用一个播放器, 同一时刻只放一路
    mb::Speaker speaker;
    speaker.configure(mb::SpeakerConfig::from_json(cfg));
    mb::DwellGuard dwell;
    dwell.set_player(&speaker);
    dwell.configure(mb::DwellGuardConfig::from_json(cfg));
    if (events_ok) {
        dwell.set_clip_extender([&events](const std::string& id, uint64_t until) {
            return events.extend_clip(id, until);
        });
    }

    // ---- 8.2 入侵 / 电子围栏联动 (扬声器警报 / 人在区内持续录像) ----
    mb::ZoneAlarm zone_alarm;
    zone_alarm.set_player(&speaker);
    zone_alarm.configure(mb::ZoneAlarmConfig::from_json(cfg));
    if (events_ok) {
        zone_alarm.set_clip_extender([&events](const std::string& id, uint64_t until) {
            return events.extend_clip(id, until);
        });
    }

    // ---- 9. Web ----
    mb::MjpegHub hub;
    mb::StatusBoard status;
    // GET /ground: 主线程按 5Hz 发布定位快照 (地面目标/轨迹/区域统计/标定质量)
    mb::StatusBoard ground_board;
    std::atomic<bool> ground_reset{false};
    // 设备经纬度: 后台线程按 geo.providers 顺序走网络定位, 不占主循环
    mb::GeoLocator geo;
    geo.configure(mb::GeoParams::from_json(cfg));
    geo.start();
    std::atomic<bool> reload_requested{false};
    mb::WebContext web_ctx;
    mb::HttpServer http;
    const bool want_web = !opt.no_web && cfg.path("web.enabled").boolean(true);
    if (want_web) {
        mb::MjpegHub::Params hp;
        hp.width = cfg.path("web.stream_width").integer(1280);
        hp.height = cfg.path("web.stream_height").integer(720);
        hp.fps = cfg.path("web.stream_fps").integer(15);
        hp.quality = cfg.path("web.stream_jpeg_quality").integer(85);
        hp.use_rga = use_rga;
        hub.start(hp);

        web_ctx.config_path = opt.config;
        web_ctx.class_map = class_map;
        web_ctx.events = events_ok ? &events : nullptr;
        web_ctx.dwell = &dwell;
        web_ctx.video = video.running() ? &video : nullptr;
        web_ctx.hub = &hub;
        web_ctx.status = &status;
        web_ctx.activity = &activity;
        web_ctx.view = &view;
        web_ctx.rules = &rules;
        web_ctx.ground = &ground_board;
        web_ctx.ground_reset = &ground_reset;
        web_ctx.geo = &geo;
        web_ctx.reload_requested = &reload_requested;
        web_ctx.running = &g_running;
        const std::string listen = cfg.path("web.listen").str("0.0.0.0");
        const int port = cfg.path("web.port").integer(8080);
        if (!http.start(listen, port, cfg.path("web.max_clients").integer(8),
                        [&web_ctx](const mb::HttpRequest& req, mb::HttpConn& conn) {
                            mb::handle_web_request(&web_ctx, req, conn);
                        },
                        &err)) {
            fprintf(stderr, "[mb-capture] Web 服务启动失败: %s\n", err.c_str());
            hub.stop();
        } else {
            printf("[mb-capture] Web http://%s:%d/  (/stream /status /counts /config /events /ground /geo)\n",
                   listen.c_str(), port);
        }
    }
    fflush(stdout);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    // ---- 10. 主循环 ----
    // 姿态模型单次 59.5ms, 比检测贵 3 倍, 所以配置里单独给了 models.pose.fps。
    // 姿态跑得比检测慢是正常的: 跌倒是"人在框里躺了一段时间"的事件, 6fps 足够,
    // 而 min_trigger_frames 数的是姿态结果, 所以降频不影响判定语义, 只是慢一点。
    int pose_fps = cfg.path("models.pose.fps").integer(6);
    // 火焰/烟雾是持续现象, 3fps 足够 (fire n 模型单次约 10ms 级), 省 NPU。
    int fire_fps = cfg.path("models.fire.fps").integer(3);
    double infer_interval_ms = 0.0, pose_interval_ms = 0.0, fire_interval_ms = 0.0;
    // 采集侧软件抽帧的最小间隔: 摄像头不支持节能帧率时, 在解码之前丢帧, 省掉解码/OSD/编码。
    double frame_interval_ms = 0.0;
    auto apply_rates = [&]() {
        const bool eco = power.mode == PowerMode::Eco;
        const int fi = eco ? capped_fps(infer_fps, power.eco_infer_fps) : infer_fps;
        const int fp = eco ? capped_fps(pose_fps, power.eco_pose_fps) : pose_fps;
        const int ff = eco ? capped_fps(fire_fps, power.eco_fire_fps) : fire_fps;
        infer_interval_ms = fi > 0 ? 1000.0 / fi : 0.0;
        pose_interval_ms = fp > 0 ? 1000.0 / fp : 0.0;
        fire_interval_ms = ff > 0 ? 1000.0 / ff : 0.0;
        const int fc = want_cam_fps();
        frame_interval_ms = (eco && fc > 0 && fc < cam_fps) ? 1000.0 / fc : 0.0;
    };
    apply_rates();
    mb::PreprocessOptions popt;
    popt.use_rga = use_rga;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t_start = Clock::now();
    Clock::time_point last_infer = Clock::now() - std::chrono::seconds(10);
    Clock::time_point last_pose = Clock::now() - std::chrono::seconds(10);
    Clock::time_point last_fire = Clock::now() - std::chrono::seconds(10);
    Clock::time_point last_cfg_check = Clock::now();
    Clock::time_point last_publish = Clock::now() - std::chrono::seconds(10);
    int64_t infer_index = 0;
    int64_t frame_index = 0;
    Stats st;
    st.t_last = Clock::now();

    // OSD 上的告警横幅: 最近一条告警挂 3 秒
    std::string banner;
    unsigned int banner_color = 0xFFFF0000;
    uint64_t banner_until = 0;
    mb::AlarmHighlights highlights;

    std::vector<mb::Track> tracks;
    std::vector<mb::Detection> dets;
    mb::ImageRGB img;
    std::shared_ptr<mb::ImageRGB> infer_bufs[2];
    int infer_buf_idx = 0;
    std::string last_infer_error;
    Clock::time_point last_infer_error_at = Clock::now() - std::chrono::seconds(60);

    mb::InferWorker worker;
    worker.start(&det, &pose, &fire_model);

    uint64_t latest_ts = 0;
    uint64_t next_ground_ms = 0;
    // 最近一次检测结果对应的采集时间戳。推理卡住/报错时轨迹不再更新, 拿旧位置继续跑规则会让
    // "已经走了的人"一直在区内计时, 超过这个时长就当作没有目标。
    uint64_t last_det_ts = 0;

    // 热加载: 只换规则/跌倒/跟踪/事件/推理阈值。摄像头、模型、端口、编码参数改了要重启。
    auto reload_config = [&]() {
        mb::Json next;
        std::string rerr;
        if (!mb::Json::parse_file(opt.config, &next, &rerr)) {
            fprintf(stderr, "[mb-capture] 热加载: 配置解析失败, 保持旧配置: %s\n", rerr.c_str());
            return;
        }
        mb::RuleEngine next_rules;
        if (!next_rules.load(next, class_map, &rerr)) {
            fprintf(stderr, "[mb-capture] 热加载: 规则配置错误, 保持旧配置: %s\n", rerr.c_str());
            return;
        }
        // 区域/线重画过 (APP 按当前画面保存): 当前画面就是新的参考, 之前的偏移清零。
        // 先套上偏移再 adopt_state, 否则它会把对齐过的区域当成改了形状而重新计时。
        view.configure(mb::ViewGuard::Params::from_json(next));
        if (!next_rules.same_geometry(rules) && view.params().enabled)
            view.request_reference("区域已重画");
        view.set_ignore(next_rules.base_ignore_polygons());
        next_rules.set_view_transform(view.applied());
        next_rules.adopt_state(rules);
        rules = next_rules;
        fall = mb::FallDetector(fall_params_from(next));
        fall_enabled = next.path("features.fall.enabled").boolean(false);
        pp.conf_threshold = (float)next.path("features.fall.pose_conf").number(0.35);
        ensure_pose(next);
        ensure_fire(next);
        fire = mb::FireDetector(fire_params_from(next), fire_labels);
        fire_enabled = next.path("features.fire.enabled").boolean(false);
        activity.configure(mb::ActivityMap::Params::from_json(next), class_map);
        counts.configure(mb::CountStore::Params::from_json(next));

        mb::TrackerParams ntp = tracker.params();
        ntp.enabled = next.path("track.enabled").boolean(true);
        ntp.track_buffer = next.path("track.track_buffer").integer(90);
        tracker.set_params(ntp);
        tp = ntp;

        const bool next_det = detector_needed();
        if (next_det != det_running) {
            printf("[mb-capture] 检测模型 %s\n", next_det ? "恢复运行" : "停用 (没有功能需要)");
            if (!next_det) {
                // 不清掉的话最后一帧的框会一直画在画面上
                tracker.reset();
                highlights.clear();
                tracks.clear();
                dets.clear();
                last_det_ts = 0;
            }
            det_running = next_det;
        }

        conf = (float)next.path("infer.conf_threshold").number(0.25);
        nms = (float)next.path("infer.nms_threshold").number(0.45);
        infer_fps = next.path("infer.infer_fps").integer(12);
        pose_fps = next.path("models.pose.fps").integer(6);
        fire_fps = next.path("models.fire.fps").integer(3);
        const PowerMode old_mode = power.mode;
        power = power_params_from(next);
        apply_rates();
        if (power.mode != old_mode)
            printf("[mb-capture] 功耗模式 %s -> %s\n", power_mode_name(old_mode),
                   power_mode_name(power.mode));

        if (events_ok) events.reconfigure(mb::EventEngineConfig::from_json(next));
        speaker.configure(mb::SpeakerConfig::from_json(next));
        dwell.configure(mb::DwellGuardConfig::from_json(next));
        zone_alarm.configure(mb::ZoneAlarmConfig::from_json(next));
        geo.configure(mb::GeoParams::from_json(next));

        const std::vector<std::string> need = mb::restart_required_keys(cfg, next);
        cfg = next;
        printf("[mb-capture] 配置已热加载\n");
        print_features();
        for (const std::string& k : need)
            printf("[mb-capture]   注意: %s 的改动需要重启 mb-capture 才生效\n", k.c_str());
        fflush(stdout);
    };

    StatSnapshot snap;
    int read_timeouts = 0;
    Clock::time_point next_cam_retry = Clock::now();
    int cam_backoff_ms = 1000;

    auto publish_status = [&]() {
        if (!want_web || Clock::now() - last_publish < std::chrono::seconds(1)) return;
        last_publish = Clock::now();
        mb::Json s = mb::Json::object();
        s.set("uptime_sec", mb::Json::number_value(
                                std::chrono::duration<double>(Clock::now() - t_start).count()));
        s.set("frame", mb::Json::number_value((double)frame_index));
        s.set("capture_fps", mb::Json::number_value(cam.started() ? snap.fps : 0.0));
        s.set("ai_ms", mb::Json::number_value(snap.ai_ms));
        s.set("decode_ms", mb::Json::number_value(snap.decode_ms));
        s.set("loop_ms", mb::Json::number_value(snap.loop_ms));
        s.set("ai_consume_ms", mb::Json::number_value(snap.ai_consume_ms));
        s.set("infer_copy_ms", mb::Json::number_value(snap.infer_copy_ms));
        s.set("rules_ms", mb::Json::number_value(snap.rules_ms));
        s.set("osd_ms", mb::Json::number_value(snap.osd_ms));
        s.set("encode_fill_ms", mb::Json::number_value(snap.encode_fill_ms));
        s.set("osd_shapes_ms", mb::Json::number_value(snap.osd_shapes_ms));
        s.set("osd_objects_ms", mb::Json::number_value(snap.osd_objects_ms));
        s.set("osd_text_ms", mb::Json::number_value(snap.osd_text_ms));
        s.set("osd_bev_ms", mb::Json::number_value(snap.osd_bev_ms));
        s.set("latency_ms", mb::Json::number_value(snap.latency_ms));
        s.set("detections", mb::Json::number_value((double)dets.size()));
        s.set("detector_running", mb::Json::bool_value(det_running));
        int live = 0;
        for (const mb::Track& t : tracks)
            if (t.confirmed && t.lost == 0) ++live;
        s.set("tracks", mb::Json::number_value(live));
        mb::Json camj = mb::Json::object();
        camj.set("connected", mb::Json::bool_value(cam.started()));
        camj.set("device", mb::Json::string_value(cam.started() ? cam.device() : dev));
        camj.set("width", mb::Json::number_value(cam.width()));
        camj.set("height", mb::Json::number_value(cam.height()));
        camj.set("format", mb::Json::string_value(cam.pixel_format_name()));
        camj.set("jpeg_decoder", mb::Json::string_value(cam.jpeg_backend()));
        camj.set("jpeg_hw_failures", mb::Json::number_value((double)cam.hardware_jpeg_failures()));
        camj.set("reconnects", mb::Json::number_value(cam_reconnects));
        camj.set("stale_skipped", mb::Json::number_value((double)cam.stale_skipped()));
        camj.set("error_frames", mb::Json::number_value((double)cam.error_frames()));
        if (!cam_error.empty()) camj.set("error", mb::Json::string_value(cam_error));
        s.set("camera", camj);
        mb::Json pw = mb::Json::object();
        pw.set("mode", mb::Json::string_value(power_mode_name(power.mode)));
        pw.set("camera_on", mb::Json::bool_value(cam.started()));
        pw.set("capture_fps_limit", mb::Json::number_value(
                                        power.mode == PowerMode::Standby ? 0 : want_cam_fps()));
        pw.set("infer_fps",
               mb::Json::number_value(infer_interval_ms > 0.0 ? 1000.0 / infer_interval_ms : 0.0));
        s.set("power", pw);
        mb::Json feats = mb::Json::object();
        for (const mb::RuleEngine::Feature& f : rules.features())
            feats.set(f.key, mb::Json::bool_value(f.enabled || feats[f.key].boolean(false)));
        feats.set("fall", mb::Json::bool_value(fall_enabled && pose_ok));
        feats.set("fire", mb::Json::bool_value(fire_enabled && fire_ok));
        s.set("features", feats);
        // 入侵/围栏的布防状态 (撤防或不在布防时段时为 false)
        // armed_regions: 各区域各自的布防状态 (每个区域可以有自己的布防时段)
        mb::Json armed = mb::Json::object();
        mb::Json armed_regions = mb::Json::object();
        for (const mb::RuleEngine::Feature& f : rules.features()) {
            if (f.enabled && f.rule == "zone" && f.trigger != "stay") {
                armed.set(f.key, mb::Json::bool_value(rules.armed(f.key)));
                if (f.region_id.empty()) continue;
                mb::Json per = armed_regions[f.key].is_object() ? armed_regions[f.key]
                                                                 : mb::Json::object();
                per.set(f.region_id, mb::Json::bool_value(rules.armed(f.key, f.region_id)));
                armed_regions.set(f.key, per);
            }
        }
        s.set("armed", armed);
        s.set("armed_regions", armed_regions);
        s.set("zone_alarm", zone_alarm.summary());
        mb::Json lc = mb::Json::object();
        for (const auto& kv : rules.line_counts()) {
            mb::Json c = mb::Json::object();
            c.set("in", mb::Json::number_value(kv.second.in));
            c.set("out", mb::Json::number_value(kv.second.out));
            c.set("total", mb::Json::number_value(kv.second.total()));
            lc.set(kv.first, c);
        }
        s.set("line_counts", lc);
        // 画面内此刻的人数 (已确认、未丢失, 和 OSD 的 NOW 一致)
        s.set("people_in_frame", mb::Json::number_value(rules.people_in_frame()));
        s.set("dwell", dwell.summary());
        s.set("fall_down", mb::Json::bool_value(fall.is_down()));
        s.set("pose_loaded", mb::Json::bool_value(pose_ok));
        s.set("fire_loaded", mb::Json::bool_value(fire_ok));
        mb::Json firej = mb::Json::object();
        firej.set("fire_active", mb::Json::bool_value(fire.fire_active()));
        firej.set("smoke_active", mb::Json::bool_value(fire.smoke_active()));
        firej.set("fire_seen", mb::Json::bool_value(fire.fire_seen()));
        firej.set("smoke_seen", mb::Json::bool_value(fire.smoke_seen()));
        s.set("fire", firej);
        mb::Json actj = mb::Json::object();
        actj.set("enabled", mb::Json::bool_value(activity.params().enabled));
        actj.set("learning", mb::Json::bool_value(activity.params().learning));
        s.set("activity", actj);
        s.set("view", view.json());
        s.set("geo", geo.json());
        s.set("config", mb::Json::string_value(opt.config));
        s.set("app_language", mb::Json::string_value(cfg.path("app.language").str("zh_CN")));
        status.publish(s);
    };

    auto drop_camera = [&](const std::string& why) {
        capture_watchdog_enabled.store(false, std::memory_order_relaxed);
        fprintf(stderr, "[mb-capture] 摄像头断开: %s, 稍后重连\n", why.c_str());
        cam.close();
        cam_error = why;
        tracks.clear();
        dets.clear();
        highlights.clear();
        cam_backoff_ms = 1000;
        next_cam_retry = Clock::now() + std::chrono::milliseconds(cam_backoff_ms);
        read_timeouts = 0;
    };

    bool in_standby = false;
    bool cam_resuming = false;  // 刚退出待机: 下一次打开摄像头不算 "断线重连"
    uint64_t last_accept_ts = 0;

    // 进入待机: 释放摄像头, 收尾进行中的录像/滞留会话, 让 NPU/编码器/CPU 都闲下来。
    // 模型保留在内存里 (NPU 空闲时由驱动自己降频/关时钟), 这样恢复是秒级的。
    auto enter_standby = [&]() {
        in_standby = true;
        capture_watchdog_enabled.store(false, std::memory_order_relaxed);
        cam.close();
        cam_error.clear();
        tracks.clear();
        dets.clear();
        last_det_ts = 0;
        last_accept_ts = 0;
        read_timeouts = 0;
        dwell.update({}, latest_ts);
        dwell.finish_missing();
        zone_alarm.clear();
        speaker.stop();
        highlights.clear();
        if (video.running()) video.flush_clips();
        fall = mb::FallDetector(fall_params_from(cfg));
        fire = mb::FireDetector(fire_params_from(cfg), fire_labels);
        snap = StatSnapshot();
        st = Stats();
        st.t_last = Clock::now();
        printf("[mb-capture] 进入待机: 摄像头已关闭, 停止采集/推理/编码\n");
        fflush(stdout);
    };

    // RGA/MPP ioctls have no userspace timeout. Keep the watchdog independent of
    // the capture thread so a wedged hardware request can still restart the process.
    std::thread capture_watchdog([&]() {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!capture_watchdog_enabled.load(std::memory_order_relaxed)) continue;
            const uint64_t now_ms = steady_ms();
            const uint64_t last_ms = capture_heartbeat_ms.load(std::memory_order_relaxed);
            if (now_ms > last_ms && now_ms - last_ms >= 8000) {
                fprintf(stderr,
                        "[mb-capture] 摄像头已连接但 8 秒没有完成一帧, RGA/MPP/USB 可能卡死; "
                        "退出进程交给 systemd 恢复\n");
                fflush(stderr);
                _exit(42);
            }
        }
    });

    while (g_running) {
        if (opt.frames > 0 && frame_index >= opt.frames) break;

        // ---- 配置热加载 (APP 改文件 / PUT /config) ----
        if (reload_requested.exchange(false) ||
            Clock::now() - last_cfg_check >= std::chrono::seconds(1)) {
            last_cfg_check = Clock::now();
            const FileStamp now_stamp = stamp_of(opt.config);
            if (now_stamp != cfg_stamp) {
                cfg_stamp = now_stamp;
                reload_config();
            }
        }

        // ---- 待机: 只保留 Web/配置/事件服务, 主循环低频醒来看配置 ----
        if (power.mode == PowerMode::Standby) {
            if (!in_standby) enter_standby();
            mb::InferResult stale;
            worker.poll(&stale);  // 待机前派出去的那次推理结果直接丢掉
            publish_status();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (in_standby) {
            in_standby = false;
            cam_resuming = true;
            cam_backoff_ms = 1000;
            next_cam_retry = Clock::now();
            printf("[mb-capture] 退出待机 (%s), 正在打开摄像头\n", power_mode_name(power.mode));
            fflush(stdout);
        }

        // normal <-> eco 切换后帧率不同: 按新帧率重新打开摄像头
        if (cam.started() && cam_open_fps != want_cam_fps()) {
            printf("[mb-capture] 采集帧率 %d -> %d, 重新打开摄像头\n", cam_open_fps,
                   want_cam_fps());
            cam.close();
            cam_resuming = true;
            next_cam_retry = Clock::now();
            last_accept_ts = 0;
        }

        // ---- 摄像头不在: 退避重连, 期间照常发布状态 ----
        if (!cam.started()) {
            if (Clock::now() >= next_cam_retry) {
                if (open_camera()) {
                    cam_backoff_ms = 1000;
                    if (cam_resuming) {
                        printf("[mb-capture] 摄像头已打开\n");
                    } else {
                        ++cam_reconnects;
                        printf("[mb-capture] 摄像头已恢复 (第 %d 次重连)\n", cam_reconnects);
                    }
                    cam_resuming = false;
                    fflush(stdout);
                } else {
                    cam_backoff_ms = std::min(cam_backoff_ms * 2, 5000);
                    next_cam_retry = Clock::now() + std::chrono::milliseconds(cam_backoff_ms);
                }
            }
            publish_status();
            if (!cam.started()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        mb::VideoFrame vf;
        std::string ferr;
        if (!cam.read(&vf, 1000, &ferr)) {
            if (!ferr.empty()) {
                drop_camera(ferr);
            } else if (++read_timeouts >= 3) {
                // 有的 UVC 掉线时不报错, 只是不再出帧
                drop_camera("连续 3 秒没有新帧");
            }
            publish_status();
            continue;
        }
        read_timeouts = 0;
        if (frame_interval_ms > 0.0) {
            // 允许 1/4 周期的抖动, 否则 30->10fps 时会因为时间戳毛刺变成 7.5fps
            const uint64_t iv = (uint64_t)frame_interval_ms;
            if (last_accept_ts != 0 && vf.timestamp_ms + iv / 4 < last_accept_ts + iv) {
                publish_status();
                continue;
            }
            last_accept_ts = vf.timestamp_ms;
        }
        ++frame_index;
        latest_ts = vf.timestamp_ms;

        const Clock::time_point t_frame = Clock::now();
        if (!cam.decode_frame(vf, &img, &ferr)) {
            fprintf(stderr, "[mb-capture] 解码失败: %s\n", ferr.c_str());
            continue;
        }
        st.decode_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_frame).count();

        const Clock::time_point now = Clock::now();

        // --- 取推理结果 (推理在 InferWorker 线程里跑, 不阻塞视频通路) ---
        const Clock::time_point t_ai_consume = Clock::now();
        mb::InferResult ir;
        const bool got_result = worker.poll(&ir);
        if (got_result) {
            if (!ir.error.empty()) {
                // 同一个错误 (如模型坏了) 每帧都会出, 10 秒内只打一次
                if (ir.error != last_infer_error || now - last_infer_error_at > std::chrono::seconds(10)) {
                    fprintf(stderr, "[mb-capture] %s\n", ir.error.c_str());
                    last_infer_error = ir.error;
                    last_infer_error_at = now;
                }
            }
            st.ai_total_ms += ir.ai_ms;
            st.infer_ms = ir.ai_ms;
            ++st.infers;

            if (ir.has_det) {
                last_det_ts = ir.ts_ms;
                dets = std::move(ir.dets);
                // --- 跟踪 ---
                tracks.clear();
                if (tp.enabled) {
                    const std::vector<mb::Track>& cur = tracker.update(dets, infer_index);
                    tracks = cur;
                } else {
                    // 不开跟踪时, 把每个检测当一个只活一帧的轨迹, 规则引擎照样能跑
                    for (size_t i = 0; i < dets.size(); ++i) {
                        mb::Track t;
                        t.id = (int)i + 1;
                        t.cls_id = dets[i].cls_id;
                        t.score = dets[i].score;
                        t.x1 = dets[i].x1;
                        t.y1 = dets[i].y1;
                        t.x2 = dets[i].x2;
                        t.y2 = dets[i].y2;
                        t.confirmed = true;
                        tracks.push_back(t);
                    }
                }
                ++infer_index;
            }

            // --- 跌倒 ---
            // 跌倒的帧计数语义绑在"一次姿态推理"上 (min_trigger_frames), 所以只在拿到
            // 新姿态结果时更新, 不能每采集帧喂同一份旧姿态, 否则计数会翻倍。
            if (ir.has_pose && fall_enabled) {
                const std::vector<mb::RuleEvent> fev =
                    fall.update(ir.poses, tracks, ir.img_w, ir.img_h, ir.ts_ms);
                if (opt.fall_debug) {
                    printf("[fall] ts=%llu poses=%zu count=%d/%d down=%d",
                           (unsigned long long)ir.ts_ms, ir.poses.size(), fall.fallen_count(),
                           fall.params().min_trigger_frames, fall.is_down() ? 1 : 0);
                    for (const auto& kv : fall.features()) {
                        const mb::FallFeatures& f = kv.second;
                        printf(" | id=%d vis=%.2f ang=%.0f score=%.1f ratio=%d posture=%d vel=%d"
                               " drop=%.0f",
                               kv.first, f.kpt_vis_ratio, f.torso_angle, f.fall_score,
                               f.ratio_ok ? 1 : 0, f.posture ? 1 : 0, f.vel_ok ? 1 : 0,
                               f.hip_drop);
                    }
                    printf("\n");
                    fflush(stdout);
                }
                print_events(fev);
                // 跌倒自带 min_trigger_frames 的时序确认, 不再走事件引擎的去抖
                if (events_ok) events.push(fev, true);
            }

            // --- 火焰烟雾 ---
            // 和跌倒一样只在拿到新的火焰结果时更新; 自带时序投票, 不再走事件引擎去抖。
            if (ir.has_fire && fire_enabled) {
                const std::vector<mb::RuleEvent> fire_ev =
                    fire.update(ir.fire_dets, ir.img_w, ir.img_h, ir.ts_ms);
                print_events(fire_ev);
                if (events_ok) events.push(fire_ev, true);
            }
        }

        st.ai_consume_ms +=
            std::chrono::duration<double, std::milli>(Clock::now() - t_ai_consume).count();

        // --- 派推理任务: 线程空闲且到点了才派, 慢了就少推几帧, 绝不积压 ---
        // 姿态自己有一条更慢的节拍 (models.pose.fps, 默认 6), 搭在检测任务上一起跑。
        const double since_infer =
            std::chrono::duration<double, std::milli>(now - last_infer).count();
        const double since_pose =
            std::chrono::duration<double, std::milli>(now - last_pose).count();
        const double since_fire =
            std::chrono::duration<double, std::milli>(now - last_fire).count();
        const bool want_pose = pose_ok && fall_enabled && since_pose >= pose_interval_ms;
        const bool want_fire = fire_ok && fire_enabled && since_fire >= fire_interval_ms;
        if (!worker.busy() && (det_running || want_pose || want_fire) &&
            ((infer_interval_ms <= 0.0) || (since_infer >= infer_interval_ms))) {
            mb::InferJob job;
            // 双缓冲轮换: 推理线程手里那份不动, 拷到另一份里 (6MB memcpy 约 2ms)
            std::shared_ptr<mb::ImageRGB>& buf = infer_bufs[infer_buf_idx];
            infer_buf_idx ^= 1;
            if (!buf || buf.use_count() > 1) buf = std::make_shared<mb::ImageRGB>();
            const Clock::time_point t_infer_copy = Clock::now();
            *buf = img;
            st.infer_copy_ms +=
                std::chrono::duration<double, std::milli>(Clock::now() - t_infer_copy).count();
            job.img = buf;
            job.ts_ms = vf.timestamp_ms;
            job.yolo.conf_threshold = conf;
            job.yolo.nms_threshold = nms;
            job.pose = pp;
            job.pose_roi = proi;
            job.pre = popt;
            job.run_det = det_running;
            job.run_pose = want_pose;
            job.run_fire = want_fire;
            if (job.run_fire) {
                // 后处理只按两类里较低的门槛出框, 每类的门槛和面积过滤在 FireDetector 里做
                const mb::FireParams& fpar = fire.params();
                job.fire.conf_threshold = std::min(fpar.fire.conf, fpar.smoke.conf);
                job.fire.nms_threshold = nms;
            }
            if (job.run_pose) {
                for (const mb::Track& t : tracks) {
                    if (t.cls_id != proi.person_cls_id) continue;
                    mb::Detection h;
                    h.cls_id = t.cls_id;
                    h.score = t.score;
                    h.x1 = t.x1;
                    h.y1 = t.y1;
                    h.x2 = t.x2;
                    h.y2 = t.y2;
                    job.pose_hints.push_back(h);
                }
            }
            const bool job_runs_fire = job.run_fire;
            if (worker.submit(std::move(job))) {
                last_infer = now;
                if (want_pose) last_pose = now;
                if (job_runs_fire) last_fire = now;
            }
        }

        // --- 规则: 每采集帧都跑 ---
        // 轨迹只在推理结果回来时变化, 但滞留计时/越线/定位上报要按采集时间戳走, 抽帧跑会让
        // 计时精度跟着 infer_fps 掉。这里用最近一次推理得到的轨迹, 位置未变时不会
        // 产生新的越线/进入事件, 所以多调用是安全的。
        const uint64_t stale_ms =
            (uint64_t)std::max(1000.0, 3.0 * (infer_interval_ms > 0.0 ? infer_interval_ms : 100.0));
        const bool tracks_fresh =
            last_det_ts != 0 && vf.timestamp_ms <= last_det_ts + stale_ms;
        static const std::vector<mb::Track> kNoTracks;
        const std::vector<mb::Track>& rule_tracks = tracks_fresh ? tracks : kNoTracks;
        if (ground_reset.exchange(false)) rules.reset_ground_stats();
        const Clock::time_point t_rules = Clock::now();
        const std::vector<mb::RuleEvent> rule_events =
            rules.update(rule_tracks, img.width, img.height, vf.timestamp_ms);
        st.rules_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_rules).count();
        print_events(rule_events);
        if (want_web && steady_ms() >= next_ground_ms) {
            next_ground_ms = steady_ms() + 200;
            mb::Json gj = rules.locator().to_json(vf.timestamp_ms);
            gj.set("geo", geo.json());
            ground_board.publish(gj);
        }
        if (events_ok) events.push(rule_events, false);
        activity.update(rule_tracks, img.width, img.height, vf.timestamp_ms, &rules);
        // 越线计数按小时累计: 每帧把累计值交给 CountStore, 差值攒到点再落盘
        counts.poll(rules.line_counts());

        // --- 滞留联动: 扬声器 / 续录像 / 会话记录 ---
        const std::vector<mb::RuleEngine::Dweller> dwellers = rules.dwellers();
        dwell.update(dwellers, vf.timestamp_ms);

        // --- 事件引擎: 去抖 / 冷却 / 分级 ---
        const uint64_t now_ms = steady_ms();
        if (events_ok) {
            const std::vector<mb::EventRecord> fired =
                events.update(rule_tracks, got_result && ir.has_det, vf.timestamp_ms,
                              [&rules](const mb::RuleEvent& ev) {
                                  switch (rules.liveness(ev)) {
                                      case mb::RuleEngine::Liveness::Active:
                                          return mb::EventEngine::Liveness::Active;
                                      case mb::RuleEngine::Liveness::Occluded:
                                          return mb::EventEngine::Liveness::Occluded;
                                      case mb::RuleEngine::Liveness::Gone:
                                          return mb::EventEngine::Liveness::Gone;
                                      default:
                                          return mb::EventEngine::Liveness::Unknown;
                                  }
                              });
            for (const mb::EventRecord& r : fired) {
                const mb::AlarmTone tone = r.level == mb::EventLevel::Critical
                    ? mb::AlarmTone::Critical : r.level == mb::EventLevel::Warning
                    ? mb::AlarmTone::Warning : mb::AlarmTone::Silent;
                if (r.ev.session > 0)
                    dwell.on_fired(r.ev.session, r.id, r.ev.kind, r.clip_state == "recording",
                                   r.ev.ts_ms, tone);
                zone_alarm.on_fired(r.ev, r.id, r.clip_state == "recording", tone);
                if (r.level == mb::EventLevel::Info) continue;  // Info 上面 print_events 已打过
                printf("[event] %-8s %s %s/%s track=%d %s %s%s\n", mb::level_name(r.level),
                       r.id.c_str(), r.ev.feature.c_str(), r.ev.kind.c_str(), r.ev.track_id,
                       r.ev.label.c_str(), r.ev.name.c_str(),
                       r.suppressed ? (" (冷却期合并 " + std::to_string(r.suppressed) + " 条)").c_str()
                                    : "");
                if (r.level != mb::EventLevel::Info) {
                    char b[160];
                    snprintf(b, sizeof(b), "%s %s/%s id%d %s",
                             r.level == mb::EventLevel::Critical ? "!! CRITICAL" : "! ALARM",
                             r.ev.feature.c_str(), r.ev.kind.c_str(), r.ev.track_id,
                             r.ev.label.c_str());
                    banner = b;
                    banner_color = r.level == mb::EventLevel::Critical ? 0xFFFF0000 : 0xFFFFA500;
                    banner_until = now_ms + 3000;
                }
            }
            if (!fired.empty()) fflush(stdout);
        }
        dwell.finish_missing();
        zone_alarm.update(vf.timestamp_ms,
                          [&rules](const mb::RuleEvent& ev) { return rules.liveness(ev); });
        if (events_ok) highlights.update(events.highlight_sources(), now_ms);

        // ---- OSD: 推流/录像/快照/存图共用同一张标注图 ----
        // APP 画区域要的是干净画面: 在画 OSD 之前拷一份缩小的出去
        if (want_web && hub.raw_wanted()) hub.offer_raw(img);
        // 画面对齐也要干净画面: 每 15 秒左右缩一张 320 宽灰度图 (约 2ms), 计算在后台线程
        if (view.wants_frame(now_ms)) {
            mb::GrayImage g;
            const int gh = std::max(36, (int)std::lround((double)mb::ViewGuard::kWidth * img.height /
                                                         std::max(1, img.width)));
            mb::downscale_gray(img, mb::ViewGuard::kWidth, gh, &g);
            std::vector<mb::NormBox> boxes;
            for (const mb::Track& t : tracks) {
                boxes.push_back(mb::NormBox{t.x1 / img.width, t.y1 / img.height, t.x2 / img.width,
                                            t.y2 / img.height});
            }
            view.offer(std::move(g), std::move(boxes), now_ms);
        }
        {
            mb::ViewGuard::Change vc;
            if (view.take_change(&vc)) {
                rules.set_view_transform(vc.applied);
                if (vc.warp_heat) activity.warp(vc.heat_delta);
            }
        }
        {
            std::string aerr;
            if (!activity.maybe_save(now_ms, false, &aerr))
                fprintf(stderr, "[mb-capture] 活动热图保存失败: %s\n", aerr.c_str());
            std::string cerr;
            if (!counts.maybe_flush(now_ms, false, &cerr))
                fprintf(stderr, "[mb-capture] 越线计数保存失败: %s\n", cerr.c_str());
        }
        const bool hub_wants = hub.wants_frame(now_ms);
        const bool need_osd = video.running() || hub_wants || events_ok || !opt.save_dir.empty();
        const Clock::time_point t_osd = Clock::now();
        if (need_osd) {
            image_buffer_t osd = wrap_osd(&img);
            const Clock::time_point t_osd_shapes = Clock::now();

            // 区域和线: 只画启用的功能用到的 (加忽略区), 各区域独立配色, 画在框下面。
            // hide_region_overlays 只隐藏静态叠加, 不改变规则、计数或告警高亮。
            if (!cfg.path("display.hide_region_overlays").boolean(false)) {
                const double fill = std::max(
                    0.0, std::min(1.0, cfg.path("display.region_alpha").number(0.25)));
                std::vector<mb::OverlayLayer> layers;
                std::vector<std::pair<std::vector<mb::Point>, unsigned int>> outlines;
                for (const mb::NamedPolygon& r : rules.regions()) {
                    if (!rules.region_active(r)) continue;
                    const unsigned int c = rules.region_color(r);
                    mb::OverlayLayer l;
                    l.poly = rules.region_pixels(r, img.width, img.height);
                    l.r = (uint8_t)((c >> 16) & 0xFF);
                    l.g = (uint8_t)((c >> 8) & 0xFF);
                    l.b = (uint8_t)(c & 0xFF);
                    // 地面标定只是坐标参考, 淡一点, 不要盖住画面
                    l.alpha = (int)(fill * (r.role == "ground" ? 128.0 : 256.0) + 0.5);
                    outlines.emplace_back(l.poly, c);
                    layers.push_back(std::move(l));
                }
                mb::fill_regions(&img, layers);
                for (const auto& o : outlines) draw_polyline(&osd, o.first, o.second, 2);
                for (const mb::NamedLine& l : rules.lines()) {
                    if (!rules.line_active(l)) continue;
                    mb::Point a, b;
                    rules.line_pixels(l, img.width, img.height, &a, &b);
                    draw_count_line(&osd, a, b, 0xFFFF00FF);
                }
            }
            for (const mb::NamedPolygon& r : rules.regions()) {
                if (!rules.region_active(r) || !highlights.region(r.id)) continue;
                const auto poly = rules.region_pixels(r, img.width, img.height);
                const int thickness = mb::alarm_thickness(&osd);
                draw_polyline(&osd, poly, COLOR_BLACK, thickness + 8);
                draw_polyline(&osd, poly, COLOR_WHITE, thickness + 4);
                draw_polyline(&osd, poly, COLOR_RED, thickness);
            }
            st.osd_shapes_ms +=
                std::chrono::duration<double, std::milli>(Clock::now() - t_osd_shapes).count();
            const Clock::time_point t_osd_objects = Clock::now();

            char text[160];
            for (const mb::Track& t : tracks) {
                if (!t.confirmed || t.lost != 0) continue;
                // 只画启用功能关心的类别; 跌倒开着时人框也要画
                if (!rules.shows_class(t.cls_id) &&
                    !(fall_enabled && t.cls_id == proi.person_cls_id))
                    continue;
                const char* name = (t.cls_id >= 0 && t.cls_id < (int)labels.size())
                                       ? labels[(size_t)t.cls_id].c_str()
                                       : "cls?";
                const unsigned int color = color_for_class(t.cls_id);
                const int x1 = (int)t.x1, y1 = (int)t.y1;
                const int w = (int)t.width(), h = (int)t.height();
                if (w <= 1 || h <= 1) continue;
                const mb::RuleEngine::Dweller* dw = nullptr;
                for (const mb::RuleEngine::Dweller& d : dwellers) {
                    if (d.track_id == t.id) dw = &d;
                }
                if (dw && dw->level > 0) {
                    // 已滞留: 红色粗框 + 停留时长 (OSD 的 Cantarell 字库只有 ASCII)
                    const unsigned int c = 0xFFFF0000;
                    draw_rectangle(&osd, x1, y1, w, h, c, dw->level >= 2 ? 6 : 4);
                    snprintf(text, sizeof(text), "id%d %s %s %.0fs", t.id, name,
                             dw->level >= 2 ? "STAY!!" : "STAY", dw->seconds);
                    int ty = y1 - 18;
                    if (ty < 0) ty = y1 + 2;
                    draw_text(&osd, text, x1, ty, c, 10);
                    continue;
                }
                draw_rectangle(&osd, x1, y1, w, h, color, 2);
                if (dw) {
                    // 区内计时中: 显示 已停留/门限
                    snprintf(text, sizeof(text), "id%d %s %.0f/%.0fs", t.id, name, dw->seconds,
                             dw->dwell_sec);
                } else {
                    snprintf(text, sizeof(text), "id%d %s %.0f%%", t.id, name, t.score * 100.0f);
                }
                int ty = y1 - 18;
                if (ty < 0) ty = y1 + 2;
                draw_text(&osd, text, x1, ty, color, 10);
            }

            // 火焰/烟雾框: 火焰红、烟雾灰, 已确认告警的画粗线
            if (fire_enabled && fire_ok) {
                for (const mb::Detection& d : fire.visible()) {
                    const bool is_fire = d.cls_id == fire.fire_cls();
                    const unsigned int color = is_fire ? 0xFFFF3000 : 0xFFB0B0B0;
                    const bool confirmed = is_fire ? fire.fire_active() : fire.smoke_active();
                    const int w = (int)(d.x2 - d.x1), h = (int)(d.y2 - d.y1);
                    if (w <= 1 || h <= 1) continue;
                    draw_rectangle(&osd, (int)d.x1, (int)d.y1, w, h, color, confirmed ? 4 : 2);
                    snprintf(text, sizeof(text), "%s %.0f%%", is_fire ? "fire" : "smoke",
                             d.score * 100.0f);
                    int ty = (int)d.y1 - 18;
                    if (ty < 0) ty = (int)d.y1 + 2;
                    draw_text(&osd, text, (int)d.x1, ty, color, 10);
                }
            }

            // 最后叠加触发源, 避免普通框/区域线盖住告警描边。
            for (const mb::Track& t : rule_tracks) {
                if (!t.confirmed || t.lost != 0 || !highlights.track(t.id)) continue;
                if (!rules.shows_class(t.cls_id) &&
                    !(fall_enabled && t.cls_id == proi.person_cls_id)) continue;
                if (t.width() <= 1 || t.height() <= 1) continue;
                mb::draw_alarm_box(&osd, (int)t.x1, (int)t.y1,
                                   (int)t.width(), (int)t.height());
            }
            if (fire_enabled && fire_ok) {
                const auto& visible = fire.visible();
                for (size_t i = 0; i < visible.size(); ++i) {
                    const mb::Detection& d = visible[i];
                    if (!highlights.fire(i, visible)) continue;
                    const int w = (int)(d.x2 - d.x1), h = (int)(d.y2 - d.y1);
                    if (w > 1 && h > 1)
                        mb::draw_alarm_box(&osd, (int)d.x1, (int)d.y1, w, h);
                }
            }
            st.osd_objects_ms +=
                std::chrono::duration<double, std::milli>(Clock::now() - t_osd_objects).count();
            const Clock::time_point t_osd_text = Clock::now();
            int alert_y = 8;
            if (cfg.path("display.show_metrics").boolean(true))
                alert_y += mb::draw_diagnostics(&osd, (long long)frame_index, dets.size(),
                                                tracks.size(), st.infer_ms);
            // Safety alerts remain visible even when diagnostics are hidden.
            snprintf(text, sizeof(text), "%s%s%s",
                     fall.is_down() ? "FALL-DOWN  " : "", fire.fire_active() ? "FIRE  " : "",
                     fire.smoke_active() ? "SMOKE" : "");
            if (text[0]) {
                draw_text(&osd, text, 8, alert_y, 0xFFFFA500, 12);
                alert_y += 32;
            }
            // 告警横幅仍留在左上角, 放在诊断和常驻安全提示之后。
            if (now_ms < banner_until && !banner.empty())
                draw_text(&osd, banner.c_str(), 8, alert_y, banner_color, 16);

            // 人数统计直接画进视频, 和右下角时间一样使用高对比黄色 + 黑色描边。
            // 常见的一条计数线只占一行; 多条线时 NOW 单独占最后一行, 避免重复。
            // NOW 是画面里此刻的人数 (已确认、未丢失), 不是累计值。
            const int osd_margin = std::min(12, std::max(2, img.width / 64));
            constexpr int kStatusFont = 18;
            const auto line_counts = rules.line_counts();
            const int people = rules.people_in_frame();
            int count_rows = 0;
            for (const mb::NamedLine& l : rules.lines()) {
                if (rules.line_active(l) && line_counts.count(l.id)) ++count_rows;
            }
            int count_y = osd_margin;
            const int count_right = img.width - osd_margin;
            for (const mb::NamedLine& l : rules.lines()) {
                if (!rules.line_active(l)) continue;
                const auto it = line_counts.find(l.id);
                if (it == line_counts.end()) continue;
                if (count_rows == 1)
                    snprintf(text, sizeof(text), "%s  IN %d  OUT %d  NOW %d", l.id.c_str(),
                             it->second.in, it->second.out, people);
                else
                    snprintf(text, sizeof(text), "%s  IN %d  OUT %d", l.id.c_str(),
                             it->second.in, it->second.out);
                mb::draw_text_right_highlighted(&osd, text, count_right, count_y,
                                                0xFFFFD400, kStatusFont);
                count_y += text_height(kStatusFont) + 2;
            }
            if (count_rows != 1 && cfg.path("features.count.enabled").boolean(false)) {
                snprintf(text, sizeof(text), "NOW %d", people);
                mb::draw_text_right_highlighted(&osd, text, count_right, count_y,
                                                0xFFFFD400, kStatusFont);
            }

            // 右下角显示设备本地时间, 精确到秒。
            char clock_text[32];
            const std::time_t wall_now = std::time(nullptr);
            std::tm local_now{};
            if (localtime_r(&wall_now, &local_now) != nullptr &&
                std::strftime(clock_text, sizeof(clock_text), "%Y-%m-%d %H:%M:%S",
                              &local_now) > 0) {
                const int clock_font = kStatusFont;
                const int clock_y = img.height - osd_margin - text_height(clock_font);
                mb::draw_text_right_highlighted(&osd, clock_text, count_right, clock_y,
                                                0xFFFFD400, clock_font);
            }

            st.osd_text_ms +=
                std::chrono::duration<double, std::milli>(Clock::now() - t_osd_text).count();
            const Clock::time_point t_osd_bev = Clock::now();
            // 鸟瞰图画中画 (display.bev_pip.enabled, 设置页开关, 热加载生效)
            mb::draw_bev_pip(&img, rules.locator().scene(), mb::BevPipParams::from_json(cfg));
            st.osd_bev_ms +=
                std::chrono::duration<double, std::milli>(Clock::now() - t_osd_bev).count();
        }
        st.osd_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_osd).count();

        // 快照用 OSD 之后的图 (带框), 和网页/APP 看到的一致
        if (events_ok) events.attach_frame(img);
        if (video.running()) {
            const Clock::time_point t_encode_fill = Clock::now();
            video.submit(img, vf.timestamp_ms);
            st.encode_fill_ms +=
                std::chrono::duration<double, std::milli>(Clock::now() - t_encode_fill).count();
        }
        if (hub_wants) hub.offer(img, now_ms);

        st.loop_ms += std::chrono::duration<double, std::milli>(Clock::now() - t_frame).count();
        {
            // V4L2 时间戳和 steady_clock 都是 CLOCK_MONOTONIC, 可以直接相减
            const uint64_t done = steady_ms();
            if (done >= vf.timestamp_ms && done - vf.timestamp_ms < 10000) {
                st.latency_ms += (double)(done - vf.timestamp_ms);
                ++st.latency_n;
            }
        }

        // ---- 存标注图 ----
        if (!opt.save_dir.empty()) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%06lld.jpg", opt.save_dir.c_str(),
                     (long long)frame_index);
            std::string e2;
            if (!mb::jpeg_encode_file(path, img, 85, &e2)) {
                fprintf(stderr, "[mb-capture] 存图失败 %s: %s\n", path, e2.c_str());
            }
        }

        // ---- 统计 ----
        ++st.frames;
        const double elapsed = std::chrono::duration<double>(Clock::now() - st.t_last).count();
        if (elapsed >= 2.0) {
            snap.fps = (double)st.frames / elapsed;
            snap.ai_ms = st.infers > 0 ? st.ai_total_ms / (double)st.infers : 0.0;
            snap.decode_ms = st.frames > 0 ? st.decode_ms / (double)st.frames : 0.0;
            snap.loop_ms = st.frames > 0 ? st.loop_ms / (double)st.frames : 0.0;
            snap.ai_consume_ms = st.frames > 0 ? st.ai_consume_ms / (double)st.frames : 0.0;
            snap.infer_copy_ms = st.frames > 0 ? st.infer_copy_ms / (double)st.frames : 0.0;
            snap.rules_ms = st.frames > 0 ? st.rules_ms / (double)st.frames : 0.0;
            snap.osd_ms = st.frames > 0 ? st.osd_ms / (double)st.frames : 0.0;
            snap.encode_fill_ms = st.frames > 0 ? st.encode_fill_ms / (double)st.frames : 0.0;
            snap.osd_shapes_ms = st.frames > 0 ? st.osd_shapes_ms / (double)st.frames : 0.0;
            snap.osd_objects_ms = st.frames > 0 ? st.osd_objects_ms / (double)st.frames : 0.0;
            snap.osd_text_ms = st.frames > 0 ? st.osd_text_ms / (double)st.frames : 0.0;
            snap.osd_bev_ms = st.frames > 0 ? st.osd_bev_ms / (double)st.frames : 0.0;
            snap.latency_ms = st.latency_n > 0 ? st.latency_ms / (double)st.latency_n : 0.0;
            printf("[stat] 采集 %.1f fps, AI %.1f ms/次 (%lld 次), 解码 %.1f ms, 单帧 %.1f ms, "
                   "阶段[AI处理 %.1f / 拷贝 %.1f / 规则 %.1f / OSD %.1f / H264填帧 %.1f], "
                   "时延 %.0f ms, 目标 %zu / 轨迹 %zu\n",
                   snap.fps, snap.ai_ms, (long long)st.infers, snap.decode_ms, snap.loop_ms,
                   snap.ai_consume_ms, snap.infer_copy_ms, snap.rules_ms, snap.osd_ms,
                   snap.encode_fill_ms, snap.latency_ms, dets.size(), tracks.size());
            fflush(stdout);
            const double keep_infer_ms = st.infer_ms;  // OSD 状态栏一直要显示最近一次
            st = Stats();
            st.infer_ms = keep_infer_ms;
            st.t_last = Clock::now();
        }

        capture_heartbeat_ms.store(steady_ms(), std::memory_order_relaxed);
        publish_status();
    }

    printf("[mb-capture] 正在退出 …\n");
    fflush(stdout);
    g_running = false;
    if (capture_watchdog.joinable()) capture_watchdog.join();
    worker.stop();
    http.stop();
    geo.stop();
    hub.stop();
    cam.close();
    {
        std::string aerr;
        if (!activity.maybe_save(steady_ms(), true, &aerr))
            fprintf(stderr, "[mb-capture] 活动热图保存失败: %s\n", aerr.c_str());
        // 最后一次 poll 之后可能还有没落盘的增量 (包括最后一分钟), 退出前写完
        counts.poll(rules.line_counts());
        std::string cerr;
        if (!counts.maybe_flush(steady_ms(), true, &cerr))
            fprintf(stderr, "[mb-capture] 越线计数保存失败: %s\n", cerr.c_str());
    }
    // 先停编码: 它会把没录满的告警片段落盘, 并通过回调让事件引擎更新 JSON;
    // 再停事件引擎, 它的写线程会把队列里剩下的活干完才退出。
    video.stop();
    events.stop();
    printf("[mb-capture] 结束, 共 %lld 帧 (最后时间戳 %llu ms)\n", (long long)frame_index,
           (unsigned long long)latest_ts);
    return 0;
}
