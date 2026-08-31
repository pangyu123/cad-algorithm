#pragma once

/// @file BoundingCylinder.h
/// @brief 计算 TopoDS_Shape 的紧密包围圆柱体。
///
/// 算法概要：
///   1. 对 shape 表面做密集采样，获取三维点集。
///   2. 生成候选圆柱轴方向（PCA 主方向、OBB 轴方向）。
///   3. 对每个候选轴：
///      a. 将采样点投影到垂直于轴的平面上，求 2D 最小包围圆（Welzl 算法）。
///      b. 将采样点投影到轴上，求轴向区间。
///      c. 记录圆柱体积 = π·r²·h。
///   4. 选择体积最小的候选轴。
///   5. 用 BRepPrimAPI_MakeCylinder 生成实体圆柱。
///
/// 与纯 OBB 方法的区别：
///   本算法直接对采样点求最小包围圆与轴向区间，而非在 OBB 外包圆柱。
///   在非矩形截面的 shape 上通常能得到更紧的圆柱。

// ---------------------------------------------------------------------------
//  Windows M_PI
// ---------------------------------------------------------------------------
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
//  OCCT
// ---------------------------------------------------------------------------
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>

#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>

#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

#include <Bnd_OBB.hxx>

#include <Poly_Triangulation.hxx>
#include <TColgp_Array1OfPnt.hxx>

#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_XY.hxx>
#include <gp_Dir.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>
#include <gp_Mat.hxx>

#include <Precision.hxx>

// ---------------------------------------------------------------------------
//  STL
// ---------------------------------------------------------------------------
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

// ===========================================================================
//  BoundingCylinder
// ===========================================================================
namespace BoundingCylinder
{

// ---------------------------------------------------------------------------
//  轴选择策略
// ---------------------------------------------------------------------------
enum class AxisPolicy
{
    /// 使用 PCA 第一主方向作为圆柱轴（通常是最长方向）。
    PcaFirstPrincipal,

    /// 遍历 PCA 三个主方向 + OBB 三个轴方向，
    /// 选择体积最小的那个。
    MinimumVolume,

    /// 仅使用 OBB 最长轴（与旧实现兼容）。
    ObbLongestAxis
};

// ---------------------------------------------------------------------------
//  选项
// ---------------------------------------------------------------------------
struct Options
{
    /// 轴选择策略。
    AxisPolicy axisPolicy = AxisPolicy::MinimumVolume;

    /// 表面采样密度（每平方毫米约采样多少个点）。
    /// 增大该值可获得更精确的结果，但计算量上升。
    double sampleDensity = 0.25;

    /// 最少采样点数。
    int minimumSampleCount = 256;

    /// 最多采样点数（防止大模型计算过慢）。
    int maximumSampleCount = 16384;

    /// 是否将 shape 的三角网格顶点也纳入采样。
    bool useTriangulation = true;

    /// 是否使用 OBB 轴作为额外的候选方向。
    bool useObbAxes = true;

    /// 半径方向额外扩展量。
    double radialClearance = 0.0;

    /// 轴向两端分别额外扩展的距离。
    double axialClearance = 0.0;

    /// 最小圆柱半径（防止退化 shape 产生零半径）。
    double minimumRadius = Precision::Confusion() * 100.0;

    /// 最小圆柱高度（防止退化 shape 产生零高度）。
    double minimumHeight = Precision::Confusion() * 100.0;
};

// ---------------------------------------------------------------------------
//  结果
// ---------------------------------------------------------------------------
struct Result
{
    /// 生成的包围圆柱实体。
    TopoDS_Shape cylinder;

    /// 圆柱中心点（位于圆柱轴的中点）。
    gp_Pnt center;

    /// 底面圆心。
    gp_Pnt baseCenter;

    /// 圆柱中心轴方向。
    gp_Dir axis;

    /// 圆柱半径。
    double radius = 0.0;

    /// 圆柱高度。
    double height = 0.0;

    /// 轴来源说明。
    int axisSource = -1;
    //   0 = PCA 第一主方向
    //   1 = PCA 第二主方向
    //   2 = PCA 第三主方向
    //   10 = OBB X 轴
    //   11 = OBB Y 轴
    //   12 = OBB Z 轴

