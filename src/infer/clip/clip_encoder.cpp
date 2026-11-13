#include "infer/clip/clip_encoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "clip_tokenizer.h"
#include "rknn_api.h"

namespace mb {
namespace {

const char* rknn_strerror(int ret) {
    switch (ret) {
        case RKNN_SUCC: return "RKNN_SUCC";
        case RKNN_ERR_FAIL: return "RKNN_ERR_FAIL";
        case RKNN_ERR_TIMEOUT: return "RKNN_ERR_TIMEOUT";
        case RKNN_ERR_DEVICE_UNAVAILABLE: return "RKNN_ERR_DEVICE_UNAVAILABLE";
        case RKNN_ERR_MALLOC_FAIL: return "RKNN_ERR_MALLOC_FAIL";
        case RKNN_ERR_PARAM_INVALID: return "RKNN_ERR_PARAM_INVALID";
        case RKNN_ERR_MODEL_INVALID: return "RKNN_ERR_MODEL_INVALID";
        case RKNN_ERR_INPUT_INVALID: return "RKNN_ERR_INPUT_INVALID";
        case RKNN_ERR_OUTPUT_INVALID: return "RKNN_ERR_OUTPUT_INVALID";
        case RKNN_ERR_DEVICE_UNMATCH: return "RKNN_ERR_DEVICE_UNMATCH";
        default: return "RKNN_ERR_UNKNOWN";
    }
}

}  // namespace

ClipTextEncoder::ClipTextEncoder() = default;

ClipTextEncoder::~ClipTextEncoder() { release(); }

void ClipTextEncoder::release() {
    if (ctx_) {
        rknn_destroy((rknn_context)ctx_);
        ctx_ = nullptr;
    }
    batch_ = 1;
    sequence_len_ = 0;
    embed_dim_ = 0;
    n_input_ = 1;
}

bool ClipTextEncoder::load(const std::string& model_path, int core_mask, std::string* err) {
    release();
    if (!tok_) tok_.reset(new CLIPTokenizer());

    // 直接把路径交给 rknn_init: 模型 127MB, 再拷一份进进程内没有意义。
    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, const_cast<char*>(model_path.c_str()), 0, 0, nullptr);
    if (ret != RKNN_SUCC) {
        if (err) {
            *err = std::string("rknn_init(") + model_path + ") failed: " + rknn_strerror(ret);
            if (ret == RKNN_ERR_MODEL_INVALID || ret == RKNN_ERR_DEVICE_UNMATCH)
                *err += " (模型与 librknnrt 版本不匹配? 必须同为 2.3.2)";
        }
        return false;
    }
    ctx_ = (void*)(uintptr_t)ctx;

    if (core_mask >= 0) {
        ret = rknn_set_core_mask(ctx, (rknn_core_mask)core_mask);
        if (ret != RKNN_SUCC)
            printf("[clip] rknn_set_core_mask(%d) -> %s (忽略)\n", core_mask, rknn_strerror(ret));
    }

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input < 1 || io_num.n_output < 1) {
        if (err) *err = "clip_text: query in/out failed";
        release();
        return false;
    }
    n_input_ = (int)io_num.n_input;

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    if (ret != RKNN_SUCC || in_attr.n_dims < 2) {
        if (err) *err = "clip_text: input must be [batch, sequence_len]";
        release();
        return false;
    }
    batch_ = (int)in_attr.dims[0];
    sequence_len_ = (int)in_attr.dims[1];

    rknn_tensor_attr out_attr;
    memset(&out_attr, 0, sizeof(out_attr));
    out_attr.index = 0;
    ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
    if (ret != RKNN_SUCC || out_attr.n_dims < 2) {
        if (err) *err = "clip_text: output must be [batch, embed_dim]";
        release();
        return false;
    }
    embed_dim_ = (int)out_attr.dims[out_attr.n_dims - 1];

    if (batch_ <= 0 || sequence_len_ <= 0 || embed_dim_ <= 0) {
        if (err) *err = "clip_text: bad input/output dims";
        release();
        return false;
    }

    printf("[clip] loaded %s, batch=%d seq_len=%d embed_dim=%d (in=%s out=%s)\n", model_path.c_str(),
           batch_, sequence_len_, embed_dim_, get_type_string(in_attr.type),
           get_type_string(out_attr.type));
    return true;
}

bool ClipTextEncoder::encode(const std::vector<std::string>& texts, std::vector<float>* out,
                             std::string* err) {
    if (!ctx_ || !tok_) {
        if (err) *err = "clip_text: not loaded";
        return false;
    }
    if (!out) return false;
    out->assign(texts.size() * (size_t)embed_dim_, 0.f);
    if (texts.empty()) return true;

    std::vector<int32_t> tokens((size_t)sequence_len_ * batch_);
    for (size_t t = 0; t < texts.size(); t += (size_t)batch_) {
        const size_t n = std::min<size_t>((size_t)batch_, texts.size() - t);
        for (size_t b = 0; b < n; ++b) {
            // 逐条 tokenize: BOS + 文本 + EOS + PAD, 长度正好 sequence_len_。
            const std::vector<int> ids = tok_->tokenize(texts[t + b], (size_t)sequence_len_, true);
            int32_t* dst = tokens.data() + b * (size_t)sequence_len_;
            for (int i = 0; i < sequence_len_; ++i)
                dst[i] = i < (int)ids.size() ? (int32_t)ids[(size_t)i] : (int32_t)PAD_TOKEN_ID;
        }

        rknn_input input;
        memset(&input, 0, sizeof(input));
        input.index = 0;
        input.buf = tokens.data();
        input.size = (uint32_t)(tokens.size() * sizeof(int32_t));
        input.type = RKNN_TENSOR_INT32;
        input.fmt = RKNN_TENSOR_UNDEFINED;

        int ret = rknn_inputs_set((rknn_context)ctx_, (uint32_t)n_input_, &input);
        if (ret != RKNN_SUCC) {
            if (err) *err = std::string("clip_text: rknn_inputs_set failed: ") + rknn_strerror(ret);
            return false;
        }
        ret = rknn_run((rknn_context)ctx_, nullptr);
        if (ret != RKNN_SUCC) {
            if (err) *err = std::string("clip_text: rknn_run failed: ") + rknn_strerror(ret);
            return false;
        }

        rknn_output output;
        memset(&output, 0, sizeof(output));
        output.index = 0;
        output.want_float = 1;
        ret = rknn_outputs_get((rknn_context)ctx_, 1, &output, nullptr);
        if (ret != RKNN_SUCC) {
            if (err) *err = std::string("clip_text: rknn_outputs_get failed: ") + rknn_strerror(ret);
            return false;
        }
        // 输出是 float32, 取前 embed_dim 个数 (batch 维度通常是 1)。
        const size_t want = (size_t)embed_dim_ * sizeof(float);
        const size_t have = (size_t)output.size;
        if (have >= want) {
            for (size_t b = 0; b < n; ++b) {
                memcpy(out->data() + (t + b) * (size_t)embed_dim_,
                       (const float*)output.buf + b * (size_t)embed_dim_, want);
            }
        }
        rknn_outputs_release((rknn_context)ctx_, 1, &output);
        if (have < want) {
            if (err) *err = "clip_text: output too small";
            return false;
        }
    }
    return true;
}

}  // namespace mb
