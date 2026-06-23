#include "PointCloudToClosedShape.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS.hxx>

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>

#include <gp_Pln.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
struct HullPlane
{
    gp_Dir normal;
    double d = 0.0;
    std::vector<int> pointIndices;
};

struct ProjectedPoint
{
    double x = 0.0;
    double y = 0.0;
    int index = -1;
};

double Dot(const gp_Dir& n, const gp_Pnt& p)
{
    return n.X() * p.X() + n.Y() * p.Y() + n.Z() * p.Z();
}

double SignedDistance(const gp_Dir& n, double d, const gp_Pnt& p)
{
    return Dot(n, p) + d;
}

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

bool AreSamePlane(const HullPlane& a, const HullPlane& b, double tol)
{
    const double dot = a.normal.Dot(b.normal);

    if (std::abs(dot - 1.0) > 1e-6)
    {
        return false;
    }

    if (std::abs(a.d - b.d) > tol)
    {
        return false;
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
    // 输入：同一个三维平面上的点投影后的二维坐标。
    // 输出：二维凸包顶点对应的原始点索引，按边界顺序排列。
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

std::vector<HullPlane> BuildHullPlanes(
    const std::vector<gp_Pnt>& points,
    double tol)
{
    // 输入：去重后的三维点集。
    // 输出：三维凸包的所有支撑平面。
    std::vector<HullPlane> planes;

    const int n = static_cast<int>(points.size());

    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            for (int k = j + 1; k < n; ++k)
            {
                gp_Vec v1(points[i], points[j]);
                gp_Vec v2(points[i], points[k]);
                gp_Vec normalVec = v1.Crossed(v2);

                if (normalVec.Magnitude() <= tol)
                {
                    continue;
                }

                normalVec.Normalize();
                gp_Dir normal(normalVec);
                double d = -Dot(normal, points[i]);

                bool hasPositive = false;
                bool hasNegative = false;

                for (const gp_Pnt& p : points)
                {
                    const double s = SignedDistance(normal, d, p);

                    if (s > tol)
                    {
                        hasPositive = true;
                    }
                    else if (s < -tol)
                    {
                        hasNegative = true;
                    }

                    if (hasPositive && hasNegative)
                    {
                        break;
                    }
                }

                // 如果平面两侧都有点，则不是凸包边界支撑平面。
                if (hasPositive && hasNegative)
                {
                    continue;
                }

                // 统一法向为外法向，使所有点位于平面内侧或负侧。
                if (!hasNegative && hasPositive)
                {
                    normal = gp_Dir(-normal.X(), -normal.Y(), -normal.Z());
                    d = -d;
                }

                HullPlane plane;
                plane.normal = normal;
                plane.d = d;

                bool duplicate = false;

                for (const HullPlane& existing : planes)
                {
                    if (AreSamePlane(plane, existing, tol))
                    {
                        duplicate = true;
                        break;
                    }
                }

                if (duplicate)
                {
                    continue;
                }

                for (int idx = 0; idx < n; ++idx)
                {
                    const double s = SignedDistance(plane.normal, plane.d, points[idx]);

                    if (std::abs(s) <= tol)
                    {
                        plane.pointIndices.push_back(idx);
                    }
                }

                if (plane.pointIndices.size() >= 3)
                {
                    planes.push_back(plane);
                }
            }
        }
    }

    return planes;
}

