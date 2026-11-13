#include "infer/open_vocab.h"

#include <cstdio>
#include <cstring>

#include "infer/yolo_post.h"

namespace mb {
namespace {

// 文本输入槽位数 (yolo_world 的 texts 张量是 [1, 80, 512])。模型在 load 时读, 这里只是上限。
constexpr int kMaxTextSlots = 128;

}  // namespace

void OpenVocabDetector::release() {
    yolo_.release();
    clip_.release();
    prompts_.clear();
}

bool OpenVocabDetector::load(const std::string& yolo_model, const std::string& clip_model,
                             const std::vector<std::string>& prompts, int core_mask,
                             std::string* err) {
    release();
    if (prompts.empty()) {
        if (err) *err = "open vocab: no prompts";
        return false;
    }

    if (!yolo_.load(yolo_model, core_mask, err)) return false;
    if (yolo_.input_count() != 2) {
        if (err) {
            *err = "open vocab: " + yolo_model + " has " + std::to_string(yolo_.input_count()) +
                   " inputs (2 expected: image + text embeddings)";
        }
        release();
        return false;
    }
    if (yolo_.aux_input_n_dims() != 3) {
        if (err) *err = "open vocab: text input must be [1, slots, dim]";
        release();
        return false;
    }

    if (!clip_.load(clip_model, core_mask, err)) {
        release();
        return false;
    }
    if (clip_.embed_dim() != (int)yolo_.aux_input_dim(2)) {
        if (err) {
            *err = "open vocab: clip embed_dim " + std::to_string(clip_.embed_dim()) +
                   " != yolo_world text dim " + std::to_string(yolo_.aux_input_dim(2));
        }
        release();
        return false;
    }

    prompts_ = prompts;
    if (!build_text_input(err)) {
        release();
        return false;
    }

    printf("[open_vocab] %dx%d, %d 个提示词 / %u 个文本槽位\n", yolo_.input_width(),
           yolo_.input_height(), (int)prompts_.size(), yolo_.aux_input_dim(1));
    return true;
}

bool OpenVocabDetector::set_prompts(const std::vector<std::string>& prompts, std::string* err) {
    if (!yolo_.ok()) {
        if (err) *err = "open vocab: not loaded";
        return false;
    }
    if (prompts.empty()) {
        if (err) *err = "open vocab: no prompts";
        return false;
    }
    const std::vector<std::string> saved = prompts_;
    prompts_ = prompts;
    if (!build_text_input(err)) {
        prompts_ = saved;
        return false;
    }
    return true;
}

bool OpenVocabDetector::build_text_input(std::string* err) {
    // dims[] 是元素个数, 不是字节数 —— FLOAT32 也不用再除 4。
    const int slots = (int)yolo_.aux_input_dim(1);
    const int dim = (int)yolo_.aux_input_dim(2);
    if (slots <= 0 || dim <= 0 || slots > kMaxTextSlots) {
        if (err) *err = "open vocab: bad text input shape";
        return false;
    }
    if (clip_.embed_dim() != dim) {
        if (err) {
            *err = "open vocab: clip dim " + std::to_string(clip_.embed_dim()) + " != " +
                   std::to_string(dim);
        }
        return false;
    }

    // 槽位多出来的部分循环复用提示词的真实嵌入 —— 绝不能留零向量,
    // 否则模型会在那些"空类"上给出随机高分 (实测会冒出几十个假框)。
    std::vector<std::string> slots_text((size_t)slots);
    for (int i = 0; i < slots; ++i) slots_text[(size_t)i] = prompts_[(size_t)i % prompts_.size()];

    std::vector<float> emb;
    if (!clip_.encode(slots_text, &emb, err)) return false;
    if (emb.size() != (size_t)slots * (size_t)dim) {
        if (err) *err = "open vocab: text embedding size mismatch";
        return false;
    }
    return yolo_.set_aux_input(emb.data(), emb.size() * sizeof(float), err);
}

bool OpenVocabDetector::detect(const ImageRGB& frame, float conf, float nms,
                               const PreprocessOptions& popt, std::vector<Object>* out,
                               std::string* err) {
    if (!out) return false;
    out->clear();
    if (!yolo_.ok()) {
        if (err) *err = "open vocab: not loaded";
        return false;
    }

    padded_.width = yolo_.input_width();
    padded_.height = yolo_.input_height();
    Letterbox lb;
    const char* warn = nullptr;
    if (!letterbox_rgb(frame, &padded_, &lb, popt, &warn)) {
        if (err) *err = "open vocab: letterbox failed";
        return false;
    }
    if (warn) printf("[open_vocab] %s\n", warn);

    if (!yolo_.run(padded_.data.data(), padded_.size_bytes(), err)) return false;

    YoloParams yp;
    yp.conf_threshold = conf;
    yp.nms_threshold = nms;
    yp.num_prompts = (int)prompts_.size();

    std::vector<Detection> dets;
    if (!yolo_postprocess(yolo_, yp, lb, &dets, err)) return false;

    out->reserve(dets.size());
    for (const Detection& d : dets) {
        if (d.cls_id < 0 || d.cls_id >= (int)prompts_.size()) continue;
        Object o;
        o.prompt_id = d.cls_id;
        o.label = prompts_[(size_t)d.cls_id];
        o.score = d.score;
        o.x1 = d.x1;
        o.y1 = d.y1;
        o.x2 = d.x2;
        o.y2 = d.y2;
        out->push_back(o);
    }
    return true;
}

}  // namespace mb