    /// 采样到的点的数量。
    int sampleCount = 0;
};

// ===========================================================================
//  内部实现
// ===========================================================================
namespace detail
{

// ---------------------------------------------------------------------------
//  三维向量助手
// ---------------------------------------------------------------------------
inline gp_XYZ ToXYZ(const gp_Pnt& p) { return p.XYZ(); }

inline gp_Pnt FromXYZ(const gp_XYZ& xyz)
{
    return gp_Pnt(xyz.X(), xyz.Y(), xyz.Z());
}

// ---------------------------------------------------------------------------
//  采样 —— 从 Shape 表面收集点位
// ---------------------------------------------------------------------------
inline std::vector<gp_Pnt> SamplePoints(
    const TopoDS_Shape& shape,
    const Options& options)
{
    std::vector<gp_Pnt> points;
    points.reserve(options.maximumSampleCount);

    // ---- 来源 A：三角网格顶点 ----
    if (options.useTriangulation)
    {
        // 确保 shape 已三角化
        BRepMesh_IncrementalMesh mesh(
            const_cast<TopoDS_Shape&>(shape),
            1.0,          // 线性偏转
            Standard_False, // 非相对模式
            0.5,          // 角度偏转
            Standard_True); // 并行

        for (TopExp_Explorer faceExp(shape, TopAbs_FACE);
             faceExp.More();
             faceExp.Next())
        {
            const TopoDS_Face& face = TopoDS::Face(faceExp.Current());

            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri =
                BRep_Tool::Triangulation(face, loc);

            if (tri.IsNull())
            {
                continue;
            }

            const TColgp_Array1OfPnt& nodes = tri->Nodes();
            const gp_Trsf& trsf = loc.Transformation();

            for (Standard_Integer i = 1; i <= nodes.Length(); ++i)
            {
                if (static_cast<int>(points.size()) >= options.maximumSampleCount)
                {
                    return points;
                }

                gp_Pnt p = nodes(i).Transformed(trsf);

                points.push_back(p);
            }
        }
    }

    // ---- 来源 B：如果三角网格顶点不够，在面上均匀采样 ----
    if (static_cast<int>(points.size()) < options.minimumSampleCount)
    {
        for (TopExp_Explorer faceExp(shape, TopAbs_FACE);
             faceExp.More();
             faceExp.Next())
        {
            const TopoDS_Face& face = TopoDS::Face(faceExp.Current());

            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri =
                BRep_Tool::Triangulation(face, loc);

            if (tri.IsNull())
            {
                continue;
            }

            const TColgp_Array1OfPnt& nodes = tri->Nodes();
            const Poly_Array1OfTriangle& triangles = tri->Triangles();
            const gp_Trsf& trsf = loc.Transformation();

            // 在每个三角形内部随机采样额外点
            const int triCount = triangles.Length();
            int extraPerTri = 0;

            if (triCount > 0 && static_cast<int>(points.size()) < options.maximumSampleCount)
            {
                const int remaining =
                    options.maximumSampleCount - static_cast<int>(points.size());

                extraPerTri = std::max(0, std::min(3, remaining / triCount));
            }

            if (extraPerTri <= 0)
            {
                continue;
            }

            // 确定性"随机"采样（使用三角形索引作为种子）
            for (Standard_Integer t = 1; t <= triCount; ++t)
            {
                if (static_cast<int>(points.size()) >= options.maximumSampleCount)
                {
                    break;
                }

                const Poly_Triangle& triV = triangles(t);

                Standard_Integer i1 = 0;
                Standard_Integer i2 = 0;
                Standard_Integer i3 = 0;
                triV.Get(i1, i2, i3);

                const gp_Pnt& n1 = nodes(i1);
                const gp_Pnt& n2 = nodes(i2);
                const gp_Pnt& n3 = nodes(i3);

                for (int e = 0; e < extraPerTri; ++e)
                {
                    // Halton 序列做拟随机采样
                    const double u = (e == 0) ? 0.3 : ((e == 1) ? 0.6 : 0.9);
                    const double v = (e == 0) ? 0.3 : ((e == 1) ? 0.3 : 0.6);
                    const double w = 1.0 - u - v;

                    if (w < 0.0) continue;

                    gp_XYZ xyz =
                        n1.XYZ() * u +
                        n2.XYZ() * v +
                        n3.XYZ() * w;

                    points.push_back(gp_Pnt(xyz).Transformed(trsf));
                }
            }
        }
    }

    return points;
}

// ---------------------------------------------------------------------------
//  PCA —— 对点集做主成分分析
// ---------------------------------------------------------------------------
struct PcaResult
{
    gp_Pnt center;
    std::array<gp_Dir, 3> directions;
    std::array<double, 3> eigenvalues;
};

inline PcaResult ComputePCA(const std::vector<gp_Pnt>& points)
{
    PcaResult result;

    const std::size_t n = points.size();

    if (n == 0)
    {
        throw std::runtime_error("BoundingCylinder: no sample points for PCA.");
    }

    // ---- 均值（质心） ----
    gp_XYZ sum(0.0, 0.0, 0.0);

    for (const gp_Pnt& p : points)
    {
        sum += p.XYZ();
    }

    const gp_XYZ mean = sum / static_cast<double>(n);
    result.center = gp_Pnt(mean);

    // ---- 协方差矩阵 ----
    // 协方差矩阵是一个 3x3 对称矩阵。
    // cov[i][j] = sum( (x_i - mean_i) * (x_j - mean_j) ) / (n - 1)

    double m00 = 0.0; // cov(0,0) = var(X)
    double m11 = 0.0; // cov(1,1) = var(Y)
    double m22 = 0.0; // cov(2,2) = var(Z)
    double m01 = 0.0; // cov(0,1) = cov(1,0)
    double m02 = 0.0; // cov(0,2) = cov(2,0)
    double m12 = 0.0; // cov(1,2) = cov(2,1)

    for (const gp_Pnt& p : points)
    {
        const double dx = p.X() - mean.X();
        const double dy = p.Y() - mean.Y();
        const double dz = p.Z() - mean.Z();

        m00 += dx * dx;
        m11 += dy * dy;
        m22 += dz * dz;
        m01 += dx * dy;
        m02 += dx * dz;
        m12 += dy * dz;
    }

    const double inv = 1.0 / static_cast<double>(n - 1);

    m00 *= inv;
    m11 *= inv;
    m22 *= inv;
    m01 *= inv;
    m02 *= inv;
    m12 *= inv;

    // ---- Jacobi 特征值分解（3x3 对称矩阵） ----
    // 对协方差矩阵 M：
    //
    //     [ m00  m01  m02 ]
    // M = [ m01  m11  m12 ]
    //     [ m02  m12  m22 ]
    //
    // 用 Jacobi 方法求解特征值与特征向量。

    // 初始化特征向量矩阵为单位阵
    double v00 = 1.0, v01 = 0.0, v02 = 0.0;
    double v10 = 0.0, v11 = 1.0, v12 = 0.0;
    double v20 = 0.0, v21 = 0.0, v22 = 1.0;

    double a00 = m00, a01 = m01, a02 = m02;
    double a11 = m11, a12 = m12;
    double a22 = m22;

    constexpr int maxSweeps = 32;
    constexpr double epsilon = 1.0e-15;

    for (int sweep = 0; sweep < maxSweeps; ++sweep)
    {
        // 找到最大的非对角线元素
        double maxOffDiag = std::abs(a01);
        int p = 0;
        int q = 1;

        if (std::abs(a02) > maxOffDiag)
        {
            maxOffDiag = std::abs(a02);
            p = 0;
            q = 2;
        }

        if (std::abs(a12) > maxOffDiag)
        {
            maxOffDiag = std::abs(a12);
            p = 1;
            q = 2;
        }

        if (maxOffDiag < epsilon)
        {
            break; // 已收敛
        }

        // Jacobi 旋转消去 a[p][q]
        double app = 0.0;
        double aqq = 0.0;
        double apq = 0.0;

        if (p == 0 && q == 1)      { app = a00; aqq = a11; apq = a01; }
        else if (p == 0 && q == 2) { app = a00; aqq = a22; apq = a02; }
        else                       { app = a11; aqq = a22; apq = a12; }

        const double theta = 0.5 * std::atan2(2.0 * apq, aqq - app);
        const double c = std::cos(theta);
        const double s = std::sin(theta);

        // 更新对角元素
        const double appNew = c * c * app + s * s * aqq - 2.0 * s * c * apq;
        const double aqqNew = s * s * app + c * c * aqq + 2.0 * s * c * apq;

        if (p == 0 && q == 1)
        {
            // 更新矩阵元素
            const double a02New = c * a02 - s * a12;
            const double a12New = s * a02 + c * a12;

            a00 = appNew;
            a11 = aqqNew;
            a01 = 0.0;
            a02 = a02New;
            a12 = a12New;
        }
        else if (p == 0 && q == 2)
        {
            const double a01New = c * a01 - s * a12;
            const double a12New = s * a01 + c * a12;

            a00 = appNew;
            a22 = aqqNew;
            a01 = a01New;
            a02 = 0.0;
            a12 = a12New;
        }
        else // p == 1, q == 2
        {
            const double a01New = c * a01 - s * a02;
            const double a02New = s * a01 + c * a02;

            a11 = appNew;
            a22 = aqqNew;
            a01 = a01New;
            a02 = a02New;
            a12 = 0.0;
        }

        // 更新特征向量矩阵 V = V * R
        for (int i = 0; i < 3; ++i)
        {
            double viP = 0.0;
            double viQ = 0.0;

            if (i == 0)
            {
                viP = (p == 0) ? v00 : ((p == 1) ? v01 : v02);
                viQ = (q == 0) ? v00 : ((q == 1) ? v01 : v02);
            }
            else if (i == 1)
            {
                viP = (p == 0) ? v10 : ((p == 1) ? v11 : v12);
                viQ = (q == 0) ? v10 : ((q == 1) ? v11 : v12);
            }
            else
            {
                viP = (p == 0) ? v20 : ((p == 1) ? v21 : v22);
                viQ = (q == 0) ? v20 : ((q == 1) ? v21 : v22);
            }

            const double viPNew = c * viP - s * viQ;
            const double viQNew = s * viP + c * viQ;

            if (i == 0)
            {
                if (p == 0) v00 = viPNew; else if (p == 1) v01 = viPNew; else v02 = viPNew;
                if (q == 0) v00 = viQNew; else if (q == 1) v01 = viQNew; else v02 = viQNew;
            }
            else if (i == 1)
            {
                if (p == 0) v10 = viPNew; else if (p == 1) v11 = viPNew; else v12 = viPNew;
                if (q == 0) v10 = viQNew; else if (q == 1) v11 = viQNew; else v12 = viQNew;
            }
            else
            {
                if (p == 0) v20 = viPNew; else if (p == 1) v21 = viPNew; else v22 = viPNew;
                if (q == 0) v20 = viQNew; else if (q == 1) v21 = viQNew; else v22 = viQNew;
            }
        }
    }

    // ---- 按特征值降序排列特征向量 ----
    // 特征值在对角线上：a00, a11, a22
    // 特征向量列：col0 = (v00, v10, v20), col1 = (v01, v11, v21), col2 = (v02, v12, v22)

    struct EigenPair
    {
        double value = 0.0;
        int index = 0;
    };

    std::array<EigenPair, 3> pairs = {{
        {a00, 0},
        {a11, 1},
        {a22, 2}
    }};

    std::sort(pairs.begin(), pairs.end(),
        [](const EigenPair& a_, const EigenPair& b_)
        {
            return a_.value > b_.value;
        });

    for (int i = 0; i < 3; ++i)
    {
        const int idx = pairs[i].index;

        double vx = 0.0;
        double vy = 0.0;
        double vz = 0.0;

        switch (idx)
        {
        case 0: vx = v00; vy = v10; vz = v20; break;
        case 1: vx = v01; vy = v11; vz = v21; break;
        case 2: vx = v02; vy = v12; vz = v22; break;
        }

        gp_Vec vec(vx, vy, vz);

        if (vec.Magnitude() < Precision::Confusion())
        {
            // 退化：使用单位阵
            vec = gp_Vec(
                (idx == 0) ? 1.0 : 0.0,
                (idx == 1) ? 1.0 : 0.0,
                (idx == 2) ? 1.0 : 0.0);
        }

        result.directions[i] = gp_Dir(vec);
        result.eigenvalues[i] = pairs[i].value;
    }

    return result;
}

// ---------------------------------------------------------------------------
//  最小包围圆（Welzl 算法）
// ---------------------------------------------------------------------------
struct Circle2D
{
    gp_XY center;
    double radius = 0.0;
};

namespace
{
    // 两点圆
    inline Circle2D CircleFrom2Points(
        const gp_XY& a,
        const gp_XY& b)
    {
        Circle2D c;
        c.center = gp_XY(
            0.5 * (a.X() + b.X()),
            0.5 * (a.Y() + b.Y()));
        c.radius = 0.5 * (a - b).Modulus();
        return c;
    }

