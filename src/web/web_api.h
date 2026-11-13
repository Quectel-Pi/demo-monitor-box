#ifndef MONITOR_BOX_WEB_WEB_API_H_
#define MONITOR_BOX_WEB_WEB_API_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/json.h"
#include "infer/types.h"
#include "web/http_server.h"

namespace mb {

class EventEngine;
class DwellGuard;
class VideoPipeline;
class ActivityMap;
class ViewGuard;
class RuleEngine;
class StatusBoard;
class GeoLocator;

// MJPEG 预览 (/stream): 默认 1280x720@15fps, 浏览器 <img src="/stream"> 直接看。
//
// 主线程按节拍 offer() 一帧 (只做 RGA 缩放, ~1ms), JPEG 编码在自己的线程里;
// 没有客户端时完全不干活。所有客户端共享同一份 JPEG, 不随人数线性增加 CPU。
class MjpegHub {
public:
    struct Params {
        int width = 1280;
        int height = 720;
        int fps = 15;
        int quality = 85;
        bool use_rga = true;
    };

    MjpegHub() = default;
    ~MjpegHub();

    void start(const Params& p);
    void stop();

    // 主线程: 这一帧要不要送过来 (有人在看 + 到点了 + 编码线程空闲)。
    bool wants_frame(uint64_t now_ms);
    void offer(const ImageRGB& img, uint64_t now_ms);

    // Web 线程
    void add_client();
    void remove_client();
    void demand();  // /snapshot.jpg 这种一次性请求, 让主线程在接下来 2s 内照常送帧
    // 等一帧比 last_seq 新的 JPEG; 超时返回 false。
    bool wait_frame(uint64_t last_seq, int timeout_ms,
                    std::shared_ptr<const std::vector<uint8_t>>* jpeg, uint64_t* seq);

    // 不带 OSD 的原始画面 (APP 画区域用)。Web 线程 request_raw() 后等 wait_raw();
    // 主线程在画 OSD 之前看 raw_wanted(), 是就 offer_raw() 一份缩小的拷贝, 编码在 Web 线程做。
    uint64_t request_raw();  // 返回当前序号, 之后 wait_raw(该序号) 等新的一帧
    bool raw_wanted() const { return raw_wanted_; }
    void offer_raw(const ImageRGB& img);
    bool wait_raw(uint64_t last_seq, int timeout_ms, ImageRGB* out, uint64_t* seq);

    int clients() const { return clients_; }
    const Params& params() const { return p_; }

private:
    void loop();

    Params p_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<int> clients_{0};
    std::atomic<uint64_t> demand_until_{0};
    uint64_t next_due_ms_ = 0;  // 按截止时间排程: 30fps 采集也能稳定出 15fps, 而不是隔帧取成 15

    std::mutex mu_;
    std::condition_variable cv_in_;
    std::condition_variable cv_out_;
    bool has_input_ = false;
    ImageRGB input_;
    std::shared_ptr<const std::vector<uint8_t>> latest_;
    uint64_t seq_ = 0;

    std::atomic<bool> raw_wanted_{false};
    ImageRGB raw_;
    uint64_t raw_seq_ = 0;
};

// 主线程每秒发布一次的运行状态, /status 直接拼上去。
class StatusBoard {
public:
    void publish(Json j) {
        std::lock_guard<std::mutex> lk(mu_);
        j_ = std::move(j);
    }
    Json get() const {
        std::lock_guard<std::mutex> lk(mu_);
        return j_;
    }

private:
    mutable std::mutex mu_;
    Json j_ = Json::object();
};

struct WebContext {
    std::string config_path;
    std::map<std::string, int> class_map;  // 校验新配置用
    EventEngine* events = nullptr;
    DwellGuard* dwell = nullptr;
    VideoPipeline* video = nullptr;
    MjpegHub* hub = nullptr;
    StatusBoard* status = nullptr;
    ActivityMap* activity = nullptr;
    ViewGuard* view = nullptr;
    RuleEngine* rules = nullptr;  // 越线计数 GET/DELETE /counts
    StatusBoard* ground = nullptr;              // GET /ground 的快照 (主线程发布)
    std::atomic<bool>* ground_reset = nullptr;  // DELETE /ground/stats: 主线程下一帧清零
    GeoLocator* geo = nullptr;                  // GET /geo, POST /geo/refresh
    std::atomic<bool>* reload_requested = nullptr;
    std::atomic<bool>* running = nullptr;
    std::mutex config_mu;
};

// 路由:
//   GET  /                     内置预览页
//   GET  /stream               MJPEG (multipart/x-mixed-replace)
//   GET  /snapshot.jpg         当前一帧 (小图, 带 OSD)
//   GET  /snapshot.jpg?raw=1   不带 OSD 的原始画面, 宽 1280 (APP 画区域用)
//   GET  /heatmap              活动热图: 人/车脚点经过各格子的次数 (APP 推荐区域用)
//   DELETE /heatmap            清空热图, 重新统计
//   GET  /counts               越线计数: 每条线 {in,out,total,net}
//   DELETE /counts[?line=<id>] 清零计数, 不带 line = 全部清零
//   GET  /view                 画面对齐: 摄像头是否被碰歪、区域当前的跟随偏移
//   POST /view                 JSON {"action":"ack|undo|reference|accept"}: 隐藏提示 /
//                              撤销跟随并以当前画面为准 / 同 undo / 画面正常, 记为补充参考
//   GET  /status               运行状态 JSON
//   GET  /dwell                人员滞留: 进行中的会话 + 最近结束的会话 + 统计
//   GET  /config               当前配置文件
//   PUT  /config  (或 POST)    整份替换, 校验通过后原子写盘并热加载
//   GET  /events?limit=&level=&before=   事件列表 (新的在前)
//   GET  /events/<id>          事件 JSON
//   GET  /events/<id>.jpg|.mp4 快照 / 录像 (支持 Range)
//   DELETE /events/<id>        删除事件 (记录 + 快照 + 录像); 录像生成中返回 409
//   DELETE /events/<id>.mp4    只删录像, 保留记录和快照 (clip_state 变为 "deleted")
//   POST /events/delete        JSON {"ids":["..."]}, 批量删最多 300 条; 录制中等情况逐条返回
//   GET  /power                功耗状态 (mode / camera_on / 帧率上限)
//   POST /power  (或 PUT)      JSON {"mode":"normal|eco|standby"}; standby = 关摄像头省电
//   GET  /ground               二维定位: 地面目标/轨迹/地面区域统计/标定质量 (鸟瞰图数据)
//   DELETE /ground/stats       清零地面区域的停留统计
//   POST /ground/evaluate      JSON {"region":{...}}: 评估一份地面标定 (不落盘), 返回质量
//   GET  /geo                  设备经纬度 (WiFi / IP / 手动)
//   POST /geo/refresh          立即重新定位
void handle_web_request(WebContext* ctx, const HttpRequest& req, HttpConn& conn);

// 新旧配置比较: 这些段改了只能重启生效 (摄像头/模型/端口/编码)。
std::vector<std::string> restart_required_keys(const Json& before, const Json& after);

}  // namespace mb

#endif  // MONITOR_BOX_WEB_WEB_API_H_
