#include "ZoneCutOffsetEdge.h"

#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include <TopAbs_State.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>

#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepClass_FaceClassifier.hxx>

#include <Geom2d_Curve.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomAPI_PointsToBSpline.hxx>

#include <TColgp_Array1OfPnt.hxx>

#include <TopLoc_Location.hxx>

#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <gp_Vec2d.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace
{
struct EdgeOnFaceOffsetResult
{
    TopoDS_Edge sourceEdge;
    TopoDS_Edge offsetEdge;
    bool success = false;
};

std::vector<TopoDS_Face> FindAdjacentFaces(
    const TopoDS_Shape& solidShape,
    const TopoDS_Edge& inputEdge)
{
    // 输入：实体/壳/复合模型 + 一条边。
    // 输出：该边在模型中的所有邻接面。
    std::vector<TopoDS_Face> faces;

    TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
    TopExp::MapShapesAndAncestors(
        solidShape,
        TopAbs_EDGE,
        TopAbs_FACE,
        edgeFaceMap);

    for (int i = 1; i <= edgeFaceMap.Extent(); ++i)
    {
        const TopoDS_Shape& mappedEdge = edgeFaceMap.FindKey(i);

        if (!mappedEdge.IsSame(inputEdge))
        {
            continue;
        }

        const TopTools_ListOfShape& faceList = edgeFaceMap.FindFromIndex(i);

        for (TopTools_ListIteratorOfListOfShape it(faceList); it.More(); it.Next())
        {
            faces.push_back(TopoDS::Face(it.Value()));
        }

        break;
    }

    return faces;
}

bool ContainsSameFace(
    const std::vector<TopoDS_Face>& faces,
    const TopoDS_Face& face)
{
    for (const TopoDS_Face& f : faces)
    {
        if (f.IsSame(face))
        {
            return true;
        }
    }

    return false;
}

bool IsPointInsideFaceUV(
    const TopoDS_Face& face,
    const gp_Pnt2d& uv,
    double tolerance)
{
    BRepClass_FaceClassifier classifier;
    classifier.Perform(face, uv, tolerance);

    const TopAbs_State state = classifier.State();
    return state == TopAbs_IN || state == TopAbs_ON;
}

bool ProjectPointToFace(
    const TopoDS_Face& face,
    const gp_Pnt& worldPoint,
    gp_Pnt2d& outUV,
    gp_Pnt& outPointOnFace,
    double tolerance,
    bool requireInsideFace)
{
    // 输入：三维点 + 目标 Face。
    // 输出：投影到 Face underlying surface 后的 UV 与三维点。
    TopLoc_Location loc;
    Handle(Geom_Surface) surface = BRep_Tool::Surface(face, loc);

    if (surface.IsNull())
    {
        return false;
    }

    gp_Pnt localPoint = worldPoint;

    if (!loc.IsIdentity())
    {
        gp_Trsf inv = loc.Transformation().Inverted();
        localPoint.Transform(inv);
    }

    GeomAPI_ProjectPointOnSurf projector(localPoint, surface);

    if (!projector.IsDone() || projector.NbPoints() < 1)
    {
        return false;
    }

    Standard_Real u = 0.0;
    Standard_Real v = 0.0;
    projector.LowerDistanceParameters(u, v);

    outUV = gp_Pnt2d(u, v);

    if (requireInsideFace && !IsPointInsideFaceUV(face, outUV, tolerance))
    {
        return false;
    }

    BRepAdaptor_Surface adaptor(face);
    outPointOnFace = adaptor.Value(u, v);

    return true;
}

bool EvaluateOffsetDirectionOnFace(
    const TopoDS_Face& face,
    const Handle(Geom2d_Curve)& pcurve,
    double parameter,
    gp_Pnt& outPoint,
    gp_Vec& outDirection,
    double tolerance)
{
    // 输入：Face 上输入边的 pcurve 参数。
    // 输出：三维点，以及该点处“位于面内且垂直于边”的偏移方向。
    if (pcurve.IsNull())
    {
        return false;
    }

    gp_Pnt2d uv;
    gp_Vec2d duv;
    pcurve->D1(parameter, uv, duv);

    if (duv.Magnitude() <= tolerance)
    {
        return false;
    }

    BRepAdaptor_Surface adaptor(face);

    gp_Pnt p;
    gp_Vec su;
    gp_Vec sv;
    adaptor.D1(uv.X(), uv.Y(), p, su, sv);

    gp_Vec normal = su.Crossed(sv);

    if (normal.Magnitude() <= tolerance)
    {
        return false;
    }

    normal.Normalize();

    gp_Vec tangent = su * duv.X() + sv * duv.Y();

    if (tangent.Magnitude() <= tolerance)
    {
        return false;
    }

    tangent.Normalize();

    gp_Vec offsetDir = normal.Crossed(tangent);

    if (offsetDir.Magnitude() <= tolerance)
    {
        return false;
    }

    offsetDir.Normalize();

    outPoint = p;
    outDirection = offsetDir;
    return true;
}

int ChooseInsideOffsetSide(
    const TopoDS_Face& face,
    const Handle(Geom2d_Curve)& pcurve,
    double first,
    double last,
    double offsetDistance,
    const ZoneCutOffsetOptions& options)
{
    // 输入：边在 face 上的 pcurve。
    // 输出：+1 或 -1，优先选择偏移后仍在 trimmed face 内部的一侧。
    const double mid = 0.5 * (first + last);

    gp_Pnt p;
    gp_Vec dir;

    if (!EvaluateOffsetDirectionOnFace(
            face,
            pcurve,
            mid,
            p,
            dir,
            options.tolerance))
    {
        return 1;
    }

    gp_Pnt2d uvPlus;
    gp_Pnt projectedPlus;
    gp_Pnt plusPoint(
        p.X() + offsetDistance * dir.X(),
        p.Y() + offsetDistance * dir.Y(),
        p.Z() + offsetDistance * dir.Z());

    const bool plusOk = ProjectPointToFace(
        face,
        plusPoint,
        uvPlus,
        projectedPlus,
        options.tolerance,
        options.requireInsideFace);

    if (plusOk)
    {
        return 1;
    }

    gp_Pnt2d uvMinus;
    gp_Pnt projectedMinus;
    gp_Pnt minusPoint(
        p.X() - offsetDistance * dir.X(),
        p.Y() - offsetDistance * dir.Y(),
        p.Z() - offsetDistance * dir.Z());

    const bool minusOk = ProjectPointToFace(
        face,
        minusPoint,
        uvMinus,
        projectedMinus,
        options.tolerance,
        options.requireInsideFace);

    if (minusOk)
    {
        return -1;
    }

    return 1;
}

TopoDS_Edge BuildEdgeFromPoints(const std::vector<gp_Pnt>& points)
{
    // 输入：偏移采样点。
    // 输出：通过这些点拟合出的 TopoDS_Edge。
    if (points.size() < 2)
    {
        return TopoDS_Edge();
    }

    if (points.size() == 2)
    {
        BRepBuilderAPI_MakeEdge edgeMaker(points[0], points[1]);

        if (!edgeMaker.IsDone())
        {
            return TopoDS_Edge();
        }

        return edgeMaker.Edge();
    }

    TColgp_Array1OfPnt array(1, static_cast<Standard_Integer>(points.size()));

    for (Standard_Integer i = 1; i <= array.Upper(); ++i)
    {
        array.SetValue(i, points[static_cast<std::size_t>(i - 1)]);
    }

    GeomAPI_PointsToBSpline bsplineBuilder(array);

    if (!bsplineBuilder.IsDone())
    {
        return TopoDS_Edge();
    }

    Handle(Geom_Curve) curve = bsplineBuilder.Curve();
    BRepBuilderAPI_MakeEdge edgeMaker(curve);

    if (!edgeMaker.IsDone())
    {
        return TopoDS_Edge();
    }

    return edgeMaker.Edge();
}

EdgeOnFaceOffsetResult GenerateOffsetEdgeOnFace(
    const TopoDS_Edge& inputEdge,
    const TopoDS_Face& face,
    double offsetDistance,
    const ZoneCutOffsetOptions& options)
{
    // 输入：一条边 + 它的某个邻接面。
    // 输出：在该面上生成的一条偏移边。
    EdgeOnFaceOffsetResult result;
    result.sourceEdge = inputEdge;
    result.success = false;

    Standard_Real first = 0.0;
    Standard_Real last = 0.0;

    Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(
        inputEdge,
        face,
        first,
        last);

    if (pcurve.IsNull())
    {
        return result;
    }

    const int sampleCount = std::max(2, options.sampleCount);

    const int side = ChooseInsideOffsetSide(
        face,
        pcurve,
        first,
        last,
        offsetDistance,
        options);

    std::vector<gp_Pnt> offsetPoints;
    offsetPoints.reserve(static_cast<std::size_t>(sampleCount));

    for (int i = 0; i < sampleCount; ++i)
    {
        double ratio = 0.0;

        if (sampleCount > 1)
        {
            ratio = static_cast<double>(i) / static_cast<double>(sampleCount - 1);
        }

        const double parameter = first + ratio * (last - first);

        gp_Pnt p;
        gp_Vec dir;

        if (!EvaluateOffsetDirectionOnFace(
                face,
                pcurve,
                parameter,
                p,
                dir,
                options.tolerance))
        {
            continue;
        }

        gp_Pnt trialPoint(
            p.X() + side * offsetDistance * dir.X(),
            p.Y() + side * offsetDistance * dir.Y(),
            p.Z() + side * offsetDistance * dir.Z());

        gp_Pnt2d projectedUV;
        gp_Pnt projectedPoint;

        const bool projected = ProjectPointToFace(
            face,
            trialPoint,
            projectedUV,
            projectedPoint,
            options.tolerance,
            options.requireInsideFace);

        if (!projected)
        {
            continue;
        }

        offsetPoints.push_back(projectedPoint);
    }

    if (offsetPoints.size() < 2)
    {
        return result;
    }

    TopoDS_Edge offsetEdge = BuildEdgeFromPoints(offsetPoints);

    if (offsetEdge.IsNull())
    {
        return result;
    }

    result.offsetEdge = offsetEdge;
    result.success = true;

    return result;
}

TopoDS_Shape BuildCompoundFromEdges(const std::vector<TopoDS_Edge>& edges)
{
    // 输入：多条 offset edge。
    // 输出：一条 edge 或 compound。
    if (edges.empty())
    {
        return TopoDS_Shape();
    }

    if (edges.size() == 1)
    {
        return edges.front();
    }

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);

    for (const TopoDS_Edge& edge : edges)
    {
        if (!edge.IsNull())
        {
            builder.Add(compound, edge);
        }
    }

    return compound;
}