    // 三点圆（外接圆）
    inline Circle2D CircleFrom3Points(
        const gp_XY& a,
        const gp_XY& b,
        const gp_XY& c_)
    {
        const double ax = a.X();
        const double ay = a.Y();
        const double bx = b.X();
        const double by = b.Y();
        const double cx = c_.X();
        const double cy = c_.Y();

        const double d =
            2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));

        if (std::abs(d) < 1e-20)
        {
            // 三点共线：退化到两点中距离最大的一对
            Circle2D ab = CircleFrom2Points(a, b);
            Circle2D bc = CircleFrom2Points(b, c_);
            Circle2D ca = CircleFrom2Points(c_, a);

            double rab = ab.radius;
            double rbc = bc.radius;
            double rca = ca.radius;

            if (rab >= rbc && rab >= rca) return ab;
            if (rbc >= rab && rbc >= rca) return bc;
            return ca;
        }

        const double ux =
            ((ax * ax + ay * ay) * (by - cy) +
             (bx * bx + by * by) * (cy - ay) +
             (cx * cx + cy * cy) * (ay - by)) / d;

        const double uy =
            ((ax * ax + ay * ay) * (cx - bx) +
             (bx * bx + by * by) * (ax - cx) +
             (cx * cx + cy * cy) * (bx - ax)) / d;

