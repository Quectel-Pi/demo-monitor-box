# 火焰烟雾模型 (fire_yolov8n.rknn)

把公开的 YOLOv8n 火焰烟雾权重转换成 RK3576 上能跑的 INT8 RKNN 模型，给 `mb-capture` 的 `features.fire` 使用。

## 一键生成

```bash
tools/fire_model/build_fire_model.sh            # -> models/fire_yolov8n.rknn + models/fire_labels.txt
EVAL=1 tools/fire_model/build_fire_model.sh     # 另外对比 ONNX fp32 vs RKNN INT8 (模拟器) 精度
```

需要 SDK 宿主机 x86_64 + Python 3.10 + 网络（HuggingFace / GitHub / PyTorch 源）。脚本会建两个 venv（版本冲突，必须分开）：

| venv | 内容 | 用途 |
| --- | --- | --- |
| `~/.venvs/yolov8-export` | torch 2.4.1 (CPU) + [airockchip/ultralytics_yolov8](https://github.com/airockchip/ultralytics_yolov8) | 导出 RKNN 优化 ONNX（去掉 DFL/解码，9 个输出） |
| `~/.venvs/rknn232` | rknn-toolkit2 **2.3.2**（原厂 SDK `external/rknn-toolkit2` 里的 wheel） | 量化转换、模拟器评估 |

torch 不能用 2.9+：新版 `torch.onnx.export` 默认走 dynamo 导出器，airockchip 分支跑不通。toolkit 必须是 2.3.2，与板上 `librknnrt.so` 一致。

## 分步

| 脚本 | 作用 |
| --- | --- |
| `export_onnx.py` | `.pt` → ONNX（`format=rknn`），顺带写出类别名文件 |
| `extract_dfire.py` | 从 HF `badsaarow/d-fire` 的 parquet 分片抽图 + YOLO 标注，切出校准集（200 张，70% 有目标）和评估集 |
| `convert_rknn.py` | ONNX → RKNN，`mean=0 std=255`，`target_platform=rk3576`，INT8 |
| `eval_model.py` | 同一批图上对比 ONNX 与 INT8：每类 AP50、图片召回、负样本误报（后处理与 `src/infer/yolo_post.cpp` 一致） |

## 实测（2026-11, D-Fire test 分片 0 的评估子集 700 张，其中 396 张无火无烟）

| | AP50 smoke | AP50 fire | conf≥0.45 火焰图片召回 / 负样本误报 | conf≥0.45 烟雾图片召回 / 负样本误报 |
| --- | --- | --- | --- | --- |
| ONNX fp32 | 0.850 | 0.914 | 95.2% / 0% | 75.6% / 0.3% |
| RKNN INT8（模拟器） | 0.856 | 0.912 | ≈95% / 0% | ≈75% / 0.3% |

板上（`mb-infer --repeat 20`）：NPU **14.1 ms/帧**；C++ INT8 解码结果与 Python ONNX 对比，分数差约 0.02、框差在几个像素以内。

D-Fire 多为户外/山火/网络图片，与室内监控视角差别大，**现场阈值要用实际画面再调**（`features.fire.*`，见项目 README）。

## 来源与许可（商用前务必核实）

- 权重：[rabahdev/fire-smoke-yolov8n](https://huggingface.co/rabahdev/fire-smoke-yolov8n)，D-Fire 微调，**AGPL-3.0**（继承 Ultralytics）。类别 `0=smoke 1=fire`。
- 导出工具：airockchip/ultralytics_yolov8，AGPL-3.0。
- 校准/评估数据：D-Fire（HF 镜像 `badsaarow/d-fire`），许可以原始数据集为准。
- 时序告警思路参考 [z11hzy/yolov8-fire-smoke-detection](https://github.com/z11hzy/yolov8-fire-smoke-detection)（MIT），本项目为独立 C++ 实现，未复制代码。

AGPL 权重随产品分发可能带来开源义务。量产建议用自有数据重新训练，或购买 Ultralytics 企业许可；换权重后重跑本脚本即可（`WEIGHTS=/path/to/best.pt`），只要是 YOLOv8 检测模型、类别名里有 `fire` / `smoke`，C++ 侧不用改。
