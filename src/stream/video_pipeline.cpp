#include "stream/video_pipeline.h"

#include <chrono>
#include <cstdio>

#include "stream/mp4_writer.h"

namespace mb {

VideoPipelineConfig VideoPipelineConfig::from_json(const Json& cfg, int cam_w, int cam_h,
                                                   int cam_fps) {
    VideoPipelineConfig c;
    const Json& h = cfg.path("stream.h264");
    c.enc.width = h["width"].integer(0);
    c.enc.height = h["height"].integer(0);
    if (c.enc.width <= 0 || c.enc.height <= 0) {
        c.enc.width = cam_w;
        c.enc.height = cam_h;
    }
    c.enc.width &= ~1;
    c.enc.height &= ~1;
    c.enc.fps = cam_fps > 0 ? cam_fps : 30;
    c.enc.bitrate_kbps = h["bitrate_kbps"].integer(4000);
    c.enc.gop = h["gop"].integer(c.enc.fps);
    c.udp_enabled = h["udp_enabled"].boolean(true);
    c.udp_host = h["udp_host"].str("127.0.0.1");
    c.udp_port = h["udp_port"].integer(15004);
    c.mtu = h["mtu"].integer(1400);
    c.preroll_sec = cfg.path("events.clip_pre_sec").number(5.0);
    return c;
}

VideoPipeline::~VideoPipeline() { stop(); }

bool VideoPipeline::start(const VideoPipelineConfig& cfg, std::string* err) {
    stop();
    cfg_ = cfg;
    if (!enc_.open(cfg_.enc, err)) return false;
    if (cfg_.udp_enabled) {
        std::string e;
        if (!rtp_.open(cfg_.udp_host, cfg_.udp_port, cfg_.mtu, &e)) {
            fprintf(stderr, "[video] UDP 推流不可用: %s\n", e.c_str());
        }
    }
    running_ = true;
    th_ = std::thread(&VideoPipeline::loop, this);
    return true;
}

void VideoPipeline::stop() {
    if (running_) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    // 收尾: 没录满的片段按现有内容落盘, 免得退出时丢告警录像
    std::vector<std::unique_ptr<Clip>> pending;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending.swap(clips_);
        for (const Job& j : queue_) enc_.release_slot(j.slot);
        queue_.clear();
        ring_.clear();
    }
    for (auto& c : pending) finish_clip(std::move(c));
    // 等后台 mux 线程写完, 否则进程退出时文件是半截的
    for (int i = 0; i < 300 && muxers_ > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    rtp_.close();
    enc_.close();
}

bool VideoPipeline::submit(const ImageRGB& img, uint64_t ts_ms) {
    if (!running_) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 时间戳必须单调: RTP/MP4 都按它算时长
        if (ts_ms <= last_submit_ts_) ts_ms = last_submit_ts_ + 1;
        last_submit_ts_ = ts_ms;
    }
    const int slot = enc_.acquire_slot();
    if (slot < 0) {
        ++dropped_;
        return false;
    }
    std::string err;
    if (!enc_.fill_slot(slot, img, &err)) {
        enc_.release_slot(slot);
        ++errors_;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(Job{slot, ts_ms});
    }
    cv_.notify_one();
    return true;
}

void VideoPipeline::request_clip(const std::string& path, uint64_t trigger_ts_ms, double pre_sec,
                                 double post_sec, ClipDone done) {
    std::unique_ptr<Clip> c(new Clip());
    c->path = path;
    c->end_ts = trigger_ts_ms + (uint64_t)(post_sec * 1000.0);
    c->done = std::move(done);
    const uint64_t pre_ms = (uint64_t)(pre_sec * 1000.0);
    const uint64_t want_from = trigger_ts_ms > pre_ms ? trigger_ts_ms - pre_ms : 0;

    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) {
        if (c->done) c->done(false, path, "编码器未运行");
        return;
    }
    // 从 want_from 之前最近的 IDR 开始; 缓冲里没有就从最早的 IDR 开始
    size_t start = ring_.size();
    for (size_t i = 0; i < ring_.size(); ++i) {
        if (!ring_[i]->keyframe) continue;
        if (ring_[i]->ts_ms <= want_from || start == ring_.size()) start = i;
        if (ring_[i]->ts_ms > want_from) break;
    }
    for (size_t i = start; i < ring_.size(); ++i) c->packets.push_back(ring_[i]);
    clips_.push_back(std::move(c));
}

