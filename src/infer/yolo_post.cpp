#include "infer/yolo_post.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace mb {
namespace {

// YOLOv5 的 3 组 anchor, 按 stride 8/16/32 排列 (与训练 cfg 一致, 别改顺序)。
const int kAnchor[3][6] = {{10, 13, 16, 30, 33, 23},
                           {30, 61, 62, 45, 59, 119},
                           {116, 90, 156, 198, 373, 326}};

// 每个 anchor 的输出长度: tx,ty,tw,th,obj + 80 类概率。
constexpr int kPropBoxSize = 5 + kYoloClassNum;

inline int clamp(float val, int min, int max) { return val > min ? (val < max ? val : max) : min; }

inline int32_t clip_i32(float val, float min, float max) {
    float f = val <= min ? min : (val >= max ? max : val);
    return (int32_t)f;
}

inline int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale) {
    return (int8_t)clip_i32(f32 / scale + zp, -128, 127);
}

inline float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

// zoo 的 IoU: 注意 +1.0 像素, 和标准 IoU 略有差别。为了和官方例程对齐保持一致。
float calculate_overlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1,
                        float xmax1, float ymax1) {
    const float w = std::fmax(0.f, std::fmin(xmax0, xmax1) - std::fmax(xmin0, xmin1) + 1.0f);
    const float h = std::fmax(0.f, std::fmin(ymax0, ymax1) - std::fmax(ymin0, ymin1) + 1.0f);
    const float i = w * h;
    const float u = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) +
                    (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - i;
    return u <= 0.f ? 0.f : (i / u);
}

// 按置信度降序做一次置换排序, 同时搬 boxes/probs 和索引。
void quick_sort_desc(std::vector<float>& probs, int left, int right, std::vector<int>& indices) {
    int low = left;
    int high = right;
    if (left >= right) return;

    const int key_index = indices[left];
    const float key = probs[left];
    while (low < high) {
        while (low < high && probs[high] <= key) --high;
        probs[low] = probs[high];
        indices[low] = indices[high];
        while (low < high && probs[low] >= key) ++low;
        probs[high] = probs[low];
        indices[high] = indices[low];
    }
    probs[low] = key;
    indices[low] = key_index;
    quick_sort_desc(probs, left, low - 1, indices);
    quick_sort_desc(probs, low + 1, right, indices);
}

// 单类 NMS: order 里 -1 表示被抑制或已处理。
void nms(int valid_count, const std::vector<float>& boxes, const std::vector<int>& class_ids,
         std::vector<int>& order, int filter_id, float threshold) {
    for (int i = 0; i < valid_count; ++i) {
        const int n = order[i];
        if (n == -1 || class_ids[n] != filter_id) continue;
        for (int j = i + 1; j < valid_count; ++j) {
            const int m = order[j];
            if (m == -1 || class_ids[m] != filter_id) continue;

            const float xmin0 = boxes[n * 4 + 0];
            const float ymin0 = boxes[n * 4 + 1];
            const float xmax0 = xmin0 + boxes[n * 4 + 2];
            const float ymax0 = ymin0 + boxes[n * 4 + 3];
            const float xmin1 = boxes[m * 4 + 0];
            const float ymin1 = boxes[m * 4 + 1];
            const float xmax1 = xmin1 + boxes[m * 4 + 2];
            const float ymax1 = ymin1 + boxes[m * 4 + 3];

            if (calculate_overlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1) > threshold) {
                order[j] = -1;
            }
        }
    }
}

