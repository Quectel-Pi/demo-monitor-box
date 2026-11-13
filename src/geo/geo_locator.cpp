#include "geo/geo_locator.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mb {

namespace {

constexpr double kIpAccuracyM = 5000.0;  // 公网 IP 只能到城市级

bool known_provider(const std::string& p) {
    return p == "wifi_google" || p == "ip" || p == "manual";
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool valid_mac(const std::string& m) {
    if (m.size() != 17) return false;
    for (size_t i = 0; i < m.size(); ++i) {
        if (i % 3 == 2 ? m[i] != ':' : !isxdigit((unsigned char)m[i])) return false;
    }
    return true;
}

std::string iso_local(time_t t) {
    char buf[32];
    struct tm tmv;
    if (t <= 0 || !localtime_r(&t, &tmv) || !strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv))
        return "";
    return buf;
}

// 2.4G: 1..14, 5G: 36..165 对应的频率 (MHz) -> 信道
int freq_to_channel(int mhz) {
    if (mhz == 2484) return 14;
    if (mhz >= 2412 && mhz <= 2472) return (mhz - 2407) / 5;
    if (mhz >= 5000 && mhz <= 5900) return (mhz - 5000) / 5;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------

GeoParams GeoParams::from_json(const Json& cfg) {
    GeoParams p;
    const Json& g = cfg["geo"];
    if (!g.is_object()) return p;
    p.enabled = g["enabled"].boolean(true);
    if (g["providers"].is_array()) {
        p.providers.clear();
        for (const Json& x : g["providers"].items()) {
            const std::string s = x.str("");
            if (known_provider(s)) p.providers.push_back(s);
        }
    }
    p.refresh_min = std::max(1.0, g["refresh_min"].number(30.0));
    p.timeout_sec = std::max(2, std::min(60, g["timeout_sec"].integer(10)));
    p.wifi_iface = g["wifi_iface"].str("wlan0");
    p.google_api_key = trim(g["google_api_key"].str(""));
    p.google_url = g["google_url"].str(p.google_url);
    p.ip_url = g["ip_url"].str(p.ip_url);
    const Json& m = g["manual"];
    if (m["lat"].is_number() && m["lon"].is_number()) {
        p.has_manual = true;
        p.manual_lat = m["lat"].number(0.0);
        p.manual_lon = m["lon"].number(0.0);
        p.manual_accuracy_m = std::max(0.0, m["accuracy_m"].number(50.0));
        p.manual_address = m["address"].str("");
    }
    return p;
}

bool GeoParams::validate(const Json& geo, std::string* err) {
    if (geo.is_null()) return true;
    auto fail = [&](const std::string& s) {
        if (err) *err = s;
        return false;
    };
    if (!geo.is_object()) return fail("geo 必须是对象");
    if (geo.has("enabled") && geo["enabled"].type() != Json::Type::Bool)
        return fail("geo.enabled 必须是布尔");
    const Json& pr = geo["providers"];
    if (!pr.is_null()) {
        if (!pr.is_array()) return fail("geo.providers 必须是数组");
        for (const Json& x : pr.items()) {
            if (!known_provider(x.str("")))
                return fail("geo.providers 只能是 wifi_google / ip / manual: " + x.str(""));
        }
    }
    for (const char* k : {"wifi_iface", "google_api_key", "google_url", "ip_url"}) {
        if (geo.has(k) && geo[k].type() != Json::Type::String)
            return fail(std::string("geo.") + k + " 必须是字符串");
    }
    for (const char* k : {"google_url", "ip_url"}) {
        const std::string u = geo[k].str("");
        if (!u.empty() && u.compare(0, 7, "http://") != 0 && u.compare(0, 8, "https://") != 0)
            return fail(std::string("geo.") + k + " 必须是 http(s) 地址");
    }
    const Json& m = geo["manual"];
    if (!m.is_null()) {
        if (!m.is_object()) return fail("geo.manual 必须是对象");
        const Json& lat = m["lat"];
        const Json& lon = m["lon"];
        if (!lat.is_null() && (!lat.is_number() || std::fabs(lat.number(0)) > 90.0))
            return fail("geo.manual.lat 必须在 -90..90");
        if (!lon.is_null() && (!lon.is_number() || std::fabs(lon.number(0)) > 180.0))
            return fail("geo.manual.lon 必须在 -180..180");
        if (lat.is_null() != lon.is_null()) return fail("geo.manual 的 lat / lon 要同时填");
    }
    return true;
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

std::vector<WifiAp> GeoLocator::parse_nmcli(const std::string& text) {
    std::vector<WifiAp> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        // -t 模式下字段用 ':' 分隔, 字段内的 ':' 写成 '\:'
        std::vector<std::string> fields(1);
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                fields.back() += line[++i];
            } else if (line[i] == ':') {
                fields.emplace_back();
            } else {
                fields.back() += line[i];
            }
        }
        if (fields.size() < 3) continue;
        WifiAp ap;
        ap.mac = lower(trim(fields[0]));
        if (!valid_mac(ap.mac)) continue;
        const int quality = atoi(fields[1].c_str());  // 0..100
        ap.signal_dbm = std::max(-100, std::min(-30, quality / 2 - 100));
        ap.channel = atoi(fields[2].c_str());
        out.push_back(ap);
    }
    return out;
}

