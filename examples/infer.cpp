// mb-infer —— M1 里程碑的验收工具: 一张图进, 框出来。
//
// 用法:
//   mb-infer --model <x.rknn> --image <in.jpg> [--output out.jpg]
//            [--labels <labels.txt>] [--conf 0.25] [--nms 0.45]
//            [--core <mask>] [--no-rga] [--dump-pre <path>]
//            [--preprocess-only --input-size 640x640]
//            [--pose <pose.rknn> [--pose-full] [--max-persons N]]
//            [--clip <clip_text.rknn> [--prompts "door,fence"]]
//
// 目录约定 (板上):
//   /opt/monitor_box/models/yolov8s-640-640.rknn
//   /opt/monitor_box/models/coco_80_labels_list.txt
//   /opt/monitor_box/models/fire_yolov8n.rknn + fire_labels.txt  (YOLOv8 头, 按输出个数自动识别)
//   /opt/monitor_box/models/yolo_world_v2s-rk3576.rknn + clip_text.rknn  (开放词表, --clip)
// 本机跑 (qemu) 时把 --model/--image 指到 prebuilds/sysroot 下的同名文件即可。
//
// --preprocess-only 不加载模型, 只跑 JPEG 解码 + letterbox (+ --dump-pre),
// 宿主机没有 NPU 时用它验证预处理链路。此时需要 --input-size 给出模型输入尺寸。
//
// --clip 走开放词表路径: --model 必须是双输入的 yolo_world, 类别来自 --prompts 而不是标签文件。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "infer/jpeg_io.h"
#include "infer/open_vocab.h"
#include "infer/pose_runner.h"
#include "infer/preprocess.h"
#include "infer/rknn_engine.h"
#include "infer/types.h"
#include "infer/yolo_post.h"

extern "C" {
#include "image_drawing.h"
}

