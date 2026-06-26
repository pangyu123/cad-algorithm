#include "PointCloudToFace.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>

#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
struct ProjectedPoint
{
    double x = 0.0;
    double y = 0.0;
    int index = -1;
};

bool AreSamePoint(const gp_Pnt& a, const gp_Pnt& b, double tol)
{
    return a.SquareDistance(b) <= tol * tol;
}

std::vector<gp_Pnt> RemoveDuplicatePoints(
    const std::vector<gp_Pnt>& input,
    double tol)
{
    // 输入：原始点集。
    // 输出：去重后的点集。
    std::vector<gp_Pnt> result;

    for (const gp_Pnt& p : input)
    {
        bool duplicate = false;

        for (const gp_Pnt& q : result)
        {
            if (AreSamePoint(p, q, tol))
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
        {
            result.push_back(p);
        }
    }

    return result;
}

bool ComputeBestPlaneFromPoints(
    const std::vector<gp_Pnt>& points,
    gp_Pln& outPlane,
    double tol)
{
    // 输入：点集。
    // 输出：由任意三个非共线点确定的参考平面。
    // 说明：这里不做 PCA，只寻找第一个稳定的非共线三点平面。
    if (points.size() < 3)
    {
        return false;
    }

    const int n = static_cast<int>(points.size());

    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            gp_Vec v1(points[i], points[j]);

            if (v1.Magnitude() <= tol)
            {
                continue;
            }

            for (int k = j + 1; k < n; ++k)
            {
                gp_Vec v2(points[i], points[k]);
                gp_Vec normal = v1.Crossed(v2);

                if (normal.Magnitude() <= tol)
                {
                    continue;
                }

                normal.Normalize();
                outPlane = gp_Pln(points[i], gp_Dir(normal));
                return true;
            }
        }
    }

    return false;
}

bool ArePointsOnPlane(
    const std::vector<gp_Pnt>& points,
    const gp_Pln& plane,
    double tol)
{
    // 输入：点集和平面。
    // 输出：所有点是否在该平面附近。
    for (const gp_Pnt& p : points)
    {
        if (std::abs(plane.Distance(p)) > tol)
        {
            return false;
        }
    }

    return true;
}

double Cross2D(
    const ProjectedPoint& o,
    const ProjectedPoint& a,
    const ProjectedPoint& b)
{
    return (a.x - o.x) * (b.y - o.y) -
           (a.y - o.y) * (b.x - o.x);
}

std::vector<int> Build2DConvexHull(
    std::vector<ProjectedPoint> projectedPoints,
    double tol)
{
    // 输入：二维投影点。
    // 输出：凸包边界点对应的原始点索引，按边界顺序排列。
    std::sort(
        projectedPoints.begin(),
        projectedPoints.end(),
        [](const ProjectedPoint& a, const ProjectedPoint& b)
        {
            if (a.x != b.x)
            {
                return a.x < b.x;
            }

            return a.y < b.y;
        });

    std::vector<ProjectedPoint> unique;

    for (const ProjectedPoint& p : projectedPoints)
    {
        if (unique.empty())
        {
            unique.push_back(p);
            continue;
        }

        const ProjectedPoint& last = unique.back();

        if (std::abs(p.x - last.x) > tol ||
            std::abs(p.y - last.y) > tol)
        {
            unique.push_back(p);
        }
    }

    if (unique.size() < 3)
    {
        return {};
    }

    std::vector<ProjectedPoint> lower;

    for (const ProjectedPoint& p : unique)
    {
        while (lower.size() >= 2)
        {
            const ProjectedPoint& a = lower[lower.size() - 2];
            const ProjectedPoint& b = lower[lower.size() - 1];

            if (Cross2D(a, b, p) > tol)
            {
                break;
            }

            lower.pop_back();
        }

        lower.push_back(p);
    }

    std::vector<ProjectedPoint> upper;

    for (auto it = unique.rbegin(); it != unique.rend(); ++it)
    {
        const ProjectedPoint& p = *it;

        while (upper.size() >= 2)
        {
            const ProjectedPoint& a = upper[upper.size() - 2];
            const ProjectedPoint& b = upper[upper.size() - 1];

            if (Cross2D(a, b, p) > tol)
            {
                break;
            }

            upper.pop_back();
        }

        upper.push_back(p);
    }

    lower.pop_back();
    upper.pop_back();

    std::vector<int> hullIndices;

    for (const ProjectedPoint& p : lower)
    {
        hullIndices.push_back(p.index);
    }

    for (const ProjectedPoint& p : upper)
    {
        hullIndices.push_back(p.index);
    }

    return hullIndices;
}

