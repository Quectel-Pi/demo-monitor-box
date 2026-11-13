#ifndef MONITOR_BOX_COMMON_GEOMETRY_H_
#define MONITOR_BOX_COMMON_GEOMETRY_H_

#include <cstddef>
#include <vector>

namespace mb {

// 二维点。图像坐标系 x 向右、y 向下; 地面坐标系 x 向右、y 向远处 (见 homography)。
struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

// 射线法判断点是否在多边形内 (奇偶规则, 边界视为在内)。
// 所有监控类空间判断 (入侵/围栏/滞留/区域定位) 都走这一个函数。
bool point_in_polygon(const std::vector<Point>& poly, Point p);

// 点到多边形边界 (各条边) 的最短距离, 与点在内在外无关。点数 < 2 返回极大值。
// 区域滞回用: 脚点刚出界但离边界很近时, 视为边界抖动而不是真的离开。
float distance_to_polygon_edge(const std::vector<Point>& poly, Point p);

// 多边形面积 (绝对值)。点数 < 3 返回 0。
float polygon_area(const std::vector<Point>& poly);

// 多边形重心 (简单顶点平均, 够画标注用; 不做面积加权)。
Point polygon_centroid(const std::vector<Point>& poly);

// 线段 ab 与 cd 是否相交。相交时把交点写进 hit。
bool segments_intersect(Point a, Point b, Point c, Point d, Point* hit);

// 叉积 cross(b-a, c-a)。>0 表示 c 在 ab 左侧 (图像坐标里 y 向下, 视觉上其实是右侧)。
float cross(const Point& a, const Point& b, const Point& c);

// 3x3 单应性矩阵, 行主序。h[8] 恒为 1。
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

// 由 4 组对应点求单应性 (DLT, 8x8 高斯消元)。退化四边形返回 false。
// 用于 #9 地面定位: 画面里的地面四边形 (归一化或像素坐标) -> 米制平面。
bool homography_from_quad(const Point src[4], const Point dst[4], Mat3* out);

// 单应性变换。w 接近 0 (点落到无穷远) 时返回 false。
bool apply_homography(const Mat3& h, Point p, Point* out);

// 求逆。奇异返回 false。
bool invert_mat3(const Mat3& in, Mat3* out);

// 2x3 仿射: x' = m0*x + m1*y + m2, y' = m3*x + m4*y + m5。
// 画面对齐用: 把配置里按参考画面画的归一化区域映射到当前画面。
struct Affine2 {
    float m[6] = {1, 0, 0, 0, 1, 0};
    Point apply(Point p) const {
        return Point{m[0] * p.x + m[1] * p.y + m[2], m[3] * p.x + m[4] * p.y + m[5]};
    }
    bool is_identity(float eps = 1e-6f) const;
};

// outer(inner(p))
Affine2 compose(const Affine2& outer, const Affine2& inner);
bool invert_affine(const Affine2& in, Affine2* out);

// 归一化坐标 (0..1) -> 像素坐标。
inline Point denormalize(Point p, int w, int h) { return Point{p.x * (float)w, p.y * (float)h}; }

}  // namespace mb

#endif  // MONITOR_BOX_COMMON_GEOMETRY_H_
