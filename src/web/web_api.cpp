#include "web/web_api.h"
#include "web/web_ui_generated.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "event/dwell_guard.h"
#include "event/event_engine.h"
#include "geo/geo_locator.h"
#include "infer/jpeg_io.h"
#include "infer/preprocess.h"
#include "locate/ground_calib.h"
#include "rules/activity_map.h"
#include "rules/rule_engine.h"
#include "stream/video_pipeline.h"
#include "video/view_guard.h"

namespace mb {

// ---------------------------------------------------------------------------
// MjpegHub
// ---------------------------------------------------------------------------

MjpegHub::~MjpegHub() { stop(); }

void MjpegHub::start(const Params& p) {
    stop();
    p_ = p;
    if (p_.width <= 0 || p_.height <= 0) {
        p_.width = 640;
        p_.height = 360;
    }
    if (p_.fps <= 0) p_.fps = 10;
    running_ = true;
    th_ = std::thread(&MjpegHub::loop, this);
}

void MjpegHub::stop() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        running_ = false;
    }
    cv_in_.notify_all();
    cv_out_.notify_all();
    if (th_.joinable()) th_.join();
}

bool MjpegHub::wants_frame(uint64_t now_ms) {
    if (!running_) return false;
    if (clients_ <= 0 && now_ms > demand_until_) return false;
    if (now_ms < next_due_ms_) return false;
    std::lock_guard<std::mutex> lk(mu_);
    return !has_input_;
}

void MjpegHub::offer(const ImageRGB& img, uint64_t now_ms) {
    const uint64_t interval = (uint64_t)(1000 / p_.fps);
    // 落后超过一个周期 (刚有人连上 / 采集卡顿) 就重新对齐, 不补发
    next_due_ms_ = (next_due_ms_ + interval < now_ms) ? now_ms + interval : next_due_ms_ + interval;
    std::unique_lock<std::mutex> lk(mu_);
    if (has_input_) return;
    input_.width = p_.width;
    input_.height = p_.height;
    Letterbox lb;
    PreprocessOptions opt;
    opt.use_rga = p_.use_rga;
    const char* warn = nullptr;
    if (!letterbox_rgb(img, &input_, &lb, opt, &warn)) return;
    has_input_ = true;
    lk.unlock();
    cv_in_.notify_one();
}

void MjpegHub::add_client() { ++clients_; }
void MjpegHub::remove_client() { --clients_; }

void MjpegHub::demand() {
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    demand_until_ = now + 2000;
}

bool MjpegHub::wait_frame(uint64_t last_seq, int timeout_ms,
                          std::shared_ptr<const std::vector<uint8_t>>* jpeg, uint64_t* seq) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_out_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [&] { return !running_ || (latest_ && seq_ > last_seq); });
    if (!latest_ || seq_ <= last_seq) return false;
    *jpeg = latest_;
    *seq = seq_;
    return true;
}

uint64_t MjpegHub::request_raw() {
    std::lock_guard<std::mutex> lk(mu_);
    raw_wanted_ = true;
    return raw_seq_;
}

void MjpegHub::offer_raw(const ImageRGB& img) {
    constexpr int kRawWidth = 1280;
    ImageRGB out;
    out.width = std::min(kRawWidth, img.width) & ~1;
    out.height = img.width > 0 ? (int)((int64_t)img.height * out.width / img.width) & ~1 : 0;
    if (out.width <= 0 || out.height <= 0) return;
    Letterbox lb;
    PreprocessOptions opt;
    opt.use_rga = p_.use_rga;
    const char* warn = nullptr;
    if (!letterbox_rgb(img, &out, &lb, opt, &warn)) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        raw_ = std::move(out);
        ++raw_seq_;
        raw_wanted_ = false;
    }
    cv_out_.notify_all();
}