namespace {

struct Options {
    std::string model;
    std::string image;
    std::string output;
    std::string labels;
    float conf = 0.25f;
    float nms = 0.45f;
    int core_mask = -1;  // -1 = 不调用 rknn_set_core_mask
    bool use_rga = true;
    std::string dump_pre;  // 非空则把 letterbox 后的模型输入另存一份
    bool preprocess_only = false;  // 不碰 NPU, 只验证解码+letterbox
    int input_w = 0;               // --preprocess-only 时模型输入尺寸, 0 = 从模型读
    int input_h = 0;
    int repeat = 1;  // 推理重复次数, 用于量 NPU 单帧耗时 (M0 验收)
    std::string pose_model;  // 非空则在检测之后按人跑姿态 (与 mb-capture 同一份 PoseRunner)
    bool pose_full = false;  // YOLO-pose 走整帧而不是按人 ROI
    int max_persons = 3;
    std::string clip_model;             // 非空 = 开放词表路径 (--model 是 yolo_world)
    std::vector<std::string> prompts;   // --prompts "door,fence"
};

void usage(const char* argv0) {
    printf(
        "用法: %s --model <x.rknn> --image <in.jpg> [选项]\n"
        "\n"
        "  --model <path>     .rknn 模型 (必须与 librknnrt 2.3.2 配套)\n"
        "  --image <path>     输入 JPEG\n"
        "  --output <path>    结果图, 默认 <image 去后缀>_out.jpg\n"
        "  --labels <path>    类别名文件, 缺省时打印类号\n"
        "  --conf <f>         置信度阈值, 默认 0.25\n"
        "  --nms <f>          NMS IoU 阈值, 默认 0.45\n"
        "  --core <mask>      NPU core mask: 0=AUTO 1=CORE_0 2=CORE_1 4=CORE_2 6=CORE_0_1\n"
        "  --no-rga           强制走 CPU 双线性 (RGA 出问题时用)\n"
        "  --dump-pre <path>  把 letterbox 后的模型输入存成 jpg, 排查预处理用\n"
        "  --preprocess-only  不加载模型, 只验证解码+letterbox (宿主机无 NPU 时用)\n"
        "  --input-size WxH   --preprocess-only 下的模型输入尺寸, 如 640x640\n"
        "  --repeat <n>       推理重复 n 次只报耗时, 用于量 NPU 单帧耗时\n"
        "  --pose <path>      检测后再跑姿态 (yolov8-pose / RTMPose 自动识别), 画骨架\n"
        "  --pose-full        yolov8-pose 走整帧, 不按人 ROI (RTMPose 不支持)\n"
        "  --max-persons <n>  按人 ROI 时最多几人, 默认 3\n"
        "  --clip <path>      开放词表: clip_text.rknn 文本编码器 (配合双输入的 --model)\n"
        "  --prompts <a,b>    开放词表提示词, 逗号分隔, 默认 door,fence,railing,gate\n",
        argv0);
}

bool parse_args(int argc, char** argv, Options* opt) {
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
        } else if (a == "--model") {
            const char* v = next("--model");
            if (!v) return false;
            opt->model = v;
        } else if (a == "--image") {
            const char* v = next("--image");
            if (!v) return false;
            opt->image = v;
        } else if (a == "--output") {
            const char* v = next("--output");
            if (!v) return false;
            opt->output = v;
        } else if (a == "--labels") {
            const char* v = next("--labels");
            if (!v) return false;
            opt->labels = v;
        } else if (a == "--conf") {
            const char* v = next("--conf");
            if (!v) return false;
            opt->conf = (float)atof(v);
        } else if (a == "--nms") {
            const char* v = next("--nms");
            if (!v) return false;
            opt->nms = (float)atof(v);
        } else if (a == "--core") {
            const char* v = next("--core");
            if (!v) return false;
            opt->core_mask = atoi(v);
        } else if (a == "--no-rga") {
            opt->use_rga = false;
        } else if (a == "--dump-pre") {
            const char* v = next("--dump-pre");
            if (!v) return false;
            opt->dump_pre = v;
        } else if (a == "--preprocess-only") {
            opt->preprocess_only = true;
        } else if (a == "--input-size") {
            const char* v = next("--input-size");
            if (!v) return false;
            int w = 0, h = 0;
            if (sscanf(v, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                fprintf(stderr, "--input-size 格式应为 WxH, 如 640x640\n");
                return false;
            }
            opt->input_w = w;
            opt->input_h = h;
        } else if (a == "--pose") {
            const char* v = next("--pose");
            if (!v) return false;
            opt->pose_model = v;
        } else if (a == "--pose-full") {
            opt->pose_full = true;
        } else if (a == "--max-persons") {
            const char* v = next("--max-persons");
            if (!v) return false;
            opt->max_persons = atoi(v);
        } else if (a == "--repeat") {
            const char* v = next("--repeat");
            if (!v) return false;
            opt->repeat = atoi(v);
            if (opt->repeat < 1) {
                fprintf(stderr, "--repeat 必须是 >= 1 的整数\n");
                return false;
            }
        } else if (a == "--clip") {
            const char* v = next("--clip");
            if (!v) return false;
            opt->clip_model = v;
        } else if (a == "--prompts") {
            const char* v = next("--prompts");
            if (!v) return false;
            opt->prompts.clear();
            std::string cur;
            for (const char* p = v;; ++p) {
                if (*p == ',' || *p == '\0') {
                    // 去掉首尾空格, 空项跳过
                    size_t b = cur.find_first_not_of(" \t");
                    size_t e = cur.find_last_not_of(" \t");
                    if (b != std::string::npos) opt->prompts.push_back(cur.substr(b, e - b + 1));
                    cur.clear();
                    if (*p == '\0') break;
                } else {
                    cur += *p;
                }
            }
            if (opt->prompts.empty()) {
                fprintf(stderr, "--prompts 不能为空\n");
                return false;
            }
        } else {
            fprintf(stderr, "未知参数: %s\n", a.c_str());
            return false;
        }
    }
    if (!opt->preprocess_only && (opt->model.empty() || opt->image.empty())) {
        fprintf(stderr, "--model 和 --image 必填\n");
        return false;
    }
    if (opt->preprocess_only && opt->image.empty()) {
        fprintf(stderr, "--preprocess-only 至少需要 --image\n");
        return false;
    }
    if (opt->preprocess_only && (opt->input_w <= 0 || opt->input_h <= 0)) {
        fprintf(stderr, "--preprocess-only 需要 --input-size WxH 指定模型输入尺寸\n");
        return false;
    }
    return true;
}

std::string default_output(const std::string& image, const char* suffix) {
    size_t dot = image.find_last_of('.');
    size_t slash = image.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) dot = image.size();
    return image.substr(0, dot) + suffix;
}