std::vector<WifiAp> GeoLocator::parse_iw(const std::string& text) {
    std::vector<WifiAp> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string raw = text.substr(pos, nl - pos);
        pos = nl + 1;
        const std::string line = trim(raw);
        if (line.compare(0, 4, "BSS ") == 0) {
            WifiAp ap;
            ap.mac = lower(line.substr(4, 17));
            if (valid_mac(ap.mac)) out.push_back(ap);
            continue;
        }
        if (out.empty()) continue;
        WifiAp& ap = out.back();
        float f = 0.0f;
        int n = 0;
        if (sscanf(line.c_str(), "signal: %f dBm", &f) == 1) {
            ap.signal_dbm = (int)std::lround(f);
        } else if (sscanf(line.c_str(), "freq: %f", &f) == 1 && ap.channel == 0) {
            ap.channel = freq_to_channel((int)std::lround(f));
        } else if (sscanf(line.c_str(), "DS Parameter set: channel %d", &n) == 1 ||
                   sscanf(line.c_str(), "* primary channel: %d", &n) == 1) {
            ap.channel = n;
        }
    }
    return out;
}

std::string GeoLocator::google_request(const std::vector<WifiAp>& aps) {
    Json req = Json::object();
    req.set("considerIp", Json::bool_value(false));
    Json list = Json::array();
    for (const WifiAp& ap : aps) {
        Json a = Json::object();
        a.set("macAddress", Json::string_value(ap.mac));
        a.set("signalStrength", Json::number_value(ap.signal_dbm));
        if (ap.channel > 0) a.set("channel", Json::number_value(ap.channel));
        list.push(a);
    }
    req.set("wifiAccessPoints", list);
    return req.dump();
}

bool GeoLocator::parse_google(const std::string& body, GeoFix* out, std::string* err) {
    Json j;
    std::string perr;
    if (!Json::parse(body, &j, &perr) || !j.is_object()) {
        if (err) *err = "响应不是 JSON: " + body.substr(0, 120);
        return false;
    }
    if (j["error"].is_object()) {
        if (err) *err = "Google: " + j["error"]["message"].str("未知错误");
        return false;
    }
    const Json& loc = j["location"];
    if (!loc["lat"].is_number() || !loc["lng"].is_number()) {
        if (err) *err = "Google 响应缺少 location";
        return false;
    }
    GeoFix f;
    f.valid = true;
    f.lat = loc["lat"].number(0.0);
    f.lon = loc["lng"].number(0.0);
    f.accuracy_m = j["accuracy"].number(0.0);
    f.source = "wifi_google";
    if (out) *out = f;
    return true;
}