TopoDS_Shape MakeFaceFromHullPlane(
    const std::vector<gp_Pnt>& points,
    const HullPlane& plane,
    double tol)
{
    // 输入：凸包的一个支撑平面及该平面上的点。
    // 输出：这个支撑平面对应的多边形 Face。
    gp_Vec nVec(plane.normal);

    gp_Pnt center(0.0, 0.0, 0.0);

    for (int idx : plane.pointIndices)
    {
        center.ChangeCoord() += points[idx].XYZ();
    }

    center.ChangeCoord() /= static_cast<double>(plane.pointIndices.size());

    gp_Vec refVec;

    if (std::abs(plane.normal.Dot(gp_Dir(1.0, 0.0, 0.0))) < 0.9)
    {
        refVec = gp_Vec(1.0, 0.0, 0.0);
    }
    else
    {
        refVec = gp_Vec(0.0, 1.0, 0.0);
    }

    gp_Vec uVec = refVec - nVec * refVec.Dot(nVec);

    if (uVec.Magnitude() <= tol)
    {
        return TopoDS_Shape();
    }

    uVec.Normalize();

    gp_Vec vVec = nVec.Crossed(uVec);

    if (vVec.Magnitude() <= tol)
    {
        return TopoDS_Shape();
    }

    vVec.Normalize();

    std::vector<ProjectedPoint> projected;

    for (int idx : plane.pointIndices)
    {
        gp_Vec cp(center, points[idx]);

        ProjectedPoint pp;
        pp.x = cp.Dot(uVec);
        pp.y = cp.Dot(vVec);
        pp.index = idx;

        projected.push_back(pp);
    }

    const std::vector<int> orderedIndices = Build2DConvexHull(projected, tol);

    if (orderedIndices.size() < 3)
    {
        return TopoDS_Shape();
    }

    BRepBuilderAPI_MakePolygon polygon;

    for (int idx : orderedIndices)
    {
        polygon.Add(points[idx]);
    }

    polygon.Close();

    if (!polygon.IsDone())
    {
        return TopoDS_Shape();
    }

    gp_Pln occPlane(center, plane.normal);

    BRepBuilderAPI_MakeFace faceMaker(
        occPlane,
        polygon.Wire(),
        true);

    if (!faceMaker.IsDone())
    {
        return TopoDS_Shape();
    }

    return faceMaker.Face();
}

TopoDS_Shape SewFacesToShellOrSolid(
    const std::vector<TopoDS_Shape>& faces,
    bool makeSolid,
    double tol)
{
    // 输入：凸包面片集合。
    // 输出：缝合后的 Shell 或 Solid。
    if (faces.empty())
    {
        return TopoDS_Shape();
    }

    BRepBuilderAPI_Sewing sewing(tol);

    for (const TopoDS_Shape& face : faces)
    {
        sewing.Add(face);
    }

    sewing.Perform();

    TopoDS_Shape sewedShape = sewing.SewedShape();

    if (sewedShape.IsNull())
    {
        return TopoDS_Shape();
    }

    if (!makeSolid)
    {
        return sewedShape;
    }

    TopoDS_Shell shell;

    if (sewedShape.ShapeType() == TopAbs_SHELL)
    {
        shell = TopoDS::Shell(sewedShape);
    }
    else
    {
        for (TopExp_Explorer exp(sewedShape, TopAbs_SHELL); exp.More(); exp.Next())
        {
            shell = TopoDS::Shell(exp.Current());
            break;
        }
    }

    if (shell.IsNull())
    {
        return sewedShape;
    }

    BRepBuilderAPI_MakeSolid solidMaker;
    solidMaker.Add(shell);

    if (!solidMaker.IsDone())
    {
        return sewedShape;
    }

    TopoDS_Shape solid = solidMaker.Solid();
    BRepCheck_Analyzer analyzer(solid);

    if (!analyzer.IsValid())
    {
        return sewedShape;
    }

    return solid;
}

} // namespace

TopoDS_Shape PointCloudToClosedShape::BuildConvexClosedShape(
    const std::vector<gp_Pnt>& inputPoints,
    const PointCloudToClosedShapeOptions& options)
{
    // 输入：无序三维点集。
    // 输出：由点集凸包生成的闭合 TopoDS_Shape，优先为 Solid。
    const double tol = options.tolerance;

    if (inputPoints.size() < 4)
    {
        return TopoDS_Shape();
    }

    std::vector<gp_Pnt> points = RemoveDuplicatePoints(inputPoints, tol);

    if (points.size() < 4)
    {
        return TopoDS_Shape();
    }

    std::vector<HullPlane> hullPlanes = BuildHullPlanes(points, tol);

    if (hullPlanes.size() < 4)
    {
        return TopoDS_Shape();
    }

    std::vector<TopoDS_Shape> faces;

    for (const HullPlane& plane : hullPlanes)
    {
        TopoDS_Shape face = MakeFaceFromHullPlane(points, plane, tol);

        if (!face.IsNull())
        {
            faces.push_back(face);
        }
    }

    if (faces.empty())
    {
        return TopoDS_Shape();
    }

    return SewFacesToShellOrSolid(
        faces,
        options.makeSolid,
        tol);
}

TopoDS_Shape BuildClosedShapeFromPoints(const std::vector<gp_Pnt>& points)
{
    // 简化接口。
    // 输入：无序三维点集。
    // 输出：闭合凸包模型。
    PointCloudToClosedShapeOptions options;
    return PointCloudToClosedShape::BuildConvexClosedShape(points, options);
}
