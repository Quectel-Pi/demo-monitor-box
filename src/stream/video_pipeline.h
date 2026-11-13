#ifndef MONITOR_BOX_STREAM_VIDEO_PIPELINE_H_
#define MONITOR_BOX_STREAM_VIDEO_PIPELINE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/json.h"
#include "infer/types.h"
#include "stream/rtp_sender.h"
#include "video/h264_encoder.h"

namespace mb {

struct VideoPipelineConfig {
    H264Config enc;
    bool udp_enabled = true;
    std::string udp_host = "127.0.0.1";
    int udp_port = 15004;
    int mtu = 1400;
    double preroll_sec = 5.0;  // 环形缓冲至少保留多久, 给告警录像的 "前 N 秒"

    static VideoPipelineConfig from_json(const Json& cfg, int cam_w, int cam_h, int cam_fps);
};

// 编码线程: H.264 常开 -> (1) RTP 推到 UDP 15004 给 APP
//                       -> (2) 环形缓冲, 告警时截 "前 pre + 后 post 秒" 封 MP4
//
// DESIGN 2.4: 编码器常开, 告警时从缓冲里往前取, 这样不会丢前 5 秒, 也不用临时起编码器。
class VideoPipeline {
public:
    using ClipDone = std::function<void(bool ok, const std::string& path, const std::string& err)>;

    VideoPipeline() = default;
    ~VideoPipeline();

    bool start(const VideoPipelineConfig& cfg, std::string* err);
    void stop();
    bool running() const { return running_; }

    // 主线程调用: RGA 转 NV12 进空闲槽位并排队; 编码跟不上时丢帧返回 false。
    bool submit(const ImageRGB& img, uint64_t ts_ms);

    // 申请一段 [trigger - pre, trigger + post] 的录像, 写完 (或失败) 后在后台线程回调。
    void request_clip(const std::string& path, uint64_t trigger_ts_ms, double pre_sec,
                      double post_sec, ClipDone done);

    // 推迟一段尚未收尾的录像的结束时刻 (只会往后推)。找不到 (已收尾) 返回 false。
    bool extend_clip(const std::string& path, uint64_t end_ts_ms);

    // 立即按现有内容收尾所有未录满的片段。摄像头关闭 (待机) 后不会再有新帧,
    // 不收尾的话这些录像要等到进程退出才落盘。
    void flush_clips();

    Json stats() const;

private:
    struct Clip {
        std::string path;
        uint64_t end_ts = 0;
        std::vector<std::shared_ptr<const EncodedPacket>> packets;
        ClipDone done;
    };
    struct Job {
        int slot = -1;
        uint64_t ts_ms = 0;
    };

    void loop();
    void finish_clip(std::unique_ptr<Clip> clip);

    VideoPipelineConfig cfg_;
    H264Encoder enc_;
    RtpSender rtp_;
    std::thread th_;
    std::atomic<bool> running_{false};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::deque<std::shared_ptr<const EncodedPacket>> ring_;
    std::vector<std::unique_ptr<Clip>> clips_;
    uint64_t last_submit_ts_ = 0;

    std::atomic<int> muxers_{0};
    std::atomic<uint64_t> encoded_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> errors_{0};
    std::atomic<uint64_t> clips_ok_{0};
    std::atomic<uint64_t> clips_failed_{0};
    std::string last_error_;
};

}  // namespace mb

#endif  // MONITOR_BOX_STREAM_VIDEO_PIPELINE_H_
