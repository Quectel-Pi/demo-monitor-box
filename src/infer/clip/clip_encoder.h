#ifndef MONITOR_BOX_CLIP_ENCODER_H_
#define MONITOR_BOX_CLIP_ENCODER_H_

#include <memory>
#include <string>
#include <vector>

class CLIPTokenizer;

namespace mb {

// CLIP 文本编码器 (clip_text.rknn)。
//
// 输入  INT32 [batch, sequence_len]  BPE token id (BOS + 文本 + EOS + PAD)
// 输出  FLOAT32 [batch, embed_dim]   文本嵌入, 直接喂给 yolo_world 的第二个输入
//
// 与 RknnEngine 分开写: RknnEngine 假定输入 0 是 NHWC 图像, 这里的输入是 2 维
// int32 token 序列、输出是 2 维浮点, 两条路径的 shape 语义完全不同。
//
// 版本纪律同 RknnEngine: 必须与 librknnrt 2.3.2 对齐。
class ClipTextEncoder {
public:
    ClipTextEncoder();
    ~ClipTextEncoder();

    ClipTextEncoder(const ClipTextEncoder&) = delete;
    ClipTextEncoder& operator=(const ClipTextEncoder&) = delete;

    bool load(const std::string& model_path, int core_mask, std::string* err);
    void release();
    bool ok() const { return ctx_ != nullptr; }

    int sequence_len() const { return sequence_len_; }
    int embed_dim() const { return embed_dim_; }
    // 每次 run 能吃几个文本 (模型输入的 batch, 通常是 1)。
    int batch() const { return batch_; }

    // 逐条编码 (内部按 batch 循环)。out 是 texts.size() * embed_dim 个 float。
    bool encode(const std::vector<std::string>& texts, std::vector<float>* out, std::string* err);

private:
    void* ctx_ = nullptr;  // rknn_context
    std::unique_ptr<CLIPTokenizer> tok_;
    int n_input_ = 1;
    int batch_ = 1;
    int sequence_len_ = 0;
    int embed_dim_ = 0;
};

}  // namespace mb

#endif  // MONITOR_BOX_CLIP_ENCODER_H_