bool GeoLocator::parse_ip(const std::string& body, GeoFix* out, std::string* err) {
    Json j;
    std::string perr;
    if (!Json::parse(body, &j, &perr) || !j.is_object()) {
        if (err) *err = "响应不是 JSON: " + body.substr(0, 120);
        return false;
    }
    if (j["status"].str("success") != "success" || j["error"].boolean(false)) {
        if (err) *err = "IP 定位失败: " + j["message"].str(j["reason"].str("未知原因"));
        return false;
    }
    GeoFix f;
    bool ok = false;
    if (j["lat"].is_number() && j["lon"].is_number()) {  // ip-api.com
        f.lat = j["lat"].number(0.0);
        f.lon = j["lon"].number(0.0);
        ok = true;
    } else if (j["latitude"].is_number() && j["longitude"].is_number()) {  // ipapi.co
        f.lat = j["latitude"].number(0.0);
        f.lon = j["longitude"].number(0.0);
        ok = true;
    } else if (j["loc"].type() == Json::Type::String) {  // ipinfo.io "lat,lon"
        ok = sscanf(j["loc"].str("").c_str(), "%lf,%lf", &f.lat, &f.lon) == 2;
    }
    if (!ok || std::fabs(f.lat) > 90.0 || std::fabs(f.lon) > 180.0) {
        if (err) *err = "IP 定位响应缺少经纬度";
        return false;
    }
    std::string addr;
    for (const char* k : {"country", "country_name", "regionName", "region", "city"}) {
        const std::string v = j[k].str("");
        if (v.empty() || addr.find(v) != std::string::npos) continue;
        if (!addr.empty()) addr += " ";
        addr += v;
    }
    f.valid = true;
    f.accuracy_m = kIpAccuracyM;
    f.address = addr;
    f.source = "ip";
    if (out) *out = f;
    return true;
}

// ---------------------------------------------------------------------------
// 外部命令
// ---------------------------------------------------------------------------

int GeoLocator::run_command(const std::vector<std::string>& argv, int timeout_sec,
                            std::string* out) {
    if (argv.empty()) return -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        std::vector<char*> args;
        for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    std::string buf;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    bool timed_out = false;
    for (;;) {
        const int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (left <= 0) {
            timed_out = true;
            break;
        }
        struct pollfd pfd = {fds[0], POLLIN, 0};
        const int r = poll(&pfd, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            timed_out = r == 0;
            break;
        }
        char chunk[4096];
        const ssize_t n = read(fds[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (buf.size() < (1u << 20)) buf.append(chunk, (size_t)n);
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (out) *out = buf;
    if (timed_out) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// 定位
// ---------------------------------------------------------------------------

GeoLocator::GeoLocator(Runner runner) : runner_(std::move(runner)) {}

GeoLocator::~GeoLocator() { stop(); }

void GeoLocator::configure(const GeoParams& p) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool changed = p.providers != params_.providers || p.enabled != params_.enabled ||
                         p.google_api_key != params_.google_api_key || p.ip_url != params_.ip_url ||
                         p.has_manual != params_.has_manual || p.manual_lat != params_.manual_lat ||
                         p.manual_lon != params_.manual_lon || p.wifi_iface != params_.wifi_iface;
    params_ = p;
    if (changed) refresh_ = true;
    cv_.notify_all();
}

void GeoLocator::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return;
    running_ = true;
    th_ = std::thread(&GeoLocator::loop, this);
}

void GeoLocator::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_) return;
        running_ = false;
        cv_.notify_all();
    }
    if (th_.joinable()) th_.join();
}

void GeoLocator::request_refresh() {
    std::lock_guard<std::mutex> lk(mu_);
    refresh_ = true;
    cv_.notify_all();
}

void GeoLocator::loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (running_) {
        const time_t now = time(nullptr);
        // 一次都没成功时 2 分钟后重试, 成功后按 refresh_min 刷新
        const double interval = fix_.valid ? params_.refresh_min * 60.0 : 120.0;
        const bool due = params_.enabled &&
                         (refresh_ || last_attempt_ == 0 || difftime(now, last_attempt_) >= interval);
        if (!due) {
            cv_.wait_for(lk, std::chrono::seconds(30));
            continue;
        }
        refresh_ = false;
        lk.unlock();
        locate_once();
        lk.lock();
    }
}

std::vector<WifiAp> GeoLocator::scan_wifi(const GeoParams& p, std::string* err) {
    std::string out;
    std::vector<WifiAp> aps;
    int rc = runner_({"nmcli", "-t", "-f", "BSSID,SIGNAL,CHAN", "dev", "wifi", "list", "ifname",
                      p.wifi_iface, "--rescan", "auto"},
                     p.timeout_sec, &out);
    if (rc == 0) aps = parse_nmcli(out);
    if (aps.size() < 2) {
        rc = runner_({"iw", "dev", p.wifi_iface, "scan"}, p.timeout_sec, &out);
        if (rc == 0) aps = parse_iw(out);
    }
    if (aps.size() < 2) {
        if (err)
            *err = rc == 127 || rc < 0 ? "WiFi 扫描失败 (nmcli / iw 不可用或超时)"
                                       : "附近 WiFi 热点少于 2 个, 无法定位";
        return {};
    }
    // 信号最强的在前, 最多送 20 个
    std::sort(aps.begin(), aps.end(),
              [](const WifiAp& a, const WifiAp& b) { return a.signal_dbm > b.signal_dbm; });
    if (aps.size() > 20) aps.resize(20);
    return aps;
}