bool MjpegHub::wait_raw(uint64_t last_seq, int timeout_ms, ImageRGB* out, uint64_t* seq) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_out_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [&] { return !running_ || raw_seq_ > last_seq; });
    if (raw_seq_ <= last_seq) return false;
    *out = raw_;
    *seq = raw_seq_;
    return true;
}

void MjpegHub::loop() {
    ImageRGB work;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_in_.wait(lk, [&] { return !running_ || has_input_; });
            if (!running_) return;
            std::swap(work, input_);
        }
        auto out = std::make_shared<std::vector<uint8_t>>();
        std::string err;
        const bool ok = jpeg_encode_mem(work, p_.quality, out.get(), &err);
        {
            std::lock_guard<std::mutex> lk(mu_);
            has_input_ = false;
            if (ok) {
                latest_ = out;
                ++seq_;
            }
        }
        if (ok) cv_out_.notify_all();
    }
}

// ---------------------------------------------------------------------------
// 路由
// ---------------------------------------------------------------------------

namespace {

// Browser assets are generated from src/web/ui/ by tools/embed_web_ui.py.

struct WebFontAsset {
    const char* path;
    const char* encoded;
    size_t encoded_size;
};

constexpr WebFontAsset kWebFonts[] = {
    {"/fonts/noto-sans-sc-400.woff2", kWebFontNotoSansSc400, kWebFontNotoSansSc400Size},
    {"/fonts/noto-sans-sc-600.woff2", kWebFontNotoSansSc600, kWebFontNotoSansSc600Size},
    {"/fonts/noto-sans-sc-700.woff2", kWebFontNotoSansSc700, kWebFontNotoSansSc700Size},
    {"/fonts/noto-sans-mono-400.woff2", kWebFontNotoSansMono400, kWebFontNotoSansMono400Size},
    {"/fonts/noto-sans-mono-700.woff2", kWebFontNotoSansMono700, kWebFontNotoSansMono700Size},
    {"/fonts/noto-sans-sc-102-400-normal.woff2", kWebFontNotoSansSc102400, kWebFontNotoSansSc102400Size},
    {"/fonts/noto-sans-sc-102-600-normal.woff2", kWebFontNotoSansSc102600, kWebFontNotoSansSc102600Size},
    {"/fonts/noto-sans-sc-102-700-normal.woff2", kWebFontNotoSansSc102700, kWebFontNotoSansSc102700Size},
    {"/fonts/noto-sans-sc-105-400-normal.woff2", kWebFontNotoSansSc105400, kWebFontNotoSansSc105400Size},
    {"/fonts/noto-sans-sc-105-600-normal.woff2", kWebFontNotoSansSc105600, kWebFontNotoSansSc105600Size},
    {"/fonts/noto-sans-sc-105-700-normal.woff2", kWebFontNotoSansSc105700, kWebFontNotoSansSc105700Size},
    {"/fonts/noto-sans-sc-109-400-normal.woff2", kWebFontNotoSansSc109400, kWebFontNotoSansSc109400Size},
    {"/fonts/noto-sans-sc-109-600-normal.woff2", kWebFontNotoSansSc109600, kWebFontNotoSansSc109600Size},
    {"/fonts/noto-sans-sc-109-700-normal.woff2", kWebFontNotoSansSc109700, kWebFontNotoSansSc109700Size},
};

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string decode_base64(const char* data, size_t size) {
    std::string out;
    out.reserve(size * 3 / 4);
    uint32_t bits = 0;
    int count = 0;
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '=') break;
        const int value = base64_value(c);
        if (value < 0) continue;
        bits = (bits << 6) | (uint32_t)value;
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back((char)((bits >> count) & 0xff));
        }
    }
    return out;
}

