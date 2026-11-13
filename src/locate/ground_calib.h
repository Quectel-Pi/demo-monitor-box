#ifndef MONITOR_BOX_LOCATE_GROUND_CALIB_H_
#define MONITOR_BOX_LOCATE_GROUND_CALIB_H_

#include <string>
#include <vector>

#include "common/geometry.h"
#include "common/json.h"

namespace mb {

// 一组画面点 <-> 地面点对应。img 为归一化画面坐标 (0..1), ground 为米。
struct GroundPair {
    Point img;
    Point ground;
};

// 标定质量。拟合残差 (rmse_m) 只说明标定点之间是否自洽, 真正的定位精度看检查点
// (check_*): 检查点不参与拟合, 它们的误差就是同一相机下定位误差的直接估计。
struct CalibQuality {
    std::string grade = "invalid";  // good / fair / poor / invalid
    int n_points = 0;
    int n_check = 0;
    float rmse_m = 0.0f;
    float max_err_m = 0.0f;
    bool has_check = false;
    float check_rmse_m = 0.0f;
    float check_max_m = 0.0f;
    std::vector<float> residuals_m;
    std::vector<float> check_residuals_m;
    float coverage = 0.0f;  // 标定点在画面上的凸包面积占比
    std::vector<std::string> warnings;

    // 定位误差的最佳估计 (米): 有检查点用检查点, 有冗余点用拟合残差, 都没有返回 -1。
    float error_estimate_m() const;
};

// 地面平面标定: 归一化画面坐标 <-> 地面米制坐标的单应性。
//
// 两种来源:
//   rect   旧配置: 地面上一个已知尺寸的矩形 (4 个画面角点 + real_size_m)
//   points N 点对应 (>= 4): 地上随便量几个点的坐标, 最小二乘拟合, 多出来的点用来评估误差
// 两种都可以加 check_points (不参与拟合) 做精度评估。
//
// 拟合用归一化 DLT (Hartley 归一化 + A^T A 最小特征向量), N=4 时就是精确解。
class GroundCalib {
public:
    // 从一个 ground 区域的 JSON 取出对应点 (配置坐标, 未经画面对齐)。
    // 区域没有任何标定信息 (既无 calib_points 又无 real_size_m) 时返回 false 且 err 为空;
    // 有但写错了返回 false 且 err 非空。
    static bool parse_region(const Json& r, std::vector<GroundPair>* calib,
                             std::vector<GroundPair>* check, std::string* mode, std::string* err);

    // 拟合并评估。失败时 valid() = false, quality().grade = "invalid"。
    bool fit(const std::vector<GroundPair>& calib, const std::vector<GroundPair>& check,
             std::string* err);

    bool valid() const { return valid_; }
    // 画面 -> 地面。地平线以上 (或正好在地平线上) 的点返回 false。
    bool to_ground(Point img_norm, Point* g) const;
    bool to_image(Point g, Point* img_norm) const;
    // 脚点有 sigma_px 像素噪声时, 地面位置的不确定度 (米, 两个方向的均方根)。
    // 越远一个像素对应的地面距离越大, 这就是"远处定位不准"的量化。
    float sigma_m(Point img_norm, float sigma_px, int w, int h) const;
    // 地面点是否落在标定覆盖范围 (标定点的凸包) 内。
    bool in_hull(Point g) const;

    const Mat3& h() const { return h_; }
    const Mat3& h_inv() const { return h_inv_; }
    const std::vector<Point>& hull_m() const { return hull_m_; }
    const CalibQuality& quality() const { return q_; }
    // minx, miny, maxx, maxy
    void bounds(float out[4]) const;

    // GET /ground 的 calib 段 (不含 region_id / name / mode, 由调用方补)。
    Json to_json() const;

private:
    bool valid_ = false;
    Mat3 h_;
    Mat3 h_inv_;
    float w_sign_ = 1.0f;  // 标定点处 w 的符号; 符号相反的点在地平线另一侧
    std::vector<Point> hull_m_;
    CalibQuality q_;
};

// 凸包 (Andrew 单调链), 逆时针, 不含共线点。少于 3 个不同点时原样返回。
std::vector<Point> convex_hull(std::vector<Point> pts);

}  // namespace mb

#endif  // MONITOR_BOX_LOCATE_GROUND_CALIB_H_