        const gp_XY center(ux, uy);
        const double r = (center - a).Modulus();

        Circle2D result;
        result.center = center;
        result.radius = r;
        return result;
    }

    // 从最多 3 个边界点构成的最小圆
    inline Circle2D TrivialCircle(
        const std::vector<gp_XY>& boundary)
    {
        if (boundary.empty())
        {
            return Circle2D();
        }

        if (boundary.size() == 1)
        {
            Circle2D c;
            c.center = boundary[0];
            c.radius = 0.0;
            return c;
        }

        if (boundary.size() == 2)
        {
            return CircleFrom2Points(boundary[0], boundary[1]);
        }

        return CircleFrom3Points(boundary[0], boundary[1], boundary[2]);
    }

    // Welzl 递归
    Circle2D WelzlRecursive(
        std::vector<gp_XY>& points,
        std::size_t n,
        std::vector<gp_XY>& boundary,
        std::mt19937& rng)
    {
        if (n == 0 || boundary.size() == 3)
        {
            return TrivialCircle(boundary);
        }

        // 随机选取一点
        const std::size_t idx = std::uniform_int_distribution<std::size_t>(
            0, static_cast<std::size_t>(n - 1))(rng);

        std::swap(points[idx], points[n - 1]);

        const gp_XY p = points[n - 1];

        // 先计算不包含 p 的最小圆
        Circle2D c = WelzlRecursive(points, n - 1, boundary, rng);

        // 如果 p 已在圆内，不需要更新
        const double dist = (p - c.center).Modulus();

        if (dist <= c.radius + 1e-12)
        {
            return c;
        }

        // p 在圆外，需要将它加入边界集
        boundary.push_back(p);
        c = WelzlRecursive(points, n - 1, boundary, rng);
        boundary.pop_back();

        return c;
    }
} // anonymous namespace

