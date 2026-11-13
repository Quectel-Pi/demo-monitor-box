#ifndef MONITOR_BOX_VIDEO_V4L2_SOURCE_H_
#define MONITOR_BOX_VIDEO_V4L2_SOURCE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "infer/types.h"

namespace mb {

class MppJpegDecoder;

// 一帧原始采集数据。MJPG 模式下 data 是压缩流, 交给 jpeg_decode_mem 解码;
// YUYV 模式下是原始 YUYV422, 走 yuyv_to_rgb。
struct VideoFrame {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    uint32_t pixel_format = 0;
    uint32_t sequence = 0;
    uint64_t timestamp_ms = 0;  // V4L2 buffer 时间戳, 单调递增
};

// UVC (V4L2) 采集封装: mmap + 多缓冲 + poll。
//
// 实测约束 (见 docs/architecture.zh-CN.md 1):
//   - 摄像头是 USB 3.0 的 `HZ USB Camera`, 采集节点在 /dev/video73 附近,
//     编号不固定, 用 tools/probe_board.sh 查, 或用 --list 自动挑。
//   - 1080p 只有 MJPG 能跑 ~30fps; YUYV 1080p 会掉到个位数, 640x480 时 15fps。
//   - 系统自带的 cam-server/cam2file 会占住节点, 启动前要先停掉。
class V4l2Source {
public:
    V4l2Source();
    ~V4l2Source();

    V4l2Source(const V4l2Source&) = delete;
    V4l2Source& operator=(const V4l2Source&) = delete;

    // 枚举 /dev/video* 里支持视频采集的节点, 返回可读的候选列表 (按编号排序)。
    // 每项形如 "/dev/video73 (HZ USB Camera) 1920x1080 MJPG"。
    static std::vector<std::string> list_capture_devices(std::string* err);

    // 找第一个 uvcvideo 采集节点 (优先支持 MJPG 的)。摄像头拔插/换口后编号会变,
    // 配置里的节点不在了就靠它重新找; 板上的 rkisp/rkvpss/rkcif 节点也是采集节点, 必须排除。
    static std::string find_uvc_device();

    // 打开并配置。pixel_format 取 "MJPG" / "YUYV", 空串表示让驱动自己挑
    // (优先 MJPG)。width/height/fps 为 0 时用驱动给出的最大值。
    bool open(const std::string& device, int width, int height, int fps,
              const std::string& pixel_format, int buffers, std::string* err);
    void close();

    // Live MJPG frames use RK MPP + RGA when available. The classic libjpeg
    // path remains the fallback for unsupported formats or devices.
    void set_hardware_jpeg(bool enabled) { hardware_jpeg_enabled_ = enabled; }
    const char* jpeg_backend() const { return jpeg_decoder_ ? "mpp" : "cpu"; }
    uint64_t hardware_jpeg_failures() const { return hardware_jpeg_failures_; }

    bool started() const { return streaming_; }
    const std::string& device() const { return device_; }
    int width() const { return width_; }
    int height() const { return height_; }
    uint32_t pixel_format() const { return pixel_format_; }
    const char* pixel_format_name() const;

    // 按 v4l2-ctl 的控件名设置 (如 exposure_dynamic_framerate=0), 按给定顺序。
    // 设不上的只记进 warnings, 不算失败: 不同摄像头的控件集合不一样。
    void apply_controls(const std::vector<std::pair<std::string, int>>& controls,
                        std::vector<std::string>* warnings);

    // 取一帧, 内部拷贝到 VideoFrame 后立刻把 buffer 还给驱动。
    // 超时返回 false 且 err 为空 (这是正常情况: 没有新帧); 驱动标记的坏帧同样
    // 静默跳过 (计入 error_frames)。err 非空表示设备真的出问题了 (如被拔掉)。
    //
    // 处理跟不上时, 驱动队列里会积压旧帧, 逐个取会让画面越来越滞后。这里每次都
    // 把已就绪的帧取空、只留最新一帧 (被跳过的计入 stale_skipped), 保证延迟有界。
    bool read(VideoFrame* out, int timeout_ms, std::string* err);

    // Decode/convert a captured frame to packed RGB888.
    bool decode_frame(const VideoFrame& frame, ImageRGB* out, std::string* err);

    uint64_t stale_skipped() const { return stale_skipped_; }
    uint64_t error_frames() const { return error_frames_; }

private:
    bool set_format(int width, int height, const std::string& pixel_format, std::string* err);
    bool set_fps(int fps, std::string* err);
    bool start_streaming(std::string* err);

    std::string device_;
    int fd_ = -1;
    bool streaming_ = false;
    int width_ = 0;
    int height_ = 0;
    uint32_t pixel_format_ = 0;
    uint64_t stale_skipped_ = 0;
    uint64_t error_frames_ = 0;
    uint64_t hardware_jpeg_failures_ = 0;
    bool hardware_jpeg_enabled_ = false;
    std::unique_ptr<MppJpegDecoder> jpeg_decoder_;

    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };
    std::vector<Buffer> buffers_;
};

// YUYV422 -> RGB888 (CPU, 有限范围的 ITU-R BT.601 反变换)。
// 不支持 RGA: RGA 的 YUYV 路径在 USB 采集的 stride 上不一定对齐, 出问题难查。
void yuyv_to_rgb(const uint8_t* src, int width, int height, ImageRGB* out);

// 把一帧解码/转换到 RGB。MJPG 走 libjpeg, YUYV 走上面那个函数。
bool frame_to_rgb(const VideoFrame& frame, ImageRGB* out, std::string* err);

}  // namespace mb

#endif  // MONITOR_BOX_VIDEO_V4L2_SOURCE_H_
