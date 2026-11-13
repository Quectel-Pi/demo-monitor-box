#include "web/http_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mb {

namespace {

constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kMaxBody = 1024 * 1024;

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out.push_back((char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2])));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

const char* http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

std::string HttpRequest::param(const std::string& name, const std::string& def) const {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(pos, amp - pos);
        const size_t eq = kv.find('=');
        const std::string k = url_decode(kv.substr(0, eq));
        if (k == name) return eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
        pos = amp + 1;
    }
    return def;
}

std::string HttpRequest::header(const std::string& name) const {
    auto it = headers.find(lower(name));
    return it == headers.end() ? "" : it->second;
}

bool HttpConn::send_all(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    while (n > 0) {
        const ssize_t r = ::send(fd_, p, n, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}


void HttpConn::reply_data(int code, const std::string& content_type, const void* body,
                          size_t size, const std::string& cache_control) {
    char hdr[640];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Cache-Control: %s\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
             code, http_status_text(code), content_type.c_str(), size, cache_control.c_str());
    sent_ = true;
    if (!send_all(hdr, strlen(hdr))) return;
    send_all(body, size);
}

void HttpConn::reply(int code, const std::string& content_type, const std::string& body,
                     const std::string& extra_headers) {
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n",
             code, http_status_text(code), content_type.c_str(), body.size());
    sent_ = true;
    if (!send_all(std::string(hdr) + extra_headers + "\r\n")) return;
    send_all(body);
}