inline Circle2D MinimumEnclosingCircle(const std::vector<gp_XY>& points)
{
    if (points.empty())
    {
        return Circle2D();
    }

    // 复制一份以便原地 shuffle
    std::vector<gp_XY> pts = points;

    // 固定种子使结果可复现
    std::mt19937 rng(42);
    std::shuffle(pts.begin(), pts.end(), rng);

    std::vector<gp_XY> boundary;
    boundary.reserve(3);

    return WelzlRecursive(pts, pts.size(), boundary, rng);
}

// ---------------------------------------------------------------------------
//  轴候选评估
// ---------------------------------------------------------------------------
struct AxisCandidate
{
    gp_Dir direction;
    int source; // 用于填充 Result::axisSource
};

inline std::vector<AxisCandidate> GenerateCandidates(
    const PcaResult& pca,
    const Bnd_OBB& obb,
    const Options& options)
{
    std::vector<AxisCandidate> candidates;

    // PCA 方向
    for (int i = 0; i < 3; ++i)
    {
        candidates.push_back({pca.directions[i], i});
    }

    // 如果 PCA 误差较大，也尝试 OBB 方向
    if (options.useObbAxes && !obb.IsVoid())
    {
        candidates.push_back({gp_Dir(obb.XDirection()), 10});
        candidates.push_back({gp_Dir(obb.YDirection()), 11});
        candidates.push_back({gp_Dir(obb.ZDirection()), 12});
    }

    return candidates;
}

