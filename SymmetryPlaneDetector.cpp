#include "SymmetryPlaneDetector.h"

#include <TopoDS_Face.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBndLib.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>

#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>

#include <TopLoc_Location.hxx>

#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <gp_Cylinder.hxx>

#include <Bnd_Box.hxx>
#include <GeomAbs_SurfaceType.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace
{
constexpr double kPi = 3.141592653589793238462643383279502884;

struct PlaneCandidate
{
    gp_Pln plane;
};

std::vector<gp_Pnt> SampleShapePoints(
    const TopoDS_Shape& inputShape,
    double meshDeflection,
    std::size_t maxSamples)
{
    // 输入：TopoDS_Shape
    // 输出：从 shape 三角网格中采样出的表面点云。
    BRepMesh_IncrementalMesh mesher(inputShape, meshDeflection);

    std::vector<gp_Pnt> points;

    for (TopExp_Explorer exp(inputShape, TopAbs_FACE); exp.More(); exp.Next())
    {
        TopoDS_Face face = TopoDS::Face(exp.Current());

        TopLoc_Location loc;
        Handle(Poly_Triangulation) triangulation =
            BRep_Tool::Triangulation(face, loc);

        if (triangulation.IsNull())
        {
            continue;
        }

        gp_Trsf trsf = loc.Transformation();
        const int nbTriangles = triangulation->NbTriangles();

        for (int i = 1; i <= nbTriangles; ++i)
        {
            Poly_Triangle tri = triangulation->Triangle(i);

            int n1 = 0;
            int n2 = 0;
            int n3 = 0;
            tri.Get(n1, n2, n3);

            gp_Pnt p1 = triangulation->Node(n1);
            gp_Pnt p2 = triangulation->Node(n2);
            gp_Pnt p3 = triangulation->Node(n3);

            p1.Transform(trsf);
            p2.Transform(trsf);
            p3.Transform(trsf);

            points.push_back(p1);
            points.push_back(p2);
            points.push_back(p3);

            // 额外加入三角形中心点，使大面片也能参与误差评价。
            gp_Pnt center(
                (p1.X() + p2.X() + p3.X()) / 3.0,
                (p1.Y() + p2.Y() + p3.Y()) / 3.0,
                (p1.Z() + p2.Z() + p3.Z()) / 3.0);

            points.push_back(center);
        }
    }

    if (points.size() > maxSamples)
    {
        std::mt19937 rng(12345);
        std::shuffle(points.begin(), points.end(), rng);
        points.resize(maxSamples);
    }

    return points;
}

double BoundingBoxDiagonal(const TopoDS_Shape& inputShape)
{
    // 输入：TopoDS_Shape
    // 输出：包围盒对角线长度，用于误差归一化。
    Bnd_Box box;
    BRepBndLib::Add(inputShape, box);

    if (box.IsVoid())
    {
        return 0.0;
    }

    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;

    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    const double dx = xmax - xmin;
    const double dy = ymax - ymin;
    const double dz = zmax - zmin;

    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

gp_Pnt BoundingBoxCenter(const TopoDS_Shape& inputShape)
{
    // 输入：TopoDS_Shape
    // 输出：AABB 包围盒中心点。
    Bnd_Box box;
    BRepBndLib::Add(inputShape, box);

    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;

    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    return gp_Pnt(
        0.5 * (xmin + xmax),
        0.5 * (ymin + ymax),
        0.5 * (zmin + zmax));
}

void AddBoundingBoxCenterPlanes(
    const TopoDS_Shape& inputShape,
    std::vector<PlaneCandidate>& candidates)
{
    // 输入：TopoDS_Shape
    // 输出：经过包围盒中心、法向为全局 X/Y/Z 的候选平面。
    const gp_Pnt c = BoundingBoxCenter(inputShape);

    candidates.push_back({ gp_Pln(c, gp_Dir(1.0, 0.0, 0.0)) });
    candidates.push_back({ gp_Pln(c, gp_Dir(0.0, 1.0, 0.0)) });
    candidates.push_back({ gp_Pln(c, gp_Dir(0.0, 0.0, 1.0)) });
}

void AddPlanesThroughAxis(
    const gp_Ax1& axis,
    int count,
    std::vector<PlaneCandidate>& candidates)
{
    // 输入：一条轴线。
    // 输出：一组包含该轴线的候选平面。
    if (count <= 0)
    {
        return;
    }

    const gp_Pnt origin = axis.Location();
    const gp_Dir axisDir = axis.Direction();
    const gp_Vec axisVec(axisDir);

    gp_Vec refVec;

    if (std::abs(axisDir.Dot(gp_Dir(1.0, 0.0, 0.0))) < 0.9)
    {
        refVec = gp_Vec(1.0, 0.0, 0.0);
    }
    else
    {
        refVec = gp_Vec(0.0, 1.0, 0.0);
    }

    // 将参考方向投影到垂直于 axisVec 的平面上。
    refVec = refVec - axisVec * refVec.Dot(axisVec);

    if (refVec.Magnitude() <= 1e-12)
    {
        return;
    }

    refVec.Normalize();

    for (int i = 0; i < count; ++i)
    {
        const double angle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(count);

        gp_Trsf rot;
        rot.SetRotation(axis, angle);

        gp_Vec dirVec = refVec;
        dirVec.Transform(rot);

        // 候选平面包含 axisVec 和 dirVec，平面法向为二者叉乘。
        gp_Vec normalVec = axisVec.Crossed(dirVec);

        if (normalVec.Magnitude() <= 1e-12)
        {
            continue;
        }

        normalVec.Normalize();
        candidates.push_back({ gp_Pln(origin, gp_Dir(normalVec)) });
    }
}

void AddCylinderAxisPlanes(
    const TopoDS_Shape& inputShape,
    int angularCandidateCount,
    std::vector<PlaneCandidate>& candidates)
{
    // 输入：TopoDS_Shape
    // 输出：从所有圆柱面轴线生成的候选平面。
    for (TopExp_Explorer exp(inputShape, TopAbs_FACE); exp.More(); exp.Next())
    {
        TopoDS_Face face = TopoDS::Face(exp.Current());
        BRepAdaptor_Surface surface(face);

        if (surface.GetType() != GeomAbs_Cylinder)
        {
            continue;
        }

        gp_Cylinder cylinder = surface.Cylinder();
        gp_Ax1 axis = cylinder.Axis();

        AddPlanesThroughAxis(axis, angularCandidateCount, candidates);
    }
}

void AddPlanarFacePairMidPlanes(
    const TopoDS_Shape& inputShape,
    std::vector<PlaneCandidate>& candidates)
{
    // 输入：TopoDS_Shape
    // 输出：由平行平面 Face 对生成的中间候选平面。
    struct PlaneInfo
    {
        gp_Dir normal;
        double d = 0.0;
    };

    std::vector<PlaneInfo> planes;

    for (TopExp_Explorer exp(inputShape, TopAbs_FACE); exp.More(); exp.Next())
    {
        TopoDS_Face face = TopoDS::Face(exp.Current());
        BRepAdaptor_Surface surface(face);

        if (surface.GetType() != GeomAbs_Plane)
        {
            continue;
        }

        gp_Pln pln = surface.Plane();
        gp_Dir n = pln.Axis().Direction();
        gp_Pnt p = pln.Location();

        // 平面方程：n.X*x + n.Y*y + n.Z*z + d = 0
        const double d = -(n.X() * p.X() + n.Y() * p.Y() + n.Z() * p.Z());

        planes.push_back({ n, d });
    }

    const double parallelTolerance = 1e-4;

    for (std::size_t i = 0; i < planes.size(); ++i)
    {
        for (std::size_t j = i + 1; j < planes.size(); ++j)
        {
            const gp_Dir n1 = planes[i].normal;
            const gp_Dir n2 = planes[j].normal;
            const double dot = n1.Dot(n2);

            // 法向需要平行或反平行。
            if (std::abs(std::abs(dot) - 1.0) > parallelTolerance)
            {
                continue;
            }

            gp_Dir n = n1;
            const double d1 = planes[i].d;
            double d2 = planes[j].d;

            // 将第二个平面的方程方向统一到 n1。
            if (dot < 0.0)
            {
                d2 = -d2;
            }

            const double midD = 0.5 * (d1 + d2);

            // n · x + midD = 0 上的一个点可以取 -midD * n。
            gp_Pnt pointOnMidPlane(
                -midD * n.X(),
                -midD * n.Y(),
                -midD * n.Z());

            candidates.push_back({ gp_Pln(pointOnMidPlane, n) });
        }
    }
}

gp_Pnt ReflectPointAboutPlane(
    const gp_Pnt& p,
    const gp_Pln& plane)
{
    // 输入：点 p 和镜像平面 plane。
    // 输出：p 关于 plane 的镜像点。
    const gp_Pnt planePoint = plane.Location();
    const gp_Dir planeNormal = plane.Axis().Direction();
    const gp_Vec n(planeNormal);

    const gp_Vec planePointToP(planePoint, p);
    const double signedDistance = planePointToP.Dot(n);

    return gp_Pnt(
        p.X() - 2.0 * signedDistance * n.X(),
        p.Y() - 2.0 * signedDistance * n.Y(),
        p.Z() - 2.0 * signedDistance * n.Z());
}

double NearestPointSquaredDistance(
    const std::vector<gp_Pnt>& points,
    const gp_Pnt& query)
{
    // 输入：点云和查询点。
    // 输出：查询点到点云最近点的平方距离。
    double best = std::numeric_limits<double>::max();

    for (const gp_Pnt& p : points)
    {
        const double d2 = p.SquareDistance(query);
        if (d2 < best)
        {
            best = d2;
        }
    }

    return best;
}

double ComputeMirrorRMSError(
    const std::vector<gp_Pnt>& points,
    const gp_Pln& plane)
{
    // 输入：表面点云和候选平面。
    // 输出：点云关于候选平面镜像后的 RMS 最近邻误差。
    if (points.empty())
    {
        return std::numeric_limits<double>::max();
    }

    double sum = 0.0;

    for (const gp_Pnt& p : points)
    {
        const gp_Pnt reflected = ReflectPointAboutPlane(p, plane);
        const double d2 = NearestPointSquaredDistance(points, reflected);
        sum += d2;
    }

    const double mean = sum / static_cast<double>(points.size());
    return std::sqrt(mean);
}

bool ArePlanesSimilar(
    const gp_Pln& a,
    const gp_Pln& b,
    double distanceTolerance)
{
    // 输入：两个平面。
    // 输出：它们是否表示几乎相同的平面。
    const gp_Dir na = a.Axis().Direction();
    const gp_Dir nb = b.Axis().Direction();

    if (std::abs(na.Dot(nb)) < 0.999)
    {
        return false;
    }

    const gp_Pnt pa = a.Location();
    const gp_Pnt pb = b.Location();
    const gp_Vec normalVec(na);
    const gp_Vec paToPb(pa, pb);

    const double dist = std::abs(paToPb.Dot(normalVec));
    return dist < distanceTolerance;
}

void RemoveDuplicatePlanes(
    std::vector<PlaneCandidate>& candidates,
    double distanceTolerance)
{
    // 输入：候选平面列表。
    // 输出：去重后的候选平面列表。
    std::vector<PlaneCandidate> uniqueCandidates;

    for (const PlaneCandidate& c : candidates)
    {
        bool duplicate = false;

        for (const PlaneCandidate& u : uniqueCandidates)
        {
            if (ArePlanesSimilar(c.plane, u.plane, distanceTolerance))
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
        {
            uniqueCandidates.push_back(c);
        }
    }

    candidates.swap(uniqueCandidates);
}

TopoDS_Shape MakePlaneShape(
    const gp_Pln& plane,
    double halfSize)
{
    // 输入：数学平面 gp_Pln 和半尺寸。
    // 输出：有限矩形平面 Face，以 TopoDS_Shape 返回。
    TopoDS_Face face = BRepBuilderAPI_MakeFace(
        plane,
        -halfSize,
        halfSize,
        -halfSize,
        halfSize);

    return face;
}

} // namespace

std::vector<SymmetryPlaneShapeResult> SymmetryPlaneDetector::Detect(
    const TopoDS_Shape& inputShape,
    const SymmetryDetectionOptions& options)
{
    // 输入：
    //   inputShape: 待检测的 OCCT 模型。
    //   options: 检测参数。
    // 输出：
    //   候选对称平面列表，每个结果包含 TopoDS_Shape 平面、gp_Pln、误差和质量等级。
    std::vector<SymmetryPlaneShapeResult> results;

    if (inputShape.IsNull())
    {
        return results;
    }

    const double diag = BoundingBoxDiagonal(inputShape);
    if (diag <= 1e-12)
    {
        return results;
    }

    const std::vector<gp_Pnt> points = SampleShapePoints(
        inputShape,
        options.meshDeflection,
        options.maxSamples);

    if (points.size() < 10)
    {
        return results;
    }

    std::vector<PlaneCandidate> candidates;

    if (options.useBoundingBoxCenterPlanes)
    {
        AddBoundingBoxCenterPlanes(inputShape, candidates);
    }

    if (options.useCylinderAxisPlanes)
    {
        AddCylinderAxisPlanes(
            inputShape,
            options.axisPlaneCandidateCount,
            candidates);
    }

    if (options.usePlanarFacePairMidPlanes)
    {
        AddPlanarFacePairMidPlanes(inputShape, candidates);
    }

    RemoveDuplicatePlanes(candidates, diag * 1e-5);

    for (const PlaneCandidate& candidate : candidates)
    {
        const double rms = ComputeMirrorRMSError(points, candidate.plane);
        const double normalized = rms / diag;

        SymmetryPlaneShapeResult result;
        result.plane = candidate.plane;
        result.rmsError = rms;
        result.normalizedError = normalized;

        if (normalized <= options.strictTolerance)
        {
            result.quality = SymmetryQuality::Strict;
        }
        else if (normalized <= options.approximateTolerance)
        {
            result.quality = SymmetryQuality::Approximate;
        }
        else
        {
            result.quality = SymmetryQuality::Rejected;
        }

        const double planeHalfSize = diag * options.planeSizeScale;
        result.planeShape = MakePlaneShape(candidate.plane, planeHalfSize);

        results.push_back(result);
    }

    std::sort(
        results.begin(),
        results.end(),
        [](const SymmetryPlaneShapeResult& a, const SymmetryPlaneShapeResult& b)
        {
            return a.normalizedError < b.normalizedError;
        });

    return results;
}

TopoDS_Shape SymmetryPlaneDetector::DetectBestPlaneShape(
    const TopoDS_Shape& inputShape,
    const SymmetryDetectionOptions& options)
{
    // 输入：TopoDS_Shape
    // 输出：最优且可接受的 TopoDS_Shape 平面；失败时返回空 Shape。
    const std::vector<SymmetryPlaneShapeResult> results = Detect(inputShape, options);

    for (const SymmetryPlaneShapeResult& result : results)
    {
        if (result.quality == SymmetryQuality::Strict ||
            result.quality == SymmetryQuality::Approximate)
        {
            return result.planeShape;
        }
    }

    return TopoDS_Shape();
}

TopoDS_Shape FindSymmetryPlaneShape(const TopoDS_Shape& inputShape)
{
    // 简化接口。
    // 输入：任意 TopoDS_Shape。
    // 输出：最优对称平面 Face；未找到时返回空 Shape。
    SymmetryDetectionOptions options;
    return SymmetryPlaneDetector::DetectBestPlaneShape(inputShape, options);
}
