#include "infer/rknn_engine.h"

#include <cstdio>
#include <cstring>

#include "rknn_api.h"

namespace mb {
namespace {

// yolov8 RKNN 优化格式是 9 个输出 (3 个 stride x box/cls/cls_sum), 留一些余量。
constexpr int kMaxOutputs = 16;

// 把文件整个读进内存。rknn_init 支持从 buffer 加载, 这样不用关心 fd 生命周期。
bool read_model(const std::string& path, std::vector<uint8_t>* out, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "open model failed: " + path;
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err) *err = "seek model failed: " + path;
        return false;
    }
    long len = ftell(f);
    if (len <= 0) {
        fclose(f);
        if (err) *err = "empty model: " + path;
        return false;
    }
    rewind(f);
    out->resize((size_t)len);
    const size_t got = fread(out->data(), 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        if (err) *err = "short read model: " + path;
        return false;
    }
    return true;
}

const char* rknn_strerror(int ret) {
    switch (ret) {
        case RKNN_SUCC: return "RKNN_SUCC";
        case RKNN_ERR_FAIL: return "RKNN_ERR_FAIL";
        case RKNN_ERR_TIMEOUT: return "RKNN_ERR_TIMEOUT";
        case RKNN_ERR_DEVICE_UNAVAILABLE: return "RKNN_ERR_DEVICE_UNAVAILABLE";
        case RKNN_ERR_MALLOC_FAIL: return "RKNN_ERR_MALLOC_FAIL";
        case RKNN_ERR_PARAM_INVALID: return "RKNN_ERR_PARAM_INVALID";
        case RKNN_ERR_MODEL_INVALID: return "RKNN_ERR_MODEL_INVALID";
        case RKNN_ERR_CTX_INVALID: return "RKNN_ERR_CTX_INVALID";
        case RKNN_ERR_INPUT_INVALID: return "RKNN_ERR_INPUT_INVALID";
        case RKNN_ERR_OUTPUT_INVALID: return "RKNN_ERR_OUTPUT_INVALID";
        case RKNN_ERR_DEVICE_UNMATCH: return "RKNN_ERR_DEVICE_UNMATCH";
        case RKNN_ERR_INCOMPATILE_PRE_COMPILE_MODEL: return "RKNN_ERR_INCOMPATILE_PRE_COMPILE_MODEL";
        case RKNN_ERR_INCOMPATILE_OPTIMIZATION_LEVEL_VERSION:
            return "RKNN_ERR_INCOMPATILE_OPTIMIZATION_LEVEL_VERSION";
        case RKNN_ERR_TARGET_PLATFORM_UNMATCH: return "RKNN_ERR_TARGET_PLATFORM_UNMATCH";
        default: return "RKNN_ERR_UNKNOWN";
    }
}

}  // namespace

RknnEngine::~RknnEngine() { release(); }

void RknnEngine::release() {
    if (ctx_) {
        rknn_destroy((rknn_context)ctx_);
        ctx_ = nullptr;
    }
    outputs_.clear();
    model_w_ = model_h_ = model_c_ = 0;
    is_quant_ = false;
    n_input_ = 1;
    aux_n_dims_ = 0;
    memset(aux_dims_, 0, sizeof(aux_dims_));
    aux_elems_ = 0;
    aux_bytes_ = 0;
    aux_type_ = 0;
    aux_fmt_ = 0;
    aux_buf_.clear();
}