struct CandidateScore
{
    gp_Dir axis;
    double radius = 0.0;
    double height = 0.0;
    double volume = 0.0;
    int source = -1;

    gp_Pnt baseCenter;
    gp_Pnt center;
};

inline CandidateScore EvaluateCandidate(
    const std::vector<gp_Pnt>& points,
    const AxisCandidate& candidate,
    const Options& options)
{
    const gp_Dir& axis = candidate.direction;

    // 构造局部坐标系：Z 轴 = candidate.direction
    gp_Ax3 localAxes(gp_Pnt(0, 0, 0), axis);

    // 投影点到垂直于轴的平面上，并投影到轴上
    std::vector<gp_XY> projected2D;
    projected2D.reserve(points.size());

    double minZ = std::numeric_limits<double>::max();
    double maxZ = -std::numeric_limits<double>::max();

    for (const gp_Pnt& p : points)
    {
        // 用 gp_Ax3 变换到局部坐标
        gp_Vec toPoint(localAxes.Location(), p);

        const double x = toPoint.Dot(gp_Vec(localAxes.XDirection()));
        const double y = toPoint.Dot(gp_Vec(localAxes.YDirection()));
        const double z = toPoint.Dot(gp_Vec(localAxes.Direction()));

        projected2D.push_back(gp_XY(x, y));

        if (z < minZ) minZ = z;
        if (z > maxZ) maxZ = z;
    }

    // 2D 最小包围圆
    const Circle2D mec = MinimumEnclosingCircle(projected2D);

    CandidateScore score;
    score.axis = axis;
    score.radius = std::max(
        mec.radius + options.radialClearance,
        options.minimumRadius);

    const double rawHeight = maxZ - minZ + 2.0 * options.axialClearance;
    score.height = std::max(rawHeight, options.minimumHeight);
    score.volume = M_PI * score.radius * score.radius * score.height;
    score.source = candidate.source;

    // 局部坐标 → 全局坐标变换：
    //   P_global = O(0,0,0) + x * XDir + y * YDir + z * ZDir

    // 底面圆心：z = minZ - axialClearance（确保底部有余量）
    score.baseCenter = gp_Pnt(0, 0, 0).Translated(
        gp_Vec(localAxes.XDirection()) * mec.center.X() +
        gp_Vec(localAxes.YDirection()) * mec.center.Y() +
        gp_Vec(axis) * (minZ - options.axialClearance));

    // 圆柱几何中心（用于 Result）
    score.center = gp_Pnt(0, 0, 0).Translated(
        gp_Vec(localAxes.XDirection()) * mec.center.X() +
        gp_Vec(localAxes.YDirection()) * mec.center.Y() +
        gp_Vec(axis) * (0.5 * (minZ + maxZ)));

    return score;
}