bool GeoLocator::try_provider(const std::string& name, const GeoParams& p, GeoFix* fix,
                              std::string* err) {
    if (name == "manual") {
        if (!p.has_manual) {
            if (err) *err = "未填写手动经纬度";
            return false;
        }
        fix->valid = true;
        fix->lat = p.manual_lat;
        fix->lon = p.manual_lon;
        fix->accuracy_m = p.manual_accuracy_m;
        fix->address = p.manual_address;
        fix->source = "manual";
        return true;
    }
    if (name == "wifi_google") {
        if (p.google_api_key.empty()) {
            if (err) *err = "未配置 google_api_key";
            return false;
        }
        const std::vector<WifiAp> aps = scan_wifi(p, err);
        if (aps.empty()) return false;
        std::string body;
        const int rc = runner_({"curl", "-sS", "-m", std::to_string(p.timeout_sec), "-H",
                                "Content-Type: application/json", "--data-binary",
                                google_request(aps), p.google_url + "?key=" + p.google_api_key},
                               p.timeout_sec + 2, &body);
        if (rc != 0) {
            if (err) *err = "请求 Google 失败 (curl " + std::to_string(rc) + "): " + trim(body).substr(0, 120);
            return false;
        }
        return parse_google(body, fix, err);
    }
    if (name == "ip") {
        std::string body;
        const int rc = runner_({"curl", "-sS", "-m", std::to_string(p.timeout_sec), p.ip_url},
                               p.timeout_sec + 2, &body);
        if (rc != 0) {
            if (err) *err = "请求 IP 定位失败 (curl " + std::to_string(rc) + "): " + trim(body).substr(0, 120);
            return false;
        }
        return parse_ip(body, fix, err);
    }
    if (err) *err = "未知 provider";
    return false;
}

bool GeoLocator::locate_once() {
    GeoParams p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        p = params_;
    }
    if (!p.enabled) return false;
    std::map<std::string, std::string> attempts;
    GeoFix got;
    for (const std::string& name : p.providers) {
        GeoFix f;
        std::string err;
        if (try_provider(name, p, &f, &err)) {
            attempts[name] = "ok";
            got = f;
            break;
        }
        attempts[name] = err;
    }
    std::lock_guard<std::mutex> lk(mu_);
    last_attempt_ = time(nullptr);
    attempts_ = attempts;
    if (got.valid) {
        got.wall = last_attempt_;
        const bool moved = !fix_.valid || got.source != fix_.source ||
                           std::fabs(got.lat - fix_.lat) > 1e-4 || std::fabs(got.lon - fix_.lon) > 1e-4;
        fix_ = got;
        if (moved)
            printf("[geo] 设备位置 %.6f, %.6f (±%.0fm, %s)\n", got.lat, got.lon, got.accuracy_m,
                   got.source.c_str());
        return true;
    }
    printf("[geo] 设备定位失败, 保留上次结果\n");
    return false;
}

GeoFix GeoLocator::fix() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fix_;
}

Json GeoLocator::json() const {
    std::lock_guard<std::mutex> lk(mu_);
    Json j = Json::object();
    j.set("enabled", Json::bool_value(params_.enabled));
    j.set("valid", Json::bool_value(fix_.valid));
    if (fix_.valid) {
        j.set("lat", Json::number_value(fix_.lat));
        j.set("lon", Json::number_value(fix_.lon));
        j.set("accuracy_m", Json::number_value(fix_.accuracy_m));
        j.set("source", Json::string_value(fix_.source));
        j.set("address", Json::string_value(fix_.address));
        j.set("updated", Json::string_value(iso_local(fix_.wall)));
        j.set("age_sec", Json::number_value(difftime(time(nullptr), fix_.wall)));
    }
    Json providers = Json::array();
    for (const std::string& s : params_.providers) providers.push(Json::string_value(s));
    j.set("providers", providers);
    Json a = Json::object();
    for (const auto& kv : attempts_) a.set(kv.first, Json::string_value(kv.second));
    j.set("attempts", a);
    j.set("last_attempt", Json::string_value(iso_local(last_attempt_)));
    return j;
}

}  // namespace mb