void RknnEngine::print_versions() const {
    if (!ctx_) return;
    rknn_sdk_version ver;
    memset(&ver, 0, sizeof(ver));
    if (rknn_query((rknn_context)ctx_, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC) {
        printf("[rknn] api_version : %s\n", ver.api_version);
        printf("[rknn] drv_version : %s\n", ver.drv_version);
    }
    printf("[rknn] model input : %dx%d c=%d, quant=%d\n", model_w_, model_h_, model_c_, (int)is_quant_);
}

bool RknnEngine::load(const std::string& model_path, int core_mask, std::string* err) {
    release();

    std::vector<uint8_t> model;
    if (!read_model(model_path, &model, err)) return false;

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model.data(), (uint32_t)model.size(), 0, nullptr);
    if (ret != RKNN_SUCC) {
        if (err) {
            *err = std::string("rknn_init failed: ") + rknn_strerror(ret);
            if (ret == RKNN_ERR_MODEL_INVALID || ret == RKNN_ERR_DEVICE_UNMATCH) {
                *err += " (模型与 librknnrt 版本不匹配? 必须同为 2.3.2)";
            }
        }
        return false;
    }
    ctx_ = (void*)(uintptr_t)ctx;

    // core_mask 只在多核 NPU 上有效, 失败不致命。
    if (core_mask >= 0) {
        ret = rknn_set_core_mask(ctx, (rknn_core_mask)core_mask);
        if (ret != RKNN_SUCC) {
            printf("[rknn] rknn_set_core_mask(%d) -> %s (忽略)\n", core_mask, rknn_strerror(ret));
        }
    }

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        if (err) *err = std::string("RKNN_QUERY_IN_OUT_NUM failed: ") + rknn_strerror(ret);
        release();
        return false;
    }
    if (io_num.n_input != 1 && io_num.n_input != 2) {
        if (err) *err = "unsupported input count (1 or 2 expected)";
        release();
        return false;
    }
    n_input_ = (int)io_num.n_input;
    if (io_num.n_output == 0 || io_num.n_output > kMaxOutputs) {
        if (err) *err = "unsupported output count";
        release();
        return false;
    }

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    if (ret != RKNN_SUCC) {
        if (err) *err = std::string("RKNN_QUERY_INPUT_ATTR failed: ") + rknn_strerror(ret);
        release();
        return false;
    }

    // RV1106/RK3576 的逻辑与 zoo 一致: NCHW 时 CHW 在 dims[1..3], NHWC 时 HWC 在 dims[1..3]。
    if (in_attr.fmt == RKNN_TENSOR_NCHW) {
        model_c_ = (int)in_attr.dims[1];
        model_h_ = (int)in_attr.dims[2];
        model_w_ = (int)in_attr.dims[3];
    } else {
        model_h_ = (int)in_attr.dims[1];
        model_w_ = (int)in_attr.dims[2];
        model_c_ = (int)in_attr.dims[3];
    }
    if (model_w_ <= 0 || model_h_ <= 0 || model_c_ <= 0) {
        if (err) *err = "bad input dims";
        release();
        return false;
    }

    // 第二个输入: 开放词表模型的文本嵌入。维度/类型从模型读, 由调用方按同一格式喂数据
    // (yolo_world 是 FLOAT32 [1, 80, 512], clip_text 是 INT32 [1, 20])。
    if (n_input_ == 2) {
        rknn_tensor_attr aux_attr;
        memset(&aux_attr, 0, sizeof(aux_attr));
        aux_attr.index = 1;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &aux_attr, sizeof(aux_attr));
        if (ret != RKNN_SUCC) {
            if (err) *err = std::string("RKNN_QUERY_INPUT_ATTR[1] failed: ") + rknn_strerror(ret);
            release();
            return false;
        }
        aux_n_dims_ = aux_attr.n_dims;
        for (uint32_t d = 0; d < aux_attr.n_dims && d < 4; ++d) aux_dims_[d] = aux_attr.dims[d];
        aux_elems_ = aux_attr.n_elems;
        aux_bytes_ = aux_attr.size;
        aux_type_ = (int32_t)aux_attr.type;
        aux_fmt_ = (int32_t)aux_attr.fmt;
        if (aux_elems_ == 0 || aux_bytes_ == 0) {
            if (err) *err = "input[1] has zero size";
            release();
            return false;
        }
    }

    outputs_.assign(io_num.n_output, OutputTensor{});
    std::vector<rknn_tensor_attr> out_attrs(io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        memset(&out_attrs[i], 0, sizeof(rknn_tensor_attr));
        out_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            if (err) *err = std::string("RKNN_QUERY_OUTPUT_ATTR failed: ") + rknn_strerror(ret);
            release();
            return false;
        }
        outputs_[i].zp = out_attrs[i].zp;
        outputs_[i].scale = out_attrs[i].scale;
        outputs_[i].fl = (float)out_attrs[i].fl;
        outputs_[i].n_dims = out_attrs[i].n_dims;
        for (uint32_t d = 0; d < out_attrs[i].n_dims && d < 4; ++d) {
            outputs_[i].dims[d] = out_attrs[i].dims[d];
        }
    }

    // zoo 的判定: 首个输出是 affine 量化且不是 float16 → 走 int8 解码路径。
    // 这里再要求所有输出量化参数一致, 否则 int8 解码不可靠。
    is_quant_ = (out_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                 out_attrs[0].type != RKNN_TENSOR_FLOAT16);
    if (is_quant_) {
        for (uint32_t i = 1; i < io_num.n_output; ++i) {
            if (out_attrs[i].qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
                out_attrs[i].type == RKNN_TENSOR_FLOAT16) {
                is_quant_ = false;
                break;
            }
        }
    }

    printf("[rknn] loaded %s (%zu bytes), in=%dx%dx%d, ins=%d, outs=%u, quant=%d\n", model_path.c_str(),
           model.size(), model_w_, model_h_, model_c_, n_input_, io_num.n_output, (int)is_quant_);
    if (n_input_ == 2) {
        printf("[rknn]   in[1] dims=[%u,%u,%u,%u] type=%d fmt=%d bytes=%zu\n", aux_dims_[0], aux_dims_[1],
               aux_dims_[2], aux_dims_[3], (int)aux_type_, (int)aux_fmt_, aux_bytes_);
    }
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        printf("[rknn]   out[%u] dims=[%u,%u,%u,%u] zp=%d scale=%f type=%d\n", i, out_attrs[i].dims[0],
               out_attrs[i].dims[1], out_attrs[i].dims[2], out_attrs[i].dims[3], out_attrs[i].zp,
               out_attrs[i].scale, (int)out_attrs[i].type);
    }
    return true;
}