// ---------------------------------------------------------------------------
//  选项验证
// ---------------------------------------------------------------------------
inline void ValidateOptions(const Options& options)
{
    if (options.sampleDensity <= 0.0)
    {
        throw std::invalid_argument(
            "BoundingCylinder: sampleDensity must be positive.");
    }

    if (options.minimumSampleCount < 8)
    {
        throw std::invalid_argument(
            "BoundingCylinder: minimumSampleCount must be at least 8.");
    }

    if (options.maximumSampleCount < options.minimumSampleCount)
    {
        throw std::invalid_argument(
            "BoundingCylinder: maximumSampleCount must be >= minimumSampleCount.");
    }

    if (!std::isfinite(options.radialClearance) ||
        options.radialClearance < 0.0)
    {
        throw std::invalid_argument(
            "BoundingCylinder: radialClearance must be finite and non-negative.");
    }

    if (!std::isfinite(options.axialClearance) ||
        options.axialClearance < 0.0)
    {
        throw std::invalid_argument(
            "BoundingCylinder: axialClearance must be finite and non-negative.");
    }

    if (!std::isfinite(options.minimumRadius) ||
        options.minimumRadius <= 0.0)
    {
        throw std::invalid_argument(
            "BoundingCylinder: minimumRadius must be finite and positive.");
    }

    if (!std::isfinite(options.minimumHeight) ||
        options.minimumHeight <= 0.0)
    {
        throw std::invalid_argument(
            "BoundingCylinder: minimumHeight must be finite and positive.");
    }
}

} // namespace detail

// ===========================================================================
//  公共 API
// ===========================================================================