unsigned int color_for_class(int cls) {
    // 稳定的伪随机配色, 同一类每次同色。
    static const unsigned int palette[] = {
        0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF,
        0xFF00FFFF, 0xFFFF8000, 0xFF8000FF, 0xFF00FF80, 0xFF80FF00,
    };
    return palette[(cls < 0 ? 0 : cls) % (int)(sizeof(palette) / sizeof(palette[0]))];
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) {
        usage(argv[0]);
        return 2;
    }
    if (opt.output.empty()) opt.output = default_output(opt.image, "_out.jpg");

    std::string err;

    // ---- 1. 读图 ----
    mb::ImageRGB img;
    if (!mb::jpeg_decode_file(opt.image, &img, &err)) {
        fprintf(stderr, "[mb-infer] 解码失败: %s\n", err.c_str());
        return 1;
    }
    printf("[mb-infer] 输入 %s %dx%d\n", opt.image.c_str(), img.width, img.height);

    // ---- 开放词表路径: --model 是双输入 yolo_world, 类别来自 --prompts ----
    // 这条路自己管预处理/推理/后处理 (OpenVocabDetector), 不走下面的单输入 engine 流程。
    if (!opt.clip_model.empty()) {
        mb::PreprocessOptions popt;
        popt.use_rga = opt.use_rga;

        mb::OpenVocabDetector det;
        if (!det.load(opt.model, opt.clip_model, opt.prompts, opt.core_mask, &err)) {
            fprintf(stderr, "[mb-infer] 开放词表加载失败: %s\n", err.c_str());
            return 1;
        }
        printf("[mb-infer] 开放词表 %dx%d, 提示词:", det.input_width(), det.input_height());
        for (const std::string& p : det.prompts()) printf(" %s", p.c_str());
        printf("\n");

        const int runs = opt.repeat > 0 ? opt.repeat : 1;
        std::vector<mb::OpenVocabDetector::Object> objs;
        double total_ms = 0.0;
        double first_ms = 0.0;
        for (int it = 0; it < runs; ++it) {
            const auto t0 = std::chrono::steady_clock::now();
            if (!det.detect(img, opt.conf, opt.nms, popt, &objs, &err)) {
                fprintf(stderr, "[mb-infer] 检测失败: %s\n", err.c_str());
                return 1;
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            if (it == 0) {
                first_ms = ms;  // 含 NPU 预热
            } else {
                total_ms += ms;
            }
        }
        if (runs > 1) {
            const double avg = total_ms / (runs - 1);
            printf("[mb-infer] 端到端 %d 次 (首次 %.2f ms 不计): 平均 %.2f ms/帧 -> %.1f fps\n",
                   runs, first_ms, avg, avg > 0.0 ? 1000.0 / avg : 0.0);
        } else {
            printf("[mb-infer] 端到端 1 次 (含预处理+推理+后处理): %.2f ms\n", first_ms);
        }

        printf("[mb-infer] 检出 %zu 个目标\n", objs.size());

        image_buffer_t osd;
        memset(&osd, 0, sizeof(osd));
        osd.width = img.width;
        osd.height = img.height;
        osd.width_stride = img.width;
        osd.height_stride = img.height;
        osd.format = IMAGE_FORMAT_RGB888;
        osd.virt_addr = img.data.data();
        osd.size = (int)img.size_bytes();
        osd.fd = -1;

        char text[128];
        for (size_t i = 0; i < objs.size(); ++i) {
            const mb::OpenVocabDetector::Object& o = objs[i];
            printf("  [%2zu] %-12s %.3f  (%.0f, %.0f) - (%.0f, %.0f)\n", i, o.label.c_str(), o.score,
                   o.x1, o.y1, o.x2, o.y2);
            const int x1 = (int)o.x1;
            const int y1 = (int)o.y1;
            const int x2 = (int)o.x2;
            const int y2 = (int)o.y2;
            if (x2 <= x1 || y2 <= y1) continue;
            const unsigned int color = color_for_class(o.prompt_id);
            draw_rectangle(&osd, x1, y1, x2 - x1, y2 - y1, color, 3);
            snprintf(text, sizeof(text), "%s %.1f%%", o.label.c_str(), o.score * 100.0f);
            int ty = y1 - 20;
            if (ty < 0) ty = y1 + 2;
            draw_text(&osd, text, x1, ty, color, 10);
        }

        if (!mb::jpeg_encode_file(opt.output, img, 90, &err)) {
            fprintf(stderr, "[mb-infer] 存结果图失败: %s\n", err.c_str());
            return 1;
        }
        printf("[mb-infer] 结果图 -> %s\n", opt.output.c_str());
        return 0;
    }

    // ---- 2. 加载模型 ----
    mb::RknnEngine engine;
    int in_w = opt.input_w;
    int in_h = opt.input_h;
    if (!opt.preprocess_only) {
        if (!engine.load(opt.model, opt.core_mask, &err)) {
            fprintf(stderr, "[mb-infer] 模型加载失败: %s\n", err.c_str());
            return 1;
        }
        engine.print_versions();
        in_w = engine.input_width();
        in_h = engine.input_height();
        if (engine.input_channel() != 3) {
            fprintf(stderr, "[mb-infer] 只支持 3 通道输入, 模型是 %d\n", engine.input_channel());
            return 1;
        }
    }

    // ---- 3. letterbox ----
    mb::ImageRGB input;
    input.width = in_w;
    input.height = in_h;

    mb::PreprocessOptions popt;
    popt.use_rga = opt.use_rga;
    mb::Letterbox lb;
    const char* warn = nullptr;
    if (!mb::letterbox_rgb(img, &input, &lb, popt, &warn)) {
        fprintf(stderr, "[mb-infer] 预处理失败\n");
        return 1;
    }
    printf("[mb-infer] letterbox: scale=%.5f pad=(%d,%d) -> %dx%d\n", lb.scale, lb.x_pad, lb.y_pad,
           input.width, input.height);
    if (warn) printf("[mb-infer] %s\n", warn);

    if (!opt.dump_pre.empty()) {
        std::string e;
        if (mb::jpeg_encode_file(opt.dump_pre, input, 90, &e)) {
            printf("[mb-infer] 模型输入已存 %s\n", opt.dump_pre.c_str());
        } else {
            fprintf(stderr, "[mb-infer] 存模型输入失败: %s\n", e.c_str());
        }
    }

    if (opt.preprocess_only) {
        printf("[mb-infer] --preprocess-only: 预处理链路正常, 未加载模型\n");
        return 0;
    }

    // ---- 4. 推理 ----
    // rknn_benchmark 在 RK3576 上不可用 (打完成张量信息就 "load image failed!", 退出码却是 0),
    // 所以 NPU 单帧耗时在这里量: --repeat N 连续推理 N 次, 只报 rknn_run 本身的均值。
    using Clock = std::chrono::steady_clock;
    double infer_total_ms = 0.0;
    double first_ms = 0.0;
    const int runs = opt.repeat > 0 ? opt.repeat : 1;
    for (int it = 0; it < runs; ++it) {
        const Clock::time_point t0 = Clock::now();
        const bool ok = engine.run(input.data.data(), input.size_bytes(), &err);
        const Clock::time_point t1 = Clock::now();
        if (!ok) {
            fprintf(stderr, "[mb-infer] 推理失败: %s\n", err.c_str());
            return 1;
        }
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (it == 0) {
            first_ms = ms;  // 含 NPU 首次提交/预热, 不计入 --repeat 均值
        } else {
            infer_total_ms += ms;
        }
    }
    if (runs > 1) {
        const double avg = infer_total_ms / (runs - 1);
        printf("[mb-infer] NPU 推理 %d 次 (首次 %.2f ms 不计): 平均 %.2f ms/帧 -> %.1f fps\n",
               runs, first_ms, avg, avg > 0.0 ? 1000.0 / avg : 0.0);
    } else {
        printf("[mb-infer] NPU 推理 1 次: %.2f ms\n", first_ms);
    }

    // ---- 5. 后处理 ----
    mb::YoloParams yp;
    yp.conf_threshold = opt.conf;
    yp.nms_threshold = opt.nms;

    std::vector<mb::Detection> dets;
    if (!mb::yolo_postprocess(engine, yp, lb, &dets, &err)) {
        fprintf(stderr, "[mb-infer] 后处理失败: %s\n", err.c_str());
        return 1;
    }

    std::vector<std::string> labels;
    if (!opt.labels.empty()) {
        labels = mb::load_labels(opt.labels, &err);
        if (labels.empty()) fprintf(stderr, "[mb-infer] 读标签失败(%s), 只打印类号\n", err.c_str());
    }

    printf("[mb-infer] 检出 %zu 个目标\n", dets.size());

    // ---- 5b. 姿态 (可选) ----
    // 必须在画框之前跑: 画框直接改 img 像素, 会污染姿态输入。
    std::vector<mb::PoseResult> poses;
    if (!opt.pose_model.empty()) {
        mb::RknnEngine pose;
        if (!pose.load(opt.pose_model, opt.core_mask, &err)) {
            fprintf(stderr, "[mb-infer] 姿态模型加载失败: %s\n", err.c_str());
            return 1;
        }
        const mb::PoseModelKind kind = mb::pose_model_kind(pose);
        printf("[mb-infer] 姿态模型 %dx%d 类型 %s\n", pose.input_width(), pose.input_height(),
               mb::pose_model_kind_name(kind));
        mb::PoseRunner runner;
        mb::PoseParams pp;
        pp.nms_threshold = opt.nms;
        mb::PoseRoiParams rp;
        rp.enabled = !opt.pose_full;
        rp.max_persons = opt.max_persons;
        const std::vector<mb::Detection> no_hints;
        double total_ms = 0.0;
        for (int it = 0; it < runs; ++it) {
            const Clock::time_point t0 = Clock::now();
            if (!runner.run(pose, img, &dets, no_hints, pp, rp, popt, &poses, &err)) {
                fprintf(stderr, "[mb-infer] 姿态失败: %s\n", err.c_str());
                return 1;
            }
            const double ms =
                std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (it == 0 && runs > 1) continue;  // 首次含预热
            total_ms += ms;
        }
        const double avg = total_ms / (runs > 1 ? runs - 1 : 1);
        printf("[mb-infer] 姿态 %d 次模型推理/轮, 平均 %.2f ms/轮 (含裁剪+预处理+后处理), 出 %zu 人\n",
               runner.last_runs(), avg, poses.size());
        for (size_t i = 0; i < poses.size(); ++i) {
            const mb::PoseResult& p = poses[i];
            int vis = 0;
            float sum = 0.0f;
            for (const mb::Keypoint& k : p.kpts) {
                if (k.conf > 0.3f) ++vis;
                sum += k.conf;
            }
            printf("  pose[%zu] score=%.3f box=(%.0f,%.0f)-(%.0f,%.0f) visible=%d/17 mean_conf=%.3f\n",
                   i, p.score, p.x1, p.y1, p.x2, p.y2, vis, sum / mb::kNumKeypoints);
            printf("    kpts:");
            for (const mb::Keypoint& k : p.kpts) printf(" %.1f,%.1f,%.2f", k.x, k.y, k.conf);
            printf("\n");
        }
    }

    // ---- 6. 画框 + 存图 ----
    // image_drawing 用的是 zoo 的 image_buffer_t, 直接包一层指向我们的 RGB 缓冲。
    image_buffer_t osd;
    memset(&osd, 0, sizeof(osd));
    osd.width = img.width;
    osd.height = img.height;
    osd.width_stride = img.width;
    osd.height_stride = img.height;
    osd.format = IMAGE_FORMAT_RGB888;
    osd.virt_addr = img.data.data();
    osd.size = (int)img.size_bytes();
    osd.fd = -1;

    char text[128];
    for (size_t i = 0; i < dets.size(); ++i) {
        const mb::Detection& d = dets[i];
        const char* name = (!labels.empty() && d.cls_id >= 0 && d.cls_id < (int)labels.size())
                               ? labels[(size_t)d.cls_id].c_str()
                               : "cls?";

        printf("  [%2zu] %-12s %.3f  (%.0f, %.0f) - (%.0f, %.0f)\n", i, name, d.score, d.x1, d.y1,
               d.x2, d.y2);

        const int x1 = (int)d.x1;
        const int y1 = (int)d.y1;
        const int x2 = (int)d.x2;
        const int y2 = (int)d.y2;
        if (x2 <= x1 || y2 <= y1) continue;

        const unsigned int color = color_for_class(d.cls_id);
        draw_rectangle(&osd, x1, y1, x2 - x1, y2 - y1, color, 3);

        snprintf(text, sizeof(text), "%s %.1f%%", name, d.score * 100.0f);
        int ty = y1 - 20;
        if (ty < 0) ty = y1 + 2;
        draw_text(&osd, text, x1, ty, color, 10);
    }

    for (const mb::PoseResult& p : poses) {
        for (const auto& e : mb::kSkeleton) {
            const mb::Keypoint& a = p.kpts[e[0]];
            const mb::Keypoint& b = p.kpts[e[1]];
            if (a.conf < 0.3f || b.conf < 0.3f) continue;
            draw_line(&osd, (int)a.x, (int)a.y, (int)b.x, (int)b.y, 0xFF00FF00, 2);
        }
        for (const mb::Keypoint& k : p.kpts) {
            if (k.conf >= 0.3f) draw_circle(&osd, (int)k.x, (int)k.y, 3, 0xFFFF0000, -1);
        }
    }

    if (!mb::jpeg_encode_file(opt.output, img, 90, &err)) {
        fprintf(stderr, "[mb-infer] 存结果图失败: %s\n", err.c_str());
        return 1;
    }
    printf("[mb-infer] 结果图 -> %s\n", opt.output.c_str());
    return 0;
}
