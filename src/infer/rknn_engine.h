#ifndef MONITOR_BOX_RKNN_ENGINE_H_
#define MONITOR_BOX_RKNN_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "infer/types.h"

namespace mb {

// RKNN 运行时封装。
//
// 版本纪律 (别改): 头文件 third_party/rknn/rknn_api.h、链接的
// prebuilds/sysroot/usr/lib/librknnrt.so (2.3.2, 429f97ae6b@2025-04-09)、
// 以及转换模型用的 rknn-toolkit2 三者都必须是 2.3.2。
// 千万不要链 rknn_model_zoo/3rdparty 下那份 2.5.0 —— 模型会报版本不匹配。
class RknnEngine {
public:
    RknnEngine() = default;
    ~RknnEngine();

    RknnEngine(const RknnEngine&) = delete;
    RknnEngine& operator=(const RknnEngine&) = delete;

    // 加载 .rknn 模型。core_mask 取值见 rknn_core_mask (0 = AUTO)。
    bool load(const std::string& model_path, int core_mask, std::string* err);
    void release();

    bool ok() const { return ctx_ != 0; }

    // 模型输入尺寸与通道数 (来自 RKNN_QUERY_INPUT_ATTR)。
    int input_width() const { return model_w_; }
    int input_height() const { return model_h_; }
    int input_channel() const { return model_c_; }

    // 输入个数。yolov8/yolov5/姿态都是 1; 开放词表检测 (yolo_world) 是 2:
    //   in0 = UINT8 NHWC 图像, in1 = FLOAT32 文本嵌入 [1, 80, 512]
    int input_count() const { return n_input_; }
    // 第二个输入的维度个数与各维长度 (单输入模型返回 0)。dims 是元素个数, 不是字节数。
    uint32_t aux_input_n_dims() const { return aux_n_dims_; }
    uint32_t aux_input_dim(uint32_t i) const { return i < 4 ? aux_dims_[i] : 0; }
    // 第二个输入的元素个数。喂进去的数据一律是 float32 (n_elems 个), 见 set_aux_input()。
    uint32_t aux_input_elems() const { return aux_elems_; }

    // 量化模型 = 输出为 int8 + affine 量化, 后处理按 i8 解码。
    bool is_quant() const { return is_quant_; }

    // 一个输出张量。data 是裸字节 (量化模型下即 int8), dims 是 4 维 shape。
    struct OutputTensor {
        std::vector<uint8_t> data;
        uint32_t dims[4] = {0, 0, 0, 0};
        uint32_t n_dims = 0;
        int32_t zp = 0;
        float scale = 1.0f;
        float fl = 0.0f;
    };

    // 第二个输入 (开放词表模型的文本嵌入)。data 必须是 aux_input_elems() 个 float32,
    // 模型内部输入若是 int8/int16 量化格式, 由 rknn_inputs_set 按 zp/scale 自己转换 ——
    // 所以这里不要求调用方关心模型的输入类型, 一律喂 float32。
    // 通常在换提示词时设一次, 之后每帧 run 复用。单输入模型调用返回 false。
    bool set_aux_input(const void* data, size_t bytes, std::string* err);
    bool has_aux_input() const { return !aux_buf_.empty(); }
    void clear_aux_input() { aux_buf_.clear(); }

    // rgb 必须是 RGB888 / NHWC / uint8, 尺寸等于 input_width/height。
    // 双输入模型必须先 set_aux_input()。
    bool run(const uint8_t* rgb, size_t bytes, std::string* err);

    // 只在下一次 run() 之前有效。
    const std::vector<OutputTensor>& outputs() const { return outputs_; }

    // 打印 api/driver 版本, 上板时一眼确认链路版本是否对齐。
    void print_versions() const;

private:
    void* ctx_ = nullptr;  // rknn_context

    int model_w_ = 0;
    int model_h_ = 0;
    int model_c_ = 0;
    bool is_quant_ = false;

    int n_input_ = 1;
    uint32_t aux_n_dims_ = 0;
    uint32_t aux_dims_[4] = {0, 0, 0, 0};
    uint32_t aux_elems_ = 0;  // 元素个数
    size_t aux_bytes_ = 0;    // 模型侧输入的字节数 (量化模型 = elems, 仅打印用)
    int32_t aux_type_ = 0;  // rknn_tensor_type
    int32_t aux_fmt_ = 0;   // rknn_tensor_format
    std::vector<uint8_t> aux_buf_;

    std::vector<OutputTensor> outputs_;
};

}  // namespace mb

#endif  // MONITOR_BOX_RKNN_ENGINE_H_
