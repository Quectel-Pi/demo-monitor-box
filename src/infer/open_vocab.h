#ifndef MONITOR_BOX_OPEN_VOCAB_H_
#define MONITOR_BOX_OPEN_VOCAB_H_

#include <string>
#include <vector>

#include "infer/clip/clip_encoder.h"
#include "infer/preprocess.h"
#include "infer/rknn_engine.h"
#include "infer/types.h"

namespace mb {

// 开放词表检测 (yolo_world + clip_text)。
//
// 与固定类别的 yolov8s 不同, 这里的类别由提示词决定: 给 "door" / "fence" 这样的词,
// 模型就在画面里找它们。APP 用它在用户手画区域之前先自动圈出门口/围栏。
//
// 两个模型:
//   yolo_world.rknn  双输入: images UINT8 NHWC [1,3,640,640] + texts FLOAT32 [1,80,512]
//   clip_text.rknn   文本编码器: token id INT32 [1,20] -> 嵌入 FLOAT32 [1,512]
//
// 文本输入固定 80 个槽位, 槽位少于 80 时把提示词循环填满 (必须填真实嵌入:
// 留零会导致误检), 后处理只保留 cls_id < 提示词个数 的结果。
class OpenVocabDetector {
public:
    OpenVocabDetector() = default;
    ~OpenVocabDetector() = default;

    OpenVocabDetector(const OpenVocabDetector&) = delete;
    OpenVocabDetector& operator=(const OpenVocabDetector&) = delete;

    // prompts 为空时返回 false (没有类别就没法检测)。core_mask 对两个模型都生效。
    bool load(const std::string& yolo_model, const std::string& clip_model,
              const std::vector<std::string>& prompts, int core_mask, std::string* err);
    void release();
    bool ok() const { return yolo_.ok(); }

    const std::vector<std::string>& prompts() const { return prompts_; }

    // 换提示词。成功后下一帧 detect() 就用新类别 (文本嵌入重新编码一次, ~几十毫秒)。
    bool set_prompts(const std::vector<std::string>& prompts, std::string* err);

    // 检测一帧。box 是原图像素坐标; label 是 prompts_[prompt_id]。
    struct Object {
        int prompt_id = 0;
        std::string label;
        float score = 0.f;
        float x1 = 0.f, y1 = 0.f, x2 = 0.f, y2 = 0.f;
    };

    bool detect(const ImageRGB& frame, float conf, float nms, const PreprocessOptions& popt,
                std::vector<Object>* out, std::string* err);

    // 最近一次 detect 的模型输入尺寸, 排错用。
    int input_width() const { return yolo_.input_width(); }
    int input_height() const { return yolo_.input_height(); }

private:
    // 按 80 个槽位重建文本嵌入并喂给 yolo_.set_aux_input()。
    bool build_text_input(std::string* err);

    RknnEngine yolo_;
    ClipTextEncoder clip_;
    std::vector<std::string> prompts_;
    ImageRGB padded_;  // letterbox 目标缓冲, 复用避免每帧分配
};

}  // namespace mb

#endif  // MONITOR_BOX_OPEN_VOCAB_H_