bool RknnEngine::set_aux_input(const void* data, size_t bytes, std::string* err) {
    if (!ctx_) {
        if (err) *err = "engine not loaded";
        return false;
    }
    if (n_input_ != 2) {
        if (err) *err = "model has only 1 input";
        return false;
    }
    // 模型把文本输入也量化成了 int8 (yolo_world 的 in[1] 就是), 但 rknn_inputs_set
    // 接受 float32 并按 zp/scale 自己转 —— 和 pass_through=0 的图像输入同一个机制。
    // 所以缓冲区按"元素个数 x 4 字节"算, 不看模型的 aux_bytes_。
    const size_t want = (size_t)aux_elems_ * sizeof(float);
    if (!data || bytes != want) {
        if (err) {
            *err = "aux input size " + std::to_string(bytes) + " != expected " +
                   std::to_string(want) + " (" + std::to_string(aux_elems_) + " x float32)";
        }
        return false;
    }
    aux_buf_.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    return true;
}

bool RknnEngine::run(const uint8_t* rgb, size_t bytes, std::string* err) {
    if (!ctx_) {
        if (err) *err = "engine not loaded";
        return false;
    }
    const size_t want = (size_t)model_w_ * model_h_ * model_c_;
    if (!rgb || bytes < want) {
        if (err) *err = "input buffer too small";
        return false;
    }
    if (n_input_ == 2 && aux_buf_.empty()) {
        if (err) *err = "second input not set (call set_aux_input first)";
        return false;
    }

    rknn_input inputs[2];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].buf = const_cast<uint8_t*>(rgb);
    inputs[0].size = (uint32_t)want;
    inputs[0].pass_through = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;

    if (n_input_ == 2) {
        inputs[1].index = 1;
        inputs[1].buf = aux_buf_.data();
        inputs[1].size = (uint32_t)aux_buf_.size();
        inputs[1].pass_through = 0;
        // 一律按 float32 喂; 模型输入若是量化格式, 由 runtime 按 zp/scale 转换。
        inputs[1].type = RKNN_TENSOR_FLOAT32;
        inputs[1].fmt = RKNN_TENSOR_UNDEFINED;
    }

    int ret = rknn_inputs_set((rknn_context)ctx_, (uint32_t)n_input_, inputs);
    if (ret != RKNN_SUCC) {
        if (err) *err = std::string("rknn_inputs_set failed: ") + rknn_strerror(ret);
        return false;
    }

    ret = rknn_run((rknn_context)ctx_, nullptr);
    if (ret != RKNN_SUCC) {
        if (err) *err = std::string("rknn_run failed: ") + rknn_strerror(ret);
        return false;
    }

    const uint32_t n = (uint32_t)outputs_.size();
    std::vector<rknn_output> outs(n);
    memset(outs.data(), 0, sizeof(rknn_output) * n);
    for (uint32_t i = 0; i < n; ++i) {
        outs[i].index = i;
        outs[i].want_float = is_quant_ ? 0 : 1;
        outs[i].is_prealloc = 0;
    }

    ret = rknn_outputs_get((rknn_context)ctx_, n, outs.data(), nullptr);
    if (ret != RKNN_SUCC) {
        if (err) *err = std::string("rknn_outputs_get failed: ") + rknn_strerror(ret);
        return false;
    }

    for (uint32_t i = 0; i < n; ++i) {
        outputs_[i].data.resize(outs[i].size);
        if (outs[i].buf && outs[i].size) {
            memcpy(outputs_[i].data.data(), outs[i].buf, outs[i].size);
        }
    }
    rknn_outputs_release((rknn_context)ctx_, n, outs.data());
    return true;
}

}  // namespace mb