void serve_web_font(HttpConn& conn, const WebFontAsset& font) {
    const std::string body = decode_base64(font.encoded, font.encoded_size);
    conn.reply_data(200, "font/woff2", body.data(), body.size(),
                    "public, max-age=31536000, immutable");
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string read_file(const std::string& path, bool* ok) {
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    *ok = f != nullptr;
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

std::string error_json(const std::string& msg) {
    Json j = Json::object();
    j.set("ok", Json::bool_value(false));
    j.set("error", Json::string_value(msg));
    return j.dump();
}

void serve_stream(WebContext* ctx, HttpConn& conn) {
    MjpegHub* hub = ctx->hub;
    if (!hub) {
        conn.reply_json(503, error_json("stream disabled"));
        return;
    }
    const char* hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=mbframe\r\n"
        "Cache-Control: no-cache, no-store\r\nPragma: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    if (!conn.send_all(hdr, strlen(hdr))) return;
    // 长连接不能用 5s 的发送超时之外的东西兜底: 客户端卡住时 send 会超时失败, 自然退出
    hub->add_client();
    uint64_t last = 0;
    while (ctx->running == nullptr || *ctx->running) {
        std::shared_ptr<const std::vector<uint8_t>> jpeg;
        uint64_t seq = 0;
        if (!hub->wait_frame(last, 1000, &jpeg, &seq)) continue;
        last = seq;
        char part[160];
        snprintf(part, sizeof(part),
                 "--mbframe\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                 jpeg->size());
        if (!conn.send_all(part, strlen(part)) || !conn.send_all(jpeg->data(), jpeg->size()) ||
            !conn.send_all("\r\n", 2))
            break;
    }
    hub->remove_client();
}

void serve_snapshot(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    MjpegHub* hub = ctx->hub;
    if (!hub) {
        conn.reply_json(503, error_json("stream disabled"));
        return;
    }
    if (req.param("raw") == "1") {
        const uint64_t s0 = hub->request_raw();
        ImageRGB raw;
        uint64_t s1 = 0;
        std::vector<uint8_t> jpeg;
        std::string err;
        if (!hub->wait_raw(s0, 3000, &raw, &s1)) {
            conn.reply_json(503, error_json("no frame (camera off or standby)"));
            return;
        }
        if (!jpeg_encode_mem(raw, 90, &jpeg, &err)) {
            conn.reply_json(500, error_json("jpeg encode failed: " + err));
            return;
        }
        conn.reply(200, "image/jpeg", std::string(jpeg.begin(), jpeg.end()));
        return;
    }
    hub->demand();
    std::shared_ptr<const std::vector<uint8_t>> old, fresh;
    uint64_t s0 = 0, s1 = 0;
    hub->wait_frame(0, 0, &old, &s0);
    const bool got = hub->wait_frame(s0, 2000, &fresh, &s1);
    const auto& use = got ? fresh : old;
    if (!use) {
        conn.reply_json(503, error_json("no frame yet"));
        return;
    }
    conn.reply(200, "image/jpeg", std::string(use->begin(), use->end()));
}

void serve_status(WebContext* ctx, HttpConn& conn) {
    Json j = ctx->status ? ctx->status->get() : Json::object();
    if (ctx->video) j.set("h264", ctx->video->stats());
    else {
        Json v = Json::object();
        v.set("running", Json::bool_value(false));
        j.set("h264", v);
    }
    if (ctx->events) j.set("events", ctx->events->stats());
    if (ctx->hub) {
        Json s = Json::object();
        s.set("clients", Json::number_value(ctx->hub->clients()));
        s.set("width", Json::number_value(ctx->hub->params().width));
        s.set("height", Json::number_value(ctx->hub->params().height));
        s.set("fps", Json::number_value(ctx->hub->params().fps));
        j.set("mjpeg", s);
    }
    conn.reply_json(200, j.dump());
}

bool valid_power_mode(const std::string& m) {
    return m == "normal" || m == "eco" || m == "standby";
}

// 原子写配置 (tmp + rename) 并通知主循环热加载。调用方持有 ctx->config_mu。
bool write_config_locked(WebContext* ctx, const Json& cfg) {
    const std::string tmp = ctx->config_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    const std::string text = cfg.dump(2) + "\n";
    bool ok = f && fwrite(text.data(), 1, text.size(), f) == text.size();
    if (f) ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp.c_str(), ctx->config_path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    if (ctx->reload_requested) *ctx->reload_requested = true;
    return true;
}

void serve_config(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    if (req.method == "GET" || req.method == "HEAD") {
        bool ok = false;
        const std::string text = read_file(ctx->config_path, &ok);
        if (!ok) {
            conn.reply_json(500, error_json("cannot read " + ctx->config_path));
            return;
        }
        conn.reply_json(200, text);
        return;
    }
    if (req.method != "PUT" && req.method != "POST") {
        conn.reply_json(405, error_json("use GET or PUT"));
        return;
    }

    Json next;
    std::string err;
    if (!Json::parse(req.body, &next, &err) || !next.is_object()) {
        conn.reply_json(400, error_json("invalid JSON: " + err));
        return;
    }
    // 用真正的规则引擎走一遍, 区域/线/类别名写错的配置不让落盘
    RuleEngine probe;
    if (!probe.load(next, ctx->class_map, &err)) {
        conn.reply_json(400, error_json("rule config rejected: " + err));
        return;
    }
    const Json& mode = next.path("power.mode");
    if (!mode.is_null() && !valid_power_mode(mode.str(""))) {
        conn.reply_json(400, error_json("power.mode must be normal, eco or standby"));
        return;
    }
    const Json& display = next["display"];
    if (!display.is_null() && !display.is_object()) {
        conn.reply_json(400, error_json("display must be an object"));
        return;
    }
    for (const char* key : {"show_metrics", "show_stream_status", "show_heatmap",
                            "hide_region_overlays"}) {
        const Json& value = display[key];
        if (display.has(key) && value.type() != Json::Type::Bool) {
            conn.reply_json(400, error_json(std::string("display.") + key + " must be boolean"));
            return;
        }
    }
    {
        const Json& pip = display["bev_pip"];
        if (!pip.is_null() && (!pip.is_object() ||
                               (pip.has("enabled") && pip["enabled"].type() != Json::Type::Bool))) {
            conn.reply_json(400, error_json("display.bev_pip.enabled must be boolean"));
            return;
        }
    }
    if (!GeoParams::validate(next["geo"], &err)) {
        conn.reply_json(400, error_json("geo config rejected: " + err));
        return;
    }

    std::lock_guard<std::mutex> lk(ctx->config_mu);
    Json before;
    std::string berr;
    Json::parse_file(ctx->config_path, &before, &berr);
    if (!write_config_locked(ctx, next)) {
        conn.reply_json(500, error_json("write failed: " + ctx->config_path));
        return;
    }

    Json resp = Json::object();
    resp.set("ok", Json::bool_value(true));
    Json keys = Json::array();
    for (const std::string& k : restart_required_keys(before, next)) keys.push(Json::string_value(k));
    resp.set("restart_required", keys);
    conn.reply_json(200, resp.dump());
}

// GET /power: 当前功耗状态 (取自 /status)。
// POST/PUT /power {"mode":"normal|eco|standby"}: 只改 power.mode, 落盘并热加载,
// 给网页按钮和脚本用 (APP 走 PUT /config, 离线时还能退回本地写文件)。
void serve_power(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    if (req.method == "GET" || req.method == "HEAD") {
        const Json st = ctx->status ? ctx->status->get() : Json::object();
        const Json& pw = st["power"];
        conn.reply_json(200, (pw.is_object() ? pw : Json::object()).dump());
        return;
    }
    if (req.method != "PUT" && req.method != "POST") {
        conn.reply_json(405, error_json("use GET or POST"));
        return;
    }
    Json body;
    std::string err;
    if (!Json::parse(req.body, &body, &err) || !body.is_object() ||
        !valid_power_mode(body["mode"].str(""))) {
        conn.reply_json(400, error_json("expected {\"mode\":\"normal|eco|standby\"}"));
        return;
    }
    const std::string mode = body["mode"].str("");

    std::lock_guard<std::mutex> lk(ctx->config_mu);
    Json cfg;
    if (!Json::parse_file(ctx->config_path, &cfg, &err) || !cfg.is_object()) {
        conn.reply_json(500, error_json("cannot read " + ctx->config_path + ": " + err));
        return;
    }
    Json pw = cfg["power"].is_object() ? cfg["power"] : Json::object();
    pw.set("mode", Json::string_value(mode));
    cfg.set("power", pw);
    if (!write_config_locked(ctx, cfg)) {
        conn.reply_json(500, error_json("write failed: " + ctx->config_path));
        return;
    }
    Json resp = Json::object();
    resp.set("ok", Json::bool_value(true));
    resp.set("mode", Json::string_value(mode));
    conn.reply_json(200, resp.dump());
}

// GET /heatmap: 活动热图; DELETE /heatmap: 清零 (摄像头挪过位置后重新统计)。
void serve_heatmap(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    if (!ctx->activity) {
        conn.reply_json(503, error_json("activity map disabled"));
        return;
    }
    if (req.method == "GET" || req.method == "HEAD") {
        conn.reply_json(200, ctx->activity->json().dump());
        return;
    }
    if (req.method != "DELETE") {
        conn.reply_json(405, error_json("use GET or DELETE"));
        return;
    }
    ctx->activity->reset();
    std::string err;
    const bool saved = ctx->activity->maybe_save(0, true, &err);
    Json j = Json::object();
    j.set("ok", Json::bool_value(true));
    if (!saved) j.set("warning", Json::string_value(err));
    conn.reply_json(200, j.dump());
}

void serve_counts(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    if (!ctx->rules) {
        conn.reply_json(503, error_json("rule engine unavailable"));
        return;
    }
    if (req.method == "GET" || req.method == "HEAD") {
        Json j = Json::object();
        Json lines = Json::object();
        for (const auto& kv : ctx->rules->line_counts()) {
            Json c = Json::object();
            c.set("in", Json::number_value(kv.second.in));
            c.set("out", Json::number_value(kv.second.out));
            c.set("total", Json::number_value(kv.second.total()));
            c.set("net", Json::number_value(kv.second.net()));
            lines.set(kv.first, c);
        }
        j.set("lines", lines);
        // 画面内此刻的人数 (已确认、未丢失), 和 OSD 的 NOW 一致
        j.set("people_in_frame", Json::number_value(ctx->rules->people_in_frame()));
        conn.reply_json(200, j.dump());
        return;
    }
    if (req.method != "DELETE") {
        conn.reply_json(405, error_json("use GET or DELETE"));
        return;
    }
    const std::string line = req.param("line");
    if (!ctx->rules->reset_line_counts(line)) {
        conn.reply_json(404, error_json("no such line: " + line));
        return;
    }
    Json j = Json::object();
    j.set("ok", Json::bool_value(true));
    if (!line.empty()) j.set("line", Json::string_value(line));
    conn.reply_json(200, j.dump());
}

void serve_view(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    if (!ctx->view) {
        conn.reply_json(503, error_json("view guard unavailable"));
        return;
    }
    if (req.method == "GET" || req.method == "HEAD") {
        conn.reply_json(200, ctx->view->json().dump());
        return;
    }
    if (req.method != "POST" && req.method != "PUT") {
        conn.reply_json(405, error_json("use GET or POST"));
        return;
    }
    Json body;
    std::string err;
    if (!Json::parse(req.body, &body, &err) || !body.is_object()) {
        conn.reply_json(400, error_json("expected JSON object with an action"));
        return;
    }
    if (!ctx->view->command(body["action"].str(""), &err)) {
        conn.reply_json(400, error_json(err));
        return;
    }
    Json j = ctx->view->json();
    j.set("ok", Json::bool_value(true));
    conn.reply_json(200, j.dump());
}

void serve_events(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    if (!ctx->events) {
        conn.reply_json(503, error_json("event engine disabled"));
        return;
    }
    if (req.path == "/events/delete" && req.method == "POST") {
        Json body;
        std::string err;
        if (!Json::parse(req.body, &body, &err) || !body.is_object() ||
            !body["ids"].is_array()) {
            conn.reply_json(400, error_json("expected JSON object with an ids array"));
            return;
        }
        const Json& ids = body["ids"];
        if (ids.size() == 0 || ids.size() > 300) {
            conn.reply_json(400, error_json("ids must contain between 1 and 300 event IDs"));
            return;
        }
        std::vector<std::string> unique_ids;
        for (const Json& value : ids.items()) {
            const std::string id = value.str("");
            if (!EventEngine::valid_id(id)) {
                conn.reply_json(400, error_json("invalid event id in ids"));
                return;
            }
            if (std::find(unique_ids.begin(), unique_ids.end(), id) == unique_ids.end())
                unique_ids.push_back(id);
        }

        Json deleted = Json::array();
        Json busy = Json::array();
        Json not_found = Json::array();
        Json failed = Json::array();
        for (const std::string& id : unique_ids) {
            std::string detail;
            switch (ctx->events->remove(id, false, &detail)) {
                case EventEngine::DeleteResult::Ok:
                    deleted.push(Json::string_value(id));
                    break;
                case EventEngine::DeleteResult::Busy: {
                    Json item = Json::object();
                    item.set("id", Json::string_value(id));
                    item.set("error", Json::string_value(detail));
                    busy.push(item);
                    break;
                }
                case EventEngine::DeleteResult::NotFound:
                    not_found.push(Json::string_value(id));
                    break;
                case EventEngine::DeleteResult::Error: {
                    Json item = Json::object();
                    item.set("id", Json::string_value(id));
                    item.set("error", Json::string_value(detail));
                    failed.push(item);
                    break;
                }
            }
        }
        Json response = Json::object();
        response.set("ok", Json::bool_value(busy.size() == 0 && failed.size() == 0 &&
                                             not_found.size() == 0));
        response.set("deleted", deleted);
        response.set("busy", busy);
        response.set("not_found", not_found);
        response.set("failed", failed);
        conn.reply_json(200, response.dump());
        return;
    }
    if (req.path == "/events" || req.path == "/events/") {
        const int limit = atoi(req.param("limit", "50").c_str());
        conn.reply_json(200, ctx->events->list(limit, req.param("level"), req.param("before")).dump());
        return;
    }
    std::string rest = req.path.substr(strlen("/events/"));
    std::string ext;
    if (ends_with(rest, ".jpg")) ext = "jpg";
    else if (ends_with(rest, ".mp4")) ext = "mp4";
    else if (ends_with(rest, ".json")) ext = "json";
    const std::string id = ext.empty() ? rest : rest.substr(0, rest.size() - ext.size() - 1);
    if (!EventEngine::valid_id(id)) {
        conn.reply_json(400, error_json("bad event id"));
        return;
    }
    if (req.method == "DELETE") {
        // DELETE /events/<id>      删整条事件 (记录 + 快照 + 录像)
        // DELETE /events/<id>.mp4  只删录像, 保留记录和快照
        if (ext == "jpg") {
            conn.reply_json(405, error_json("snapshot cannot be deleted alone"));
            return;
        }
        std::string derr;
        switch (ctx->events->remove(id, ext == "mp4", &derr)) {
            case EventEngine::DeleteResult::Ok: {
                Json j = Json::object();
                j.set("ok", Json::bool_value(true));
                j.set("id", Json::string_value(id));
                conn.reply_json(200, j.dump());
                return;
            }
            case EventEngine::DeleteResult::NotFound: conn.reply_json(404, error_json(derr)); return;
            case EventEngine::DeleteResult::Busy: conn.reply_json(409, error_json(derr)); return;
            default: conn.reply_json(500, error_json(derr)); return;
        }
    }
    if (ext.empty() || ext == "json") {
        Json j;
        if (ctx->events->get(id, &j)) conn.reply_json(200, j.dump());
        else conn.reply_json(404, error_json("no such event"));
        return;
    }
    const std::string path = ctx->events->file_path(id, ext);
    if (path.empty()) {
        conn.reply_json(404, error_json("not found"));
        return;
    }
    conn.reply_file(req, path, ext == "jpg" ? "image/jpeg" : "video/mp4");
}

// GET /ground: 主线程发布的定位快照; DELETE /ground/stats: 清零地面区域统计;
// POST /ground/evaluate: APP 改标定点时预览质量, 不落盘。
void serve_ground(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    if (req.path == "/ground/evaluate") {
        if (req.method != "POST" && req.method != "PUT") {
            conn.reply_json(405, error_json("use POST"));
            return;
        }
        Json body;
        std::string err;
        if (!Json::parse(req.body, &body, &err) || !body["region"].is_object()) {
            conn.reply_json(400, error_json("expect {\"region\": {...}}"));
            return;
        }
        std::vector<GroundPair> calib, check;
        std::string mode;
        Json resp = Json::object();
        GroundCalib gc;
        if (!GroundCalib::parse_region(body["region"], &calib, &check, &mode, &err)) {
            resp.set("ok", Json::bool_value(false));
            resp.set("error", Json::string_value(err.empty() ? "缺少 calib_points 或 real_size_m" : err));
        } else if (!gc.fit(calib, check, &err)) {
            resp.set("ok", Json::bool_value(false));
            resp.set("error", Json::string_value(err));
            resp.set("calib", gc.to_json());
        } else {
            Json c = gc.to_json();
            c.set("mode", Json::string_value(mode));
            resp.set("ok", Json::bool_value(true));
            resp.set("calib", c);
        }
        conn.reply_json(200, resp.dump());
        return;
    }
    if (req.path == "/ground/stats") {
        if (req.method != "DELETE") {
            conn.reply_json(405, error_json("use DELETE"));
            return;
        }
        if (!ctx->ground_reset) {
            conn.reply_json(503, error_json("locator not running"));
            return;
        }
        ctx->ground_reset->store(true);
        conn.reply_json(200, R"({"ok":true})");
        return;
    }
    if (req.path != "/ground") {
        conn.reply_json(404, error_json("not found"));
        return;
    }
    if (req.method != "GET" && req.method != "HEAD") {
        conn.reply_json(405, error_json("use GET"));
        return;
    }
    Json j = ctx->ground ? ctx->ground->get() : Json::object();
    if (!j.has("enabled")) j.set("enabled", Json::bool_value(false));
    conn.reply_json(200, j.dump());
}

void serve_geo(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    if (!ctx->geo) {
        conn.reply_json(503, error_json("geo locator not running"));
        return;
    }
    if (req.path == "/geo/refresh") {
        if (req.method != "POST") {
            conn.reply_json(405, error_json("use POST"));
            return;
        }
        ctx->geo->request_refresh();
        conn.reply_json(200, R"({"ok":true})");
        return;
    }
    if (req.method != "GET" && req.method != "HEAD") {
        conn.reply_json(405, error_json("use GET"));
        return;
    }
    conn.reply_json(200, ctx->geo->json().dump());
}

}  // namespace

std::vector<std::string> restart_required_keys(const Json& before, const Json& after) {
    // 阈值 / 帧率 / 功能开关 / 规则参数都能热加载, 这里只列真正要重启的
    static const char* const kRestart[] = {
        // video.source 是 APP 自己的选项, mb-capture 不看, 不能算进来
        "video.device",    "video.width",          "video.height",       "video.fps",
        "video.pixel_format", "video.buffers",     "video.hw_jpeg",      "video.controls",
        "infer.model",          "infer.labels",       "infer.core_mask",
        "infer.use_rga",   "models.yolo",          "models.fire",        "models.pose.path",
        "models.pose.core_mask", "web",            "stream",             "events.snapshot_dir",
        "events.clip_pre_sec",
    };
    std::vector<std::string> out;
    for (const char* k : kRestart) {
        if (before.path(k).dump() != after.path(k).dump()) out.push_back(k);
    }
    return out;
}

void handle_web_request(WebContext* ctx, const HttpRequest& req, HttpConn& conn) {
    const std::string& p = req.path;
    if (req.method == "OPTIONS") {
        conn.reply(204, "text/plain", "",
                   "Access-Control-Allow-Methods: GET, PUT, POST, DELETE, OPTIONS\r\n"
                   "Access-Control-Allow-Headers: Content-Type\r\n");
        return;
    }
    if (p == "/config") {
        serve_config(ctx, req, conn);
        return;
    }
    if (p == "/power") {
        serve_power(ctx, req, conn);
        return;
    }
    if (p == "/heatmap") {
        serve_heatmap(ctx, req, conn);
        return;
    }
    if (p == "/counts") {
        serve_counts(ctx, req, conn);
        return;
    }
    if (p == "/view") {
        serve_view(ctx, req, conn);
        return;
    }
    if (p == "/ground" || p.compare(0, 8, "/ground/") == 0) {
        serve_ground(ctx, req, conn);
        return;
    }
    if (p == "/geo" || p == "/geo/refresh") {
        serve_geo(ctx, req, conn);
        return;
    }
    if (req.method == "DELETE" && p.compare(0, 8, "/events/") == 0) {
        serve_events(ctx, req, conn);
        return;
    }
    if (req.method == "POST" && p == "/events/delete") {
        serve_events(ctx, req, conn);
        return;
    }
    if (req.method != "GET" && req.method != "HEAD") {
        conn.reply_json(405, error_json("method not allowed"));
        return;
    }
    if (p == "/" || p == "/index.html") {
        conn.reply(200, "text/html; charset=utf-8", kIndexHtml);
    } else if (p == "/app.css") {
        conn.reply(200, "text/css; charset=utf-8", kWebCss);
    } else if (p == "/app.js") {
        conn.reply(200, "application/javascript; charset=utf-8", kWebJs);
    } else if (p == "/app-i18n.js") {
        conn.reply(200, "application/javascript; charset=utf-8", kWebI18n);
    } else if (p == "/app-icon.png") {
        conn.reply(200, "image/png",
                   std::string(reinterpret_cast<const char*>(kAppIconPng), kAppIconPngSize));
    } else if (p == "/fonts/OFL.txt") {
        conn.reply_data(200, "text/plain; charset=utf-8", kWebFontLicense,
                        sizeof(kWebFontLicense) - 1, "public, max-age=86400");
    } else if (p.compare(0, 7, "/fonts/") == 0) {
        bool found = false;
        for (const WebFontAsset& font : kWebFonts) {
            if (p == font.path) {
                serve_web_font(conn, font);
                found = true;
                break;
            }
        }
        if (!found) conn.reply_json(404, error_json("font not found"));
    } else if (p == "/stream") {
        serve_stream(ctx, conn);
    } else if (p == "/snapshot.jpg") {
        serve_snapshot(ctx, req, conn);
    } else if (p == "/status") {
        serve_status(ctx, conn);
    } else if (p == "/dwell") {
        conn.reply_json(200, (ctx->dwell ? ctx->dwell->json() : Json::object()).dump());
    } else if (p == "/events" || p.compare(0, 8, "/events/") == 0) {
        serve_events(ctx, req, conn);
    } else {
        conn.reply_json(404, error_json("not found"));
    }
}

}  // namespace mb
