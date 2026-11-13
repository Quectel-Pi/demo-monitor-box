#ifndef MONITOR_BOX_YOLO_POST_H_
#define MONITOR_BOX_YOLO_POST_H_

#include <string>
#include <vector>

#include "infer/rknn_engine.h"
#include "infer/types.h"

namespace mb {

// YOLOv5 路径的类别数 (COCO 80 类), 写死; 换 v5 自定义模型时改这里 + 换标签文件。
// 默认的 yolov8s 走 YoloV8 路径, 类别数取 cls 输出的通道数, 不受它限制。
constexpr int kYoloClassNum = 80;
constexpr int kMaxDetections = 128;

struct YoloParams {
    float conf_threshold = 0.25f;
    float nms_threshold = 0.45f;
    // 仅 YoloWorld 用: 有效提示词个数。文本输入有固定槽位 (默认 80), 只有前 num_prompts 个
    // 槽位是调用方真正关心的类; 0 = 不限 (此时类号直接按槽位号给)。多出来的槽位即使填了
    // 循环复用的真实嵌入也要丢弃, 否则同一条提示词会出现两次。
    int num_prompts = 0;
};

// 支持的检测头, 按输出张量个数区分:
//   YoloV5: 3 个输出 (stride 8/16/32), anchor-based, 类别数固定 kYoloClassNum
//   YoloV8: airockchip/ultralytics_yolov8 导出的 RKNN 友好格式, 每个 stride 一组
//           [box(4*16 DFL), cls(nc, 已 sigmoid), cls_sum(1, 可省)] → 9 个 (或 6 个) 输出;
//           类别数从 cls 张量通道数读, 火焰烟雾这类自训模型 (nc=2) 走这条
//   YoloWorld: 双输入开放词表模型 (yolo_world)。每个 stride 一组
//           [score(nc, 已 sigmoid), box(4), score_sum(1, 可省)], 注意 **分数张量在前、框在后**
//           (与 v8 相反); 框是 ltrb 距离, 没有 DFL。类别数 = 提示词个数, 由调用方告知。
enum class YoloHead { Unknown, YoloV5, YoloV8, YoloWorld };

YoloHead yolo_head_kind(const RknnEngine& engine);
const char* yolo_head_name(YoloHead h);
// YoloV5 返回 kYoloClassNum; YoloV8 返回 cls 张量通道数; 认不出返回 0。
int yolo_num_classes(const RknnEngine& engine);

// 解码全部输出头 → 逐类 NMS → 反 letterbox 还原到原图坐标。按 yolo_head_kind 自动分派。
// YoloV5 的 outputs 顺序必须为 stride 8/16/32 (rknn-toolkit2 导出的 yolov5 都是这个顺序)。
bool yolo_postprocess(const RknnEngine& engine, const YoloParams& params, const Letterbox& lb,
                      std::vector<Detection>* out, std::string* err);

// 读 coco 标签文件, 一行一类。失败返回空表 (此时 infer.cpp 会退回打印类号)。
std::vector<std::string> load_labels(const std::string& path, std::string* err);

}  // namespace mb

#endif  // MONITOR_BOX_YOLO_POST_H_