// int8 解码一个输出头。boxes 存 (x,y,w,h) 相对模型输入画布的浮点坐标。
int process_i8(const int8_t* input, const int* anchor, int grid_h, int grid_w, int height, int width,
               int stride, std::vector<float>& boxes, std::vector<float>& probs,
               std::vector<int>& class_ids, float threshold, int32_t zp, float scale) {
    const int grid_len = grid_h * grid_w;
    const int8_t thres_i8 = qnt_f32_to_affine(threshold, zp, scale);
    int count = 0;

    for (int a = 0; a < 3; ++a) {
        for (int i = 0; i < grid_h; ++i) {
            for (int j = 0; j < grid_w; ++j) {
                const int8_t box_confidence = input[(kPropBoxSize * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence < thres_i8) continue;

                const int offset = (kPropBoxSize * a) * grid_len + i * grid_w + j;
                const int8_t* in_ptr = input + offset;

                float box_x = deqnt_affine_to_f32(*in_ptr, zp, scale) * 2.0f - 0.5f;
                float box_y = deqnt_affine_to_f32(in_ptr[grid_len], zp, scale) * 2.0f - 0.5f;
                float box_w = deqnt_affine_to_f32(in_ptr[2 * grid_len], zp, scale) * 2.0f;
                float box_h = deqnt_affine_to_f32(in_ptr[3 * grid_len], zp, scale) * 2.0f;

                box_x = (box_x + (float)j) * (float)stride;
                box_y = (box_y + (float)i) * (float)stride;
                // tw/th 的平方是 YOLOv5 的约定 —— 不是笔误。
                box_w = box_w * box_w * (float)anchor[a * 2];
                box_h = box_h * box_h * (float)anchor[a * 2 + 1];

                box_x -= (box_w / 2.0f);
                box_y -= (box_h / 2.0f);

                int8_t max_class_probs = in_ptr[5 * grid_len];
                int max_class_id = 0;
                for (int k = 1; k < kYoloClassNum; ++k) {
                    const int8_t prob = in_ptr[(5 + k) * grid_len];
                    if (prob > max_class_probs) {
                        max_class_id = k;
                        max_class_probs = prob;
                    }
                }

                const float limit_score = deqnt_affine_to_f32(max_class_probs, zp, scale) *
                                          deqnt_affine_to_f32(box_confidence, zp, scale);
                if (limit_score < threshold) continue;

                boxes.push_back(box_x);
                boxes.push_back(box_y);
                boxes.push_back(box_w);
                boxes.push_back(box_h);
                probs.push_back(limit_score);
                class_ids.push_back(max_class_id);
                ++count;
            }
        }
    }
    (void)height;
    (void)width;
    return count;
}

// 非量化 (fp32) 输出, 仅在模型不是 int8 时用。
int process_fp32(const float* input, const int* anchor, int grid_h, int grid_w, int stride,
                 std::vector<float>& boxes, std::vector<float>& probs, std::vector<int>& class_ids,
                 float threshold) {
    const int grid_len = grid_h * grid_w;
    int count = 0;

    for (int a = 0; a < 3; ++a) {
        for (int i = 0; i < grid_h; ++i) {
            for (int j = 0; j < grid_w; ++j) {
                const float box_confidence = input[(kPropBoxSize * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence < threshold) continue;

                const int offset = (kPropBoxSize * a) * grid_len + i * grid_w + j;
                const float* in_ptr = input + offset;

                float box_x = (*in_ptr * 2.0f) - 0.5f;
                float box_y = (in_ptr[grid_len] * 2.0f) - 0.5f;
                float box_w = in_ptr[2 * grid_len] * 2.0f;
                float box_h = in_ptr[3 * grid_len] * 2.0f;

                box_x = (box_x + (float)j) * (float)stride;
                box_y = (box_y + (float)i) * (float)stride;
                box_w = box_w * box_w * (float)anchor[a * 2];
                box_h = box_h * box_h * (float)anchor[a * 2 + 1];

                box_x -= (box_w / 2.0f);
                box_y -= (box_h / 2.0f);

                float max_class_probs = in_ptr[5 * grid_len];
                int max_class_id = 0;
                for (int k = 1; k < kYoloClassNum; ++k) {
                    const float prob = in_ptr[(5 + k) * grid_len];
                    if (prob > max_class_probs) {
                        max_class_id = k;
                        max_class_probs = prob;
                    }
                }

                const float limit_score = max_class_probs * box_confidence;
                if (limit_score < threshold) continue;

                boxes.push_back(box_x);
                boxes.push_back(box_y);
                boxes.push_back(box_w);
                boxes.push_back(box_h);
                probs.push_back(limit_score);
                class_ids.push_back(max_class_id);
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

YoloHead yolo_head_kind(const RknnEngine& engine) {
    const auto& outs = engine.outputs();

    // 开放词表模型靠输入个数区分, 不看输出排布: 图像之外还要一份文本嵌入 (in1)。
    // 每个 stride 一组 [score(nc), box(4) (, score_sum)], 与 v8 的 [box, cls (, cls_sum)] 相反,
    // 只能靠输入个数判定 (nc == 4 的 v8 模型和它长得一样)。
    if (engine.input_count() == 2) {
        if (outs.size() != 6 && outs.size() != 9) return YoloHead::Unknown;
        const size_t per = outs.size() / 3;
        for (size_t i = 0; i < outs.size(); i += per) {
            const auto& score = outs[i];
            const auto& box = outs[i + 1];
            if (score.n_dims < 4 || box.n_dims < 4) return YoloHead::Unknown;
            if (score.dims[1] == 0 || box.dims[1] != 4) return YoloHead::Unknown;
            if (score.dims[2] != box.dims[2] || score.dims[3] != box.dims[3]) return YoloHead::Unknown;
            if (score.dims[1] != outs[0].dims[1]) return YoloHead::Unknown;
            if (per == 3 && outs[i + 2].dims[1] != 1) return YoloHead::Unknown;
        }
        return YoloHead::YoloWorld;
    }

    if (outs.size() == 3) return YoloHead::YoloV5;
    if (outs.size() != 6 && outs.size() != 9) return YoloHead::Unknown;
    const size_t per = outs.size() / 3;
    for (size_t i = 0; i < outs.size(); i += per) {
        const auto& box = outs[i];
        const auto& cls = outs[i + 1];
        if (box.n_dims < 4 || cls.n_dims < 4) return YoloHead::Unknown;
        if (box.dims[1] == 0 || box.dims[1] % 4 != 0 || cls.dims[1] == 0) return YoloHead::Unknown;
        if (box.dims[2] != cls.dims[2] || box.dims[3] != cls.dims[3]) return YoloHead::Unknown;
        if (cls.dims[1] != outs[1].dims[1]) return YoloHead::Unknown;
        if (per == 3 && outs[i + 2].dims[1] != 1) return YoloHead::Unknown;
    }
    return YoloHead::YoloV8;
}

const char* yolo_head_name(YoloHead h) {
    switch (h) {
        case YoloHead::YoloV5: return "yolov5";
        case YoloHead::YoloV8: return "yolov8";
        case YoloHead::YoloWorld: return "yolo_world";
        default: return "unknown";
    }
}

int yolo_num_classes(const RknnEngine& engine) {
    switch (yolo_head_kind(engine)) {
        case YoloHead::YoloV5: return kYoloClassNum;
        case YoloHead::YoloV8: return (int)engine.outputs()[1].dims[1];
        // 开放词表: 类号 = 提示词槽位号, 槽位数 = 分数张量的通道数 (默认 80)。
        case YoloHead::YoloWorld: return (int)engine.outputs()[0].dims[1];
        default: return 0;
    }
}

namespace {

// DFL: 每条边 reg_max 个 bin 做 softmax 后取期望, 得到到网格中心的距离 (单位: stride)。
void compute_dfl(const float* t, int dfl_len, float* box) {
    for (int b = 0; b < 4; ++b) {
        const float* p = t + b * dfl_len;
        float mx = p[0];
        for (int i = 1; i < dfl_len; ++i) mx = std::max(mx, p[i]);
        float sum = 0.f, acc = 0.f;
        for (int i = 0; i < dfl_len; ++i) {
            const float e = std::exp(p[i] - mx);
            sum += e;
            acc += e * (float)i;
        }
        box[b] = acc / sum;
    }
}

float tensor_at(const RknnEngine::OutputTensor& t, bool quant, size_t idx) {
    if (quant) return deqnt_affine_to_f32(reinterpret_cast<const int8_t*>(t.data.data())[idx], t.zp, t.scale);
    return reinterpret_cast<const float*>(t.data.data())[idx];
}

// 一个 stride 的 v8 输出: box (4*dfl_len, H, W) + cls (nc, H, W) + 可选 cls_sum (1, H, W)。
// boxes 存 (x, y, w, h), 与 v5 路径一致, 后面共用 NMS/反 letterbox。
bool process_v8(const RknnEngine::OutputTensor& box_t, const RknnEngine::OutputTensor& cls_t,
                const RknnEngine::OutputTensor* sum_t, bool quant, int model_in_h, float threshold,
                std::vector<float>& boxes, std::vector<float>& probs, std::vector<int>& class_ids,
                int* count, std::string* err) {
    const int grid_h = (int)cls_t.dims[2];
    const int grid_w = (int)cls_t.dims[3];
    const int grid_len = grid_h * grid_w;
    const int nc = (int)cls_t.dims[1];
    const int dfl_len = (int)box_t.dims[1] / 4;
    const int stride = grid_h > 0 ? model_in_h / grid_h : 0;
    const size_t elem = quant ? 1 : 4;
    if (stride <= 0 || box_t.data.size() < (size_t)dfl_len * 4 * grid_len * elem ||
        cls_t.data.size() < (size_t)nc * grid_len * elem ||
        (sum_t && sum_t->data.size() < (size_t)grid_len * elem)) {
        if (err) *err = "yolov8 output tensor size mismatch";
        return false;
    }

    // 阈值先换算到各自张量的量化域, 绝大多数格子在整数比较时就被跳过。
    const int8_t* cls_i8 = reinterpret_cast<const int8_t*>(cls_t.data.data());
    const int8_t* sum_i8 = sum_t ? reinterpret_cast<const int8_t*>(sum_t->data.data()) : nullptr;
    const int8_t cls_thr_i8 = qnt_f32_to_affine(threshold, cls_t.zp, cls_t.scale);
    const int8_t sum_thr_i8 = sum_t ? qnt_f32_to_affine(threshold, sum_t->zp, sum_t->scale) : 0;

    std::vector<float> before_dfl((size_t)dfl_len * 4);
    for (int i = 0; i < grid_h; ++i) {
        for (int j = 0; j < grid_w; ++j) {
            const int off = i * grid_w + j;
            if (sum_t) {
                if (quant ? sum_i8[off] < sum_thr_i8 : tensor_at(*sum_t, false, off) < threshold) continue;
            }

            int best = -1;
            float best_score = threshold;
            if (quant) {
                int8_t best_q = cls_thr_i8;
                for (int c = 0; c < nc; ++c) {
                    const int8_t q = cls_i8[c * grid_len + off];
                    if (q >= best_q && (best < 0 || q > best_q)) {
                        best_q = q;
                        best = c;
                    }
                }
                if (best >= 0) best_score = deqnt_affine_to_f32(best_q, cls_t.zp, cls_t.scale);
            } else {
                for (int c = 0; c < nc; ++c) {
                    const float s = tensor_at(cls_t, false, (size_t)c * grid_len + off);
                    if (s >= best_score) {
                        best_score = s;
                        best = c;
                    }
                }
            }
            if (best < 0 || best_score < threshold) continue;

            for (int k = 0; k < dfl_len * 4; ++k) {
                before_dfl[k] = tensor_at(box_t, quant, (size_t)k * grid_len + off);
            }
            float d[4];
            compute_dfl(before_dfl.data(), dfl_len, d);
            const float x1 = (-d[0] + j + 0.5f) * stride;
            const float y1 = (-d[1] + i + 0.5f) * stride;
            const float x2 = (d[2] + j + 0.5f) * stride;
            const float y2 = (d[3] + i + 0.5f) * stride;
            boxes.push_back(x1);
            boxes.push_back(y1);
            boxes.push_back(x2 - x1);
            boxes.push_back(y2 - y1);
            probs.push_back(best_score);
            class_ids.push_back(best);
            ++*count;
        }
    }
    return true;
}

// 排序 + 逐类 NMS + 反 letterbox。score_by_rank 只为 v5 保留 zoo 的取分方式 (见调用处注释)。
void finish_detections(std::vector<float>& boxes, std::vector<float>& probs,
                       std::vector<int>& class_ids, int valid_count, const YoloParams& params,
                       const Letterbox& lb, int model_in_w, int model_in_h, bool score_by_rank,
                       std::vector<Detection>* out) {
    if (valid_count <= 0) return;
    const std::vector<float> raw_probs = probs;

    std::vector<int> order((size_t)valid_count);
    for (int i = 0; i < valid_count; ++i) order[i] = i;

    quick_sort_desc(probs, 0, valid_count - 1, order);

    std::set<int> class_set(class_ids.begin(), class_ids.end());
    for (int c : class_set) nms(valid_count, boxes, class_ids, order, c, params.nms_threshold);

    for (int i = 0; i < valid_count; ++i) {
        if (order[i] == -1) continue;
        if ((int)out->size() >= kMaxDetections) break;

        const int n = order[i];
        float x1 = boxes[n * 4 + 0] - (float)lb.x_pad;
        float y1 = boxes[n * 4 + 1] - (float)lb.y_pad;
        const float x2 = x1 + boxes[n * 4 + 2];
        const float y2 = y1 + boxes[n * 4 + 3];

        Detection d;
        d.cls_id = class_ids[n];
        // zoo 的 yolov5 这里取的是 objProbs[i] 而不是 [n] —— 排序后两者不同。为了与官方例程的分数
        // 分布完全一致 v5 先照抄; 但排序后 i 是 rank, 只能当近似置信度。见 architecture.zh-CN.md 备注。
        // v8 (火焰等按分数做阈值判定的模型) 用这个框自己的分数。
        d.score = score_by_rank ? probs[i] : raw_probs[n];
        d.x1 = (float)clamp(x1, 0, model_in_w) / lb.scale;
        d.y1 = (float)clamp(y1, 0, model_in_h) / lb.scale;
        d.x2 = (float)clamp(x2, 0, model_in_w) / lb.scale;
        d.y2 = (float)clamp(y2, 0, model_in_h) / lb.scale;
        out->push_back(d);
    }
}

// 一个 stride 的 yolo_world 输出: score (nc, H, W, 已 sigmoid) + box (4, H, W)。
// 注意分数张量在前。box 是到网格中心的 ltrb 距离 (单位: stride), 没有 DFL。
// 与 zoo 的 yolo_world 后处理对齐 (rknn_model_zoo/examples/yolo_world/cpp/postprocess.cc):
// 每个格子只取分数最高的那一类, 不同类在同一格子上不会各出一个框。
bool process_world(const RknnEngine::OutputTensor& score_t, const RknnEngine::OutputTensor& box_t,
                   bool quant, int model_in_h, int max_classes, float threshold,
                   std::vector<float>& boxes, std::vector<float>& probs, std::vector<int>& class_ids,
                   int* count, std::string* err) {
    const int grid_h = (int)box_t.dims[2];
    const int grid_w = (int)box_t.dims[3];
    const int grid_len = grid_h * grid_w;
    const int score_nc = (int)score_t.dims[1];
    int nc = score_nc;
    if (max_classes > 0 && nc > max_classes) nc = max_classes;
    const int stride = grid_h > 0 ? model_in_h / grid_h : 0;
    const size_t elem = quant ? 1 : 4;
    if (stride <= 0 || nc <= 0 || box_t.data.size() < (size_t)4 * grid_len * elem ||
        score_t.data.size() < (size_t)score_nc * grid_len * elem) {
        if (err) *err = "yolo_world output tensor size mismatch";
        return false;
    }

    const int8_t* score_i8 = reinterpret_cast<const int8_t*>(score_t.data.data());
    const int8_t thr_i8 = qnt_f32_to_affine(threshold, score_t.zp, score_t.scale);

    for (int i = 0; i < grid_h; ++i) {
        for (int j = 0; j < grid_w; ++j) {
            const size_t off = (size_t)i * grid_w + j;
            int best = -1;
            float best_score = threshold;
            if (quant) {
                int8_t best_q = thr_i8;
                for (int c = 0; c < nc; ++c) {
                    const int8_t q = score_i8[(size_t)c * grid_len + off];
                    if (q > best_q) {
                        best_q = q;
                        best = c;
                    }
                }
                if (best < 0) continue;
                best_score = deqnt_affine_to_f32(best_q, score_t.zp, score_t.scale);
            } else {
                for (int c = 0; c < nc; ++c) {
                    const float s = tensor_at(score_t, false, (size_t)c * grid_len + off);
                    if (s > best_score) {
                        best_score = s;
                        best = c;
                    }
                }
                if (best < 0) continue;
            }
            if (best_score < threshold) continue;

            const float d0 = tensor_at(box_t, quant, 0 * grid_len + off);
            const float d1 = tensor_at(box_t, quant, 1 * grid_len + off);
            const float d2 = tensor_at(box_t, quant, 2 * grid_len + off);
            const float d3 = tensor_at(box_t, quant, 3 * grid_len + off);
            const float x1 = (-d0 + (float)j + 0.5f) * (float)stride;
            const float y1 = (-d1 + (float)i + 0.5f) * (float)stride;
            const float x2 = (d2 + (float)j + 0.5f) * (float)stride;
            const float y2 = (d3 + (float)i + 0.5f) * (float)stride;

            boxes.push_back(x1);
            boxes.push_back(y1);
            boxes.push_back(x2 - x1);
            boxes.push_back(y2 - y1);
            probs.push_back(best_score);
            class_ids.push_back(best);
            ++*count;
        }
    }
    return true;
}

bool yoloworld_postprocess(const RknnEngine& engine, const YoloParams& params, const Letterbox& lb,
                           std::vector<Detection>* out, std::string* err) {
    const auto& outputs = engine.outputs();
    const size_t per = outputs.size() / 3;
    const bool quant = engine.is_quant();
    std::vector<float> boxes;
    std::vector<float> probs;
    std::vector<int> class_ids;
    int valid_count = 0;
    for (size_t i = 0; i < outputs.size(); i += per) {
        // i*per 是分数, i*per+1 是框 —— 与 v8 顺序相反。
        if (!process_world(outputs[i], outputs[i + 1], quant, engine.input_height(),
                           params.num_prompts, params.conf_threshold, boxes, probs, class_ids,
                           &valid_count, err)) {
            return false;
        }
    }
    finish_detections(boxes, probs, class_ids, valid_count, params, lb, engine.input_width(),
                      engine.input_height(), false, out);
    return true;
}

bool yolov8_postprocess(const RknnEngine& engine, const YoloParams& params, const Letterbox& lb,
                        std::vector<Detection>* out, std::string* err) {
    const auto& outputs = engine.outputs();
    const size_t per = outputs.size() / 3;
    const bool quant = engine.is_quant();
    std::vector<float> boxes;
    std::vector<float> probs;
    std::vector<int> class_ids;
    int valid_count = 0;
    for (size_t i = 0; i < outputs.size(); i += per) {
        if (!process_v8(outputs[i], outputs[i + 1], per == 3 ? &outputs[i + 2] : nullptr, quant,
                        engine.input_height(), params.conf_threshold, boxes, probs, class_ids,
                        &valid_count, err)) {
            return false;
        }
    }
    finish_detections(boxes, probs, class_ids, valid_count, params, lb, engine.input_width(),
                      engine.input_height(), false, out);
    return true;
}

}  // namespace

bool yolo_postprocess(const RknnEngine& engine, const YoloParams& params, const Letterbox& lb,
                      std::vector<Detection>* out, std::string* err) {
    if (!out) return false;
    out->clear();

    const auto& outputs = engine.outputs();
    const YoloHead head = yolo_head_kind(engine);
    if (head == YoloHead::YoloV8) return yolov8_postprocess(engine, params, lb, out, err);
    if (head == YoloHead::YoloWorld) return yoloworld_postprocess(engine, params, lb, out, err);
    if (head != YoloHead::YoloV5) {
        if (err) {
            *err = "unsupported yolo head: " + std::to_string(outputs.size()) +
                   " outputs (yolov5 needs 3, yolov8 rknn-opt needs 6/9, yolo_world needs 6/9 with "
                   "a second input)";
        }
        return false;
    }

    const int model_in_w = engine.input_width();
    const int model_in_h = engine.input_height();
    const bool quant = engine.is_quant();

    std::vector<float> boxes;   // x, y, w, h
    std::vector<float> probs;
    std::vector<int> class_ids;
    int valid_count = 0;

    for (int i = 0; i < 3; ++i) {
        const auto& t = outputs[i];
        if (t.n_dims < 4) {
            if (err) *err = "output[" + std::to_string(i) + "] has < 4 dims";
            return false;
        }
        const int grid_h = (int)t.dims[2];
        const int grid_w = (int)t.dims[3];
        const int stride = model_in_h / grid_h;
        const int expect = kPropBoxSize * 3 * grid_h * grid_w;

        const size_t elem = quant ? 1 : 4;
        if (t.data.size() < (size_t)expect * elem) {
            if (err) {
                *err = "output[" + std::to_string(i) + "] size " + std::to_string(t.data.size()) +
                       " < expected " + std::to_string((size_t)expect * elem);
            }
            return false;
        }

        if (quant) {
            valid_count += process_i8(reinterpret_cast<const int8_t*>(t.data.data()), kAnchor[i], grid_h,
                                      grid_w, model_in_h, model_in_w, stride, boxes, probs, class_ids,
                                      params.conf_threshold, t.zp, t.scale);
        } else {
            valid_count += process_fp32(reinterpret_cast<const float*>(t.data.data()), kAnchor[i], grid_h,
                                        grid_w, stride, boxes, probs, class_ids, params.conf_threshold);
        }
    }

    finish_detections(boxes, probs, class_ids, valid_count, params, lb, model_in_w, model_in_h, true,
                      out);
    return true;
}

std::vector<std::string> load_labels(const std::string& path, std::string* err) {
    std::vector<std::string> labels;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        if (err) *err = "open labels failed: " + path;
        return labels;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n > 0) labels.emplace_back(line);
    }
    fclose(f);
    return labels;
}

}  // namespace mb
