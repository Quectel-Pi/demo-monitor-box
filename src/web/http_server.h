#ifndef MONITOR_BOX_WEB_HTTP_SERVER_H_
#define MONITOR_BOX_WEB_HTTP_SERVER_H_

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace mb {

struct HttpRequest {
    std::string method;
    std::string path;   // 已 URL 解码, 不含 query
    std::string query;  // 原始 query 串
    std::map<std::string, std::string> headers;  // 键小写
    std::string body;
    std::string peer;

    std::string param(const std::string& name, const std::string& def = "") const;
    std::string header(const std::string& name) const;
};

class HttpConn {
public:
    explicit HttpConn(int fd) : fd_(fd) {}

    bool send_all(const void* data, size_t n);
    bool send_all(const std::string& s) { return send_all(s.data(), s.size()); }

    // 普通响应 (Connection: close)。
    void reply(int code, const std::string& content_type, const std::string& body,
               const std::string& extra_headers = "");
    // 二进制资源：避免为内嵌字体等大对象再构造一份 std::string。
    void reply_data(int code, const std::string& content_type, const void* body, size_t size,
                    const std::string& cache_control);
    void reply_json(int code, const std::string& json) {
        reply(code, "application/json; charset=utf-8", json);
    }
    // 静态文件, 支持单段 Range (浏览器 <video> 拖动进度条要用)。
    void reply_file(const HttpRequest& req, const std::string& path,
                    const std::string& content_type);

    bool sent_headers() const { return sent_; }
    int fd() const { return fd_; }

private:
    int fd_;
    bool sent_ = false;
};

// 极简 HTTP/1.1 服务器: 每连接一个线程、一请求一连接 (Connection: close)。
//
// 为什么不用 mongoose: 它是 GPLv2 / 商业双授权 (不是 BSD), 闭源产品要么开源要么买授权;
// 而这里只需要几个 GET/PUT 和一路 MJPEG 长连接, 自己写几百行更可控。
// 并发上限 max_clients, 超过直接 503, 避免网页开太多标签把板子拖垮。
class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpConn&)>;

    HttpServer() = default;
    ~HttpServer();

    bool start(const std::string& listen_addr, int port, int max_clients, Handler h,
               std::string* err);
    void stop();
    bool running() const { return running_; }
    int active_clients() const { return active_; }

private:
    void accept_loop();
    void serve(int fd, std::string peer);

    int listen_fd_ = -1;
    int max_clients_ = 8;
    Handler handler_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<int> active_{0};
};

const char* http_status_text(int code);
std::string url_decode(const std::string& s);

}  // namespace mb

#endif  // MONITOR_BOX_WEB_HTTP_SERVER_H_