bool BuildPlaneBasis(
    const gp_Pln& plane,
    gp_Vec& outU,
    gp_Vec& outV,
    double tol)
{
    // 输入：平面。
    // 输出：平面内两个正交方向 u/v。
    gp_Dir normalDir = plane.Axis().Direction();
    gp_Vec normal(normalDir);

    gp_Vec ref;

    if (std::abs(normalDir.Dot(gp_Dir(1.0, 0.0, 0.0))) < 0.9)
    {
        ref = gp_Vec(1.0, 0.0, 0.0);
    }
    else
    {
        ref = gp_Vec(0.0, 1.0, 0.0);
    }

    gp_Vec u = ref - normal * ref.Dot(normal);

    if (u.Magnitude() <= tol)
    {
        return false;
    }

    u.Normalize();

    gp_Vec v = normal.Crossed(u);

    if (v.Magnitude() <= tol)
    {
        return false;
    }

    v.Normalize();

    outU = u;
    outV = v;
    return true;
}

std::vector<gp_Pnt> OrderPointsByConvexHull(
    const std::vector<gp_Pnt>& points,
    const gp_Pln& plane,
    double tol)
{
    // 输入：无序共面点集。
    // 输出：按二维凸包顺序排列的边界点。
    gp_Vec u;
    gp_Vec v;

    if (!BuildPlaneBasis(plane, u, v, tol))
    {
        return {};
    }

    const gp_Pnt origin = plane.Location();
    std::vector<ProjectedPoint> projected;

    for (int i = 0; i < static_cast<int>(points.size()); ++i)
    {
        gp_Vec op(origin, points[static_cast<std::size_t>(i)]);

        ProjectedPoint pp;
        pp.x = op.Dot(u);
        pp.y = op.Dot(v);
        pp.index = i;

        projected.push_back(pp);
    }

    const std::vector<int> orderedIndices = Build2DConvexHull(projected, tol);

    if (orderedIndices.size() < 3)
    {
        return {};
    }

    std::vector<gp_Pnt> orderedPoints;

    for (int idx : orderedIndices)
    {
        orderedPoints.push_back(points[static_cast<std::size_t>(idx)]);
    }

    return orderedPoints;
}

TopoDS_Face MakeFaceFromOrderedBoundary(
    const std::vector<gp_Pnt>& orderedBoundary,
    const gp_Pln& plane)
{
    // 输入：有序闭合边界点和平面。
    // 输出：TopoDS_Face。
    if (orderedBoundary.size() < 3)
    {
        return TopoDS_Face();
    }

    BRepBuilderAPI_MakePolygon polygon;

    for (const gp_Pnt& p : orderedBoundary)
    {
        polygon.Add(p);
    }

    polygon.Close();

    if (!polygon.IsDone())
    {
        return TopoDS_Face();
    }

    BRepBuilderAPI_MakeFace faceMaker(
        plane,
        polygon.Wire(),
        true);

    if (!faceMaker.IsDone())
    {
        return TopoDS_Face();
    }

    return faceMaker.Face();
}

} // namespace

TopoDS_Face PointCloudToFace::BuildFace(
    const std::vector<gp_Pnt>& inputPoints,
    const PointCloudToFaceOptions& options)
{
    // 输入：点集。
    // 输出：TopoDS_Face。
    const double tol = options.tolerance;

    std::vector<gp_Pnt> points = RemoveDuplicatePoints(inputPoints, tol);

    if (points.size() < 3)
    {
        return TopoDS_Face();
    }

    gp_Pln plane;

    if (!ComputeBestPlaneFromPoints(points, plane, tol))
    {
        return TopoDS_Face();
    }

    if (!ArePointsOnPlane(points, plane, tol))
    {
        return TopoDS_Face();
    }

    std::vector<gp_Pnt> boundaryPoints;

    if (options.inputPointsAreOrderedBoundary)
    {
        boundaryPoints = points;
    }
    else
    {
        boundaryPoints = OrderPointsByConvexHull(points, plane, tol);
    }

    if (boundaryPoints.size() < 3)
    {
        return TopoDS_Face();
    }

    return MakeFaceFromOrderedBoundary(boundaryPoints, plane);
}

TopoDS_Face BuildFaceFromPoints(const std::vector<gp_Pnt>& points)
{
    // 简化接口。
    // 输入：无序共面点集。
    // 输出：共面凸包 TopoDS_Face。
    PointCloudToFaceOptions options;
    options.inputPointsAreOrderedBoundary = false;
    return PointCloudToFace::BuildFace(points, options);
}