void HttpConn::reply_file(const HttpRequest& req, const std::string& path,
                          const std::string& content_type) {
    const int f = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (f < 0 || fstat(f, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (f >= 0) ::close(f);
        reply_json(404, "{\"error\":\"not found\"}");
        return;
    }
    const uint64_t size = (uint64_t)st.st_size;
    uint64_t begin = 0, end = size ? size - 1 : 0;
    int code = 200;

    const std::string range = req.header("range");
    if (!range.empty() && range.compare(0, 6, "bytes=") == 0 && size > 0) {
        const std::string spec = range.substr(6);
        const size_t dash = spec.find('-');
        if (dash != std::string::npos && spec.find(',') == std::string::npos) {
            const std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
            if (a.empty()) {  // bytes=-N: 最后 N 字节
                const uint64_t n = strtoull(b.c_str(), nullptr, 10);
                begin = n >= size ? 0 : size - n;
            } else {
                begin = strtoull(a.c_str(), nullptr, 10);
                if (!b.empty()) end = strtoull(b.c_str(), nullptr, 10);
            }
            if (end >= size) end = size - 1;
            if (begin > end) {
                ::close(f);
                char extra[64];
                snprintf(extra, sizeof(extra), "Content-Range: bytes */%llu\r\n",
                         (unsigned long long)size);
                reply(416, "text/plain", "", extra);
                return;
            }
            code = 206;
        }
    }

    const uint64_t len = size ? end - begin + 1 : 0;
    char hdr[640];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                     "Accept-Ranges: bytes\r\nAccess-Control-Allow-Origin: *\r\n"
                     "Cache-Control: max-age=3600\r\nConnection: close\r\n",
                     code, http_status_text(code), content_type.c_str(), (unsigned long long)len);
    if (code == 206)
        n += snprintf(hdr + n, sizeof(hdr) - (size_t)n, "Content-Range: bytes %llu-%llu/%llu\r\n",
                      (unsigned long long)begin, (unsigned long long)end, (unsigned long long)size);
    snprintf(hdr + n, sizeof(hdr) - (size_t)n, "\r\n");
    sent_ = true;
    if (!send_all(hdr, strlen(hdr)) || req.method == "HEAD") {
        ::close(f);
        return;
    }
    if (lseek(f, (off_t)begin, SEEK_SET) < 0) {
        ::close(f);
        return;
    }
    char buf[64 * 1024];
    uint64_t remain = len;
    while (remain > 0) {
        const ssize_t r = ::read(f, buf, remain < sizeof(buf) ? remain : sizeof(buf));
        if (r <= 0 || !send_all(buf, (size_t)r)) break;
        remain -= (uint64_t)r;
    }
    ::close(f);
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(const std::string& listen_addr, int port, int max_clients, Handler h,
                       std::string* err) {
    stop();
    handler_ = std::move(h);
    max_clients_ = max_clients > 0 ? max_clients : 8;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, listen_addr.empty() ? "0.0.0.0" : listen_addr.c_str(),
                  &addr.sin_addr) != 1) {
        if (err) *err = "无效的监听地址: " + listen_addr;
        return false;
    }
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        if (err) *err = std::string("socket: ") + strerror(errno);
        return false;
    }
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listen_fd_, 16) != 0) {
        if (err) *err = "监听 " + listen_addr + ":" + std::to_string(port) + " 失败: " + strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    running_ = true;
    th_ = std::thread(&HttpServer::accept_loop, this);
    return true;
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
    if (th_.joinable()) th_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
    // 连接线程是 detach 的, 还引用着 handler 的上下文; 最坏情况卡在 5s 的发送超时上,
    // 所以等满 6s, 免得调用方先析构上下文
    for (int i = 0; i < 600 && active_ > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void HttpServer::accept_loop() {
    while (running_) {
        pollfd p{listen_fd_, POLLIN, 0};
        const int pr = poll(&p, 1, 500);
        if (pr <= 0) continue;
        sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int fd = accept4(listen_fd_, (sockaddr*)&peer, &plen, SOCK_CLOEXEC);
        if (fd < 0) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (active_ >= max_clients_) {
            HttpConn c(fd);
            c.reply_json(503, "{\"error\":\"too many clients\"}");
            ::close(fd);
            continue;
        }
        ++active_;
        std::thread(&HttpServer::serve, this, fd, std::string(ip)).detach();
    }
}

void HttpServer::serve(int fd, std::string peer) {
    HttpConn conn(fd);
    std::string buf;
    size_t header_end = std::string::npos;
    char tmp[4096];
    while (header_end == std::string::npos) {
        const ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        buf.append(tmp, (size_t)r);
        header_end = buf.find("\r\n\r\n");
        if (buf.size() > kMaxHeader) break;
    }

    HttpRequest req;
    req.peer = peer;
    bool ok = header_end != std::string::npos;
    if (ok) {
        const std::string head = buf.substr(0, header_end);
        size_t line_end = head.find("\r\n");
        const std::string first = head.substr(0, line_end);
        const size_t s1 = first.find(' ');
        const size_t s2 = first.find(' ', s1 + 1);
        ok = s1 != std::string::npos && s2 != std::string::npos;
        if (ok) {
            req.method = first.substr(0, s1);
            const std::string target = first.substr(s1 + 1, s2 - s1 - 1);
            const size_t q = target.find('?');
            req.path = url_decode(target.substr(0, q));
            if (q != std::string::npos) req.query = target.substr(q + 1);
        }
        size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
        while (ok && pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            if (e == std::string::npos) e = head.size();
            const std::string line = head.substr(pos, e - pos);
            const size_t colon = line.find(':');
            if (colon != std::string::npos)
                req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
            pos = e + 2;
        }
    }

    if (ok) {
        const size_t clen = (size_t)strtoull(req.header("content-length").c_str(), nullptr, 10);
        if (clen > kMaxBody) {
            conn.reply_json(413, "{\"error\":\"body too large\"}");
            ok = false;
        } else {
            req.body = buf.substr(header_end + 4);
            while (req.body.size() < clen) {
                const ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
                if (r <= 0) break;
                req.body.append(tmp, (size_t)r);
            }
            if (req.body.size() < clen) ok = false;
            else req.body.resize(clen);
        }
    } else if (!buf.empty()) {
        conn.reply_json(400, "{\"error\":\"bad request\"}");
    }

    if (ok && handler_) {
        handler_(req, conn);
        if (!conn.sent_headers()) conn.reply_json(500, "{\"error\":\"no response\"}");
    }
    shutdown(fd, SHUT_WR);
    ::close(fd);
    --active_;
}

}  // namespace mb
