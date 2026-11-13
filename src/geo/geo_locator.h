#ifndef MONITOR_BOX_GEO_GEO_LOCATOR_H_
#define MONITOR_BOX_GEO_GEO_LOCATOR_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/json.h"

namespace mb {

// 设备经纬度 (不是画面里目标的位置)。以网络方式为主:
//   wifi_google  扫周围 WiFi 热点 -> Google Geolocation API (需要 key), 几十米级
//   ip           公网 IP 粗定位 (城市级, 免 key)
//   manual       配置里手填的坐标, 兜底
// 按 geo.providers 的顺序尝试, 第一个成功的为准; 全失败时保留上一次的结果。
struct GeoParams {
    bool enabled = true;
    std::vector<std::string> providers{"wifi_google", "ip", "manual"};
    double refresh_min = 30.0;
    int timeout_sec = 10;
    std::string wifi_iface = "wlan0";
    std::string google_api_key;
    std::string google_url = "https://www.googleapis.com/geolocation/v1/geolocate";
    std::string ip_url =
        "http://ip-api.com/json/?fields=status,message,lat,lon,city,regionName,country,query";
    bool has_manual = false;
    double manual_lat = 0.0;
    double manual_lon = 0.0;
    double manual_accuracy_m = 50.0;
    std::string manual_address;

    static GeoParams from_json(const Json& cfg);
    // PUT /config 用: geo 段写错了 (未知 provider / 经纬度越界) 返回 false。
    static bool validate(const Json& geo, std::string* err);
};

struct GeoFix {
    bool valid = false;
    double lat = 0.0;
    double lon = 0.0;
    double accuracy_m = 0.0;
    std::string source;
    std::string address;
    time_t wall = 0;
};

struct WifiAp {
    std::string mac;   // aa:bb:cc:dd:ee:ff 小写
    int signal_dbm = -100;
    int channel = 0;
};

class GeoLocator {
public:
    // 执行外部命令 (不经过 shell), 返回退出码; 超时/启动失败返回 -1。测试里可以替换。
    using Runner = std::function<int(const std::vector<std::string>& argv, int timeout_sec,
                                     std::string* out)>;
    static int run_command(const std::vector<std::string>& argv, int timeout_sec, std::string* out);

    explicit GeoLocator(Runner runner = run_command);
    ~GeoLocator();
    GeoLocator(const GeoLocator&) = delete;
    GeoLocator& operator=(const GeoLocator&) = delete;

    void configure(const GeoParams& p);
    void start();
    void stop();
    void request_refresh();

    // 同步跑一遍 provider 链 (后台线程调用; 测试直接调)。
    bool locate_once();

    GeoFix fix() const;
    Json json() const;

    // ---- 解析 (纯函数, 可测) ----
    // nmcli -t -f BSSID,SIGNAL,CHAN dev wifi list 的输出 (BSSID 里的 ':' 被转义成 '\:')
    static std::vector<WifiAp> parse_nmcli(const std::string& text);
    // iw dev <if> scan 的输出
    static std::vector<WifiAp> parse_iw(const std::string& text);
    static std::string google_request(const std::vector<WifiAp>& aps);
    static bool parse_google(const std::string& body, GeoFix* out, std::string* err);
    // ip-api.com / ipapi.co / ipinfo.io 风格的响应
    static bool parse_ip(const std::string& body, GeoFix* out, std::string* err);

private:
    bool try_provider(const std::string& name, const GeoParams& p, GeoFix* fix, std::string* err);
    std::vector<WifiAp> scan_wifi(const GeoParams& p, std::string* err);
    void loop();

    Runner runner_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    GeoParams params_;
    GeoFix fix_;
    std::map<std::string, std::string> attempts_;
    time_t last_attempt_ = 0;
    bool refresh_ = true;
    bool running_ = false;
    std::thread th_;
};

}  // namespace mb

#endif  // MONITOR_BOX_GEO_GEO_LOCATOR_H_