/// @brief 计算 Shape 的紧密包围圆柱体。
///
/// @param shape  输入几何体（任意 TopoDS_Shape）。
/// @param options  可选配置参数。
///
/// @return Result 包含圆柱实体、半径、高度、轴方向等信息。
///
/// @throw std::invalid_argument  输入 shape 为空或选项无效。
/// @throw std::runtime_error  采样失败或圆柱构造失败。
inline Result Compute(
    const TopoDS_Shape& shape,
    const Options& options = {})
{
    using namespace detail;

    if (shape.IsNull())
    {
        throw std::invalid_argument(
            "BoundingCylinder: input shape is null.");
    }

    ValidateOptions(options);

    // -------------------------------------------------------------------
    //  1. 采样
    // -------------------------------------------------------------------
    std::vector<gp_Pnt> points = SamplePoints(shape, options);

    if (points.size() < 8)
    {
        throw std::runtime_error(
            "BoundingCylinder: too few sample points — "
            "the shape may be empty or degenerate.");
    }

    // -------------------------------------------------------------------
    //  2. 计算 PCA
    // -------------------------------------------------------------------
    const PcaResult pca = ComputePCA(points);

    // -------------------------------------------------------------------
    //  3. 计算 OBB（用于补充候选方向 + 调试参考）
    // -------------------------------------------------------------------
    Bnd_OBB obb;

    BRepBndLib::AddOBB(
        shape,
        obb,
        true,  // useTriangulation
        true,  // useOptimalObb
        true); // useShapeTolerance

    // -------------------------------------------------------------------
    //  4. 生成候选轴
    // -------------------------------------------------------------------
    std::vector<AxisCandidate> candidates =
        GenerateCandidates(pca, obb, options);

    // -------------------------------------------------------------------
    //  5. 评估每个候选轴，选择最优
    // -------------------------------------------------------------------
    CandidateScore best;
    best.volume = std::numeric_limits<double>::max();

    for (const AxisCandidate& cand : candidates)
    {
        CandidateScore score = EvaluateCandidate(points, cand, options);

        if (score.volume < best.volume)
        {
            best = score;
        }
    }

    if (best.volume >= std::numeric_limits<double>::max())
    {
        throw std::runtime_error(
            "BoundingCylinder: failed to evaluate any axis candidate.");
    }

    // -------------------------------------------------------------------
    //  6. 防守型校验：确保所有几何参数有效
    // -------------------------------------------------------------------
    const auto IsFinitePnt = [](const gp_Pnt& p) -> bool
    {
        return std::isfinite(p.X()) && std::isfinite(p.Y()) && std::isfinite(p.Z());
    };

    if (!IsFinitePnt(best.baseCenter))
    {
        throw std::runtime_error(
            "BoundingCylinder: baseCenter contains NaN or Inf.");
    }

    if (!IsFinitePnt(best.center))
    {
        throw std::runtime_error(
            "BoundingCylinder: center contains NaN or Inf.");
    }

    if (!std::isfinite(best.radius))
    {
        throw std::runtime_error(
            "BoundingCylinder: radius is not finite (NaN or Inf).");
    }

    if (!std::isfinite(best.height))
    {
        throw std::runtime_error(
            "BoundingCylinder: height is not finite (NaN or Inf).");
    }

    if (best.radius <= Precision::Confusion())
    {
        throw std::runtime_error(
            "BoundingCylinder: radius is too small (<= Precision::Confusion). "
            "Consider increasing minimumRadius in Options.");
    }

    if (best.height <= Precision::Confusion())
    {
        throw std::runtime_error(
            "BoundingCylinder: height is too small (<= Precision::Confusion). "
            "Consider increasing minimumHeight in Options.");
    }

    const double axisMag = gp_Vec(best.axis).Magnitude();

    if (axisMag < Precision::Confusion())
    {
        throw std::runtime_error(
            "BoundingCylinder: axis direction is degenerate (zero vector).");
    }

    // -------------------------------------------------------------------
    //  7. 构造圆柱实体
    //
    //  关键：使用 gp_Ax2 三参数构造函数，显式提供垂直于轴方向的
    //  XDirection，而非依赖两参数构造函数的自动计算。
    //
    //  两参数 gp_Ax2(P, D) 内部用 (1,0,0) 或 (0,0,1) 做参考方向
    //  来推导 XDirection。当轴方向恰好与参考方向平行时，自动计算
    //  会产生非确定性结果，极少数情况下可能导致 BRepPrim_Cylinder
    //  内部母线构造失败。
    // -------------------------------------------------------------------
    gp_Vec axisVec(best.axis);

    // 选取一个不平行于轴方向的参考方向
    gp_Vec refVec;

    if (std::abs(best.axis.Dot(gp_Dir(1.0, 0.0, 0.0))) < 0.9999)
    {
        refVec = gp_Vec(1.0, 0.0, 0.0);
    }
    else
    {
        refVec = gp_Vec(0.0, 1.0, 0.0);
    }

    // XDirection = axis × (ref × axis) → 保证垂直于 axis
    gp_Vec xDir = axisVec.Crossed(refVec);

    if (xDir.SquareMagnitude() < Precision::SquareConfusion())
    {
        throw std::runtime_error(
            "BoundingCylinder: failed to compute perpendicular XDirection "
            "for cylinder coordinate system.");
    }

    const gp_Ax2 cylinderAxes(
        best.baseCenter,
        best.axis,
        gp_Dir(xDir));

    BRepPrimAPI_MakeCylinder cylinderMaker(
        cylinderAxes,
        best.radius,
        best.height);

    if (!cylinderMaker.IsDone())
    {
        throw std::runtime_error(
            "BoundingCylinder: BRepPrimAPI_MakeCylinder::IsDone() "
            "returned false — internal revolution algorithm failed.");
    }

    // -------------------------------------------------------------------
    //  8. 返回结果
    // -------------------------------------------------------------------
    Result result;
    result.cylinder = cylinderMaker.Shape();
    result.center = best.center;
    result.baseCenter = best.baseCenter;
    result.axis = best.axis;
    result.radius = best.radius;
    result.height = best.height;
    result.axisSource = best.source;
    result.sampleCount = static_cast<int>(points.size());

    return result;
}

} // namespace BoundingCylinder