std::vector<TopoDS_Edge> RemoveNullAndDuplicateEdges(
    const std::vector<TopoDS_Edge>& inputEdges)
{
    std::vector<TopoDS_Edge> edges;

    for (const TopoDS_Edge& edge : inputEdges)
    {
        if (edge.IsNull())
        {
            continue;
        }

        bool duplicate = false;

        for (const TopoDS_Edge& existing : edges)
        {
            if (existing.IsSame(edge))
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
        {
            edges.push_back(edge);
        }
    }

    return edges;
}

} // namespace

std::vector<ZoneCutOffsetResult>
ZoneCutOffsetEdge::GenerateOffsetEdgesOnAdjacentFaces(
    const TopoDS_Shape& solidShape,
    const TopoDS_Edge& inputEdge,
    double offsetDistance,
    const ZoneCutOffsetOptions& options)
{
    // 单条边版本：内部转成多边版本，保证行为一致。
    std::vector<TopoDS_Edge> inputEdges;
    inputEdges.push_back(inputEdge);

    return GenerateOffsetEdgesOnAdjacentFaces(
        solidShape,
        inputEdges,
        offsetDistance,
        options);
}

std::vector<ZoneCutOffsetResult>
ZoneCutOffsetEdge::GenerateOffsetEdgesOnAdjacentFaces(
    const TopoDS_Shape& solidShape,
    const std::vector<TopoDS_Edge>& inputEdges,
    double offsetDistance,
    const ZoneCutOffsetOptions& options)
{
    // 输入：实体/壳/复合模型 + 多条相邻边 + 偏移距离。
    // 输出：按邻接面分组的偏移结果。
    std::vector<ZoneCutOffsetResult> results;

    if (solidShape.IsNull())
    {
        return results;
    }

    if (std::abs(offsetDistance) <= options.tolerance)
    {
        return results;
    }

    const std::vector<TopoDS_Edge> edges = RemoveNullAndDuplicateEdges(inputEdges);

    if (edges.empty())
    {
        return results;
    }

    std::vector<TopoDS_Face> allAdjacentFaces;

    for (const TopoDS_Edge& edge : edges)
    {
        const std::vector<TopoDS_Face> edgeFaces = FindAdjacentFaces(solidShape, edge);

        for (const TopoDS_Face& face : edgeFaces)
        {
            if (!ContainsSameFace(allAdjacentFaces, face))
            {
                allAdjacentFaces.push_back(face);
            }
        }
    }

    for (const TopoDS_Face& face : allAdjacentFaces)
    {
        ZoneCutOffsetResult faceResult;
        faceResult.sourceFace = face;
        faceResult.success = false;

        for (const TopoDS_Edge& edge : edges)
        {
            // 只有当该 edge 确实位于当前 face 上时，CurveOnSurface 才会有效。
            EdgeOnFaceOffsetResult edgeResult = GenerateOffsetEdgeOnFace(
                edge,
                face,
                offsetDistance,
                options);

            if (!edgeResult.success)
            {
                continue;
            }

            faceResult.sourceEdges.push_back(edgeResult.sourceEdge);
            faceResult.offsetEdges.push_back(edgeResult.offsetEdge);
        }

        if (!faceResult.offsetEdges.empty())
        {
            faceResult.success = true;

            if (options.buildCompoundShape)
            {
                faceResult.offsetShape = BuildCompoundFromEdges(faceResult.offsetEdges);
            }
        }

        results.push_back(faceResult);
    }

    return results;
}