bool VideoPipeline::extend_clip(const std::string& path, uint64_t end_ts_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& c : clips_) {
        if (c->path != path) continue;
        if (end_ts_ms > c->end_ts) c->end_ts = end_ts_ms;
        return true;
    }
    return false;
}

void VideoPipeline::flush_clips() {
    std::vector<std::unique_ptr<Clip>> pending;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending.swap(clips_);
    }
    for (auto& c : pending) finish_clip(std::move(c));
}

void VideoPipeline::finish_clip(std::unique_ptr<Clip> clip) {
    // MP4 封装 + 写盘约几十毫秒, 放后台线程, 不拖慢编码
    ++muxers_;
    const int w = cfg_.enc.width, h = cfg_.enc.height;
    std::shared_ptr<Clip> c(clip.release());
    std::thread([this, c, w, h]() {
        std::vector<const EncodedPacket*> pk;
        pk.reserve(c->packets.size());
        for (const auto& p : c->packets) pk.push_back(p.get());
        std::string err;
        const bool ok = write_mp4(c->path, pk, w, h, &err);
        if (ok)
            ++clips_ok_;
        else
            ++clips_failed_;
        if (c->done) c->done(ok, c->path, err);
        --muxers_;
    }).detach();
}

void VideoPipeline::loop() {
    // 缓冲窗口 = 预录时长 + 一个 GOP 余量 (保证往前一定能找到 IDR)
    const uint64_t gop_ms = (uint64_t)(1000.0 * cfg_.enc.gop / (cfg_.enc.fps > 0 ? cfg_.enc.fps : 30));
    const uint64_t keep_ms = (uint64_t)(cfg_.preroll_sec * 1000.0) + gop_ms + 1000;

    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
            if (!running_) return;
            job = queue_.front();
            queue_.pop_front();
        }

        auto pkt = std::make_shared<EncodedPacket>();
        std::string err;
        const bool ok = enc_.encode_slot(job.slot, job.ts_ms, pkt.get(), &err);
        enc_.release_slot(job.slot);
        if (!ok) {
            ++errors_;
            std::lock_guard<std::mutex> lk(mu_);
            last_error_ = err;
            continue;
        }
        ++encoded_;
        bytes_ += pkt->data.size();

        if (rtp_.is_open()) rtp_.send_access_unit(pkt->data.data(), pkt->data.size(), pkt->ts_ms);

        std::vector<std::unique_ptr<Clip>> done;
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::shared_ptr<const EncodedPacket> cp = pkt;
            ring_.push_back(cp);
            while (!ring_.empty() && ring_.front()->ts_ms + keep_ms < pkt->ts_ms) ring_.pop_front();
            for (size_t i = 0; i < clips_.size();) {
                clips_[i]->packets.push_back(cp);
                if (pkt->ts_ms >= clips_[i]->end_ts) {
                    done.push_back(std::move(clips_[i]));
                    clips_.erase(clips_.begin() + (long)i);
                } else {
                    ++i;
                }
            }
        }
        for (auto& c : done) finish_clip(std::move(c));
    }
}

Json VideoPipeline::stats() const {
    Json j = Json::object();
    j.set("running", Json::bool_value(running_));
    j.set("width", Json::number_value(cfg_.enc.width));
    j.set("height", Json::number_value(cfg_.enc.height));
    j.set("bitrate_kbps", Json::number_value(cfg_.enc.bitrate_kbps));
    j.set("gop", Json::number_value(cfg_.enc.gop));
    j.set("rga", Json::bool_value(enc_.using_rga()));
    j.set("encoded", Json::number_value((double)encoded_));
    j.set("dropped", Json::number_value((double)dropped_));
    j.set("errors", Json::number_value((double)errors_));
    j.set("bytes", Json::number_value((double)bytes_));
    j.set("clips_ok", Json::number_value((double)clips_ok_));
    j.set("clips_failed", Json::number_value((double)clips_failed_));
    Json udp = Json::object();
    udp.set("enabled", Json::bool_value(rtp_.is_open()));
    udp.set("host", Json::string_value(cfg_.udp_host));
    udp.set("port", Json::number_value(cfg_.udp_port));
    udp.set("packets", Json::number_value((double)rtp_.packets_sent()));
    udp.set("errors", Json::number_value((double)rtp_.send_errors()));
    j.set("udp", udp);
    std::lock_guard<std::mutex> lk(mu_);
    j.set("pending_clips", Json::number_value((double)clips_.size()));
    if (!last_error_.empty()) j.set("last_error", Json::string_value(last_error_));
    return j;
}

}  // namespace mb
