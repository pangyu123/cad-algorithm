#pragma once

#include <BRepAdaptor_Surface.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>

#include <Geom2d_Curve.hxx>
#include <GeomAbs_Shape.hxx>
#include <GeomAbs_SurfaceType.hxx>

#include <Precision.hxx>
#include <Standard_Failure.hxx>

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>

#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <queue>
#include <stdexcept>
#include <vector>

namespace occ
{

enum class FilletSurfaceKind
{
    Cylinder,
    Torus,
    Sphere
};

struct FilletSearchOptions
{
    // 是否识别相应解析曲面。
    bool includeCylinder = true;
    bool includeTorus = true;
    bool includeSphere = true;

    /*
     * 默认要求相邻圆角面的半径基本一致。
     *
     * true：
     *   适合搜索等半径圆角，能避免不同半径的圆角被误合并。
     *
     * false：
     *   允许变半径圆角或不同半径过渡面组成同一条链。
     */
    bool requireSimilarRadius = true;

    // 两个候选圆角面半径比较时使用的容差。
    double radiusAbsTolerance = 1.0e-4;
    double radiusRelTolerance = 0.05;

    /*
     * 判断 G1 连续时允许的法向夹角。
     *
     * STEP 模型可能没有正确记录 Continuity，
     * 因此程序会回退到边界两侧法向比较。
     */
    double tangentAngleToleranceRadians =
        3.0 * 3.14159265358979323846 / 180.0;

    /*
     * 一个正常的边圆角通常至少有两组切向支撑边：
     *
     *      支撑面 A
     *         |
     *     圆角曲面
     *         |
     *      支撑面 B
     *
     * 普通孔壁通常与上下平面垂直，因此没有两组切向支撑边。
     */
    int minimumTangentSupportSides = 2;

    /*
     * true：排除没有足够切向支撑面的候选链。
     * false：返回所有解析曲面候选链，便于调试。
     */
    bool rejectWithoutEnoughSupports = true;
};

struct FilletFaceInfo
{
    TopoDS_Face face;
    double radius = 0.0;
    FilletSurfaceKind kind = FilletSurfaceKind::Cylinder;
};

struct FilletChain
{
    /*
     * 圆角链包含的所有面。
     *
     * 对于普通非分支链，faces 会尽量按照相邻关系排序；
     * 对于发生分支的圆角网络，则使用连通遍历顺序。
     */
    std::vector<TopoDS_Face> faces;

    // 圆角链与外部面之间的全部边界边。
    std::vector<TopoDS_Edge> boundaryEdges;

    // 圆角链与支撑面切向连续的边。
    std::vector<TopoDS_Edge> tangentSupportEdges;

    // 非切向边界，通常代表一条开放圆角链的起点和终点。
    std::vector<TopoDS_Edge> endEdges;

    double minimumRadius = 0.0;
    double maximumRadius = 0.0;
    double meanRadius = 0.0;

    // 切向支撑边按顶点连通关系分组后的数量。
    int tangentSupportSideCount = 0;

    /*
     * 如果没有非切向端部边，认为圆角链沿轮廓闭合。
     *
     * 注意，这不是指组成的 Face 图一定是环形；
     * 而是指圆角条带没有明显的开放端部。
     */
    bool isClosed = false;

    // 候选面图中是否存在度数大于 2 的节点。
    bool isBranched = false;
};

class FilletChainSearcher
{
public:
    static std::vector<FilletChain> Search(
        const TopoDS_Shape& shape,
        double minimumRadius,
        double maximumRadius,
        const FilletSearchOptions& options = {})
    {
        ValidateArguments(
            shape,
            minimumRadius,
            maximumRadius,
            options);

        TopTools_IndexedMapOfShape faceMap;
        TopExp::MapShapes(shape, TopAbs_FACE, faceMap);

        TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
        TopExp::MapShapesAndAncestors(
            shape,
            TopAbs_EDGE,
            TopAbs_FACE,
            edgeFaceMap);

        const int faceCount = faceMap.Extent();

        /*
         * 所有数组按照 OCCT IndexedMap 的习惯使用 1-based 下标。
         */
        std::vector<std::optional<FilletFaceInfo>>
            candidates(static_cast<std::size_t>(faceCount + 1));

        CollectCandidateFaces(
            faceMap,
            minimumRadius,
            maximumRadius,
            options,
            candidates);

        /*
         * adjacency[i] 表示候选面 i 能够沿 G1 连续关系
         * 扩展到哪些候选面。
         */
        std::vector<std::vector<int>>
            adjacency(static_cast<std::size_t>(faceCount + 1));

        BuildCandidateGraph(
            faceMap,
            edgeFaceMap,
            candidates,
            options,
            adjacency);

        std::vector<std::vector<int>> components =
            FindConnectedComponents(candidates, adjacency);

        std::vector<FilletChain> result;
        result.reserve(components.size());

        for (const std::vector<int>& component : components)
        {
            FilletChain chain = BuildChainResult(
                component,
                faceMap,
                edgeFaceMap,
                candidates,
                adjacency,
                options);

            if (options.rejectWithoutEnoughSupports &&
                chain.tangentSupportSideCount <
                    options.minimumTangentSupportSides)
            {
                continue;
            }

            result.push_back(std::move(chain));
        }

        return result;
    }

    static TopoDS_Compound MakeCompound(const FilletChain& chain)
    {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);

        for (const TopoDS_Face& face : chain.faces)
        {
            builder.Add(compound, face);
        }

        return compound;
    }

private:
    static void ValidateArguments(
        const TopoDS_Shape& shape,
        double minimumRadius,
        double maximumRadius,
        const FilletSearchOptions& options)
    {
        if (shape.IsNull())
        {
            throw std::invalid_argument(
                "FilletChainSearcher: input shape is null.");
        }

        if (!std::isfinite(minimumRadius) ||
            !std::isfinite(maximumRadius))
        {
            throw std::invalid_argument(
                "FilletChainSearcher: radius must be finite.");
        }

        if (minimumRadius <= 0.0 ||
            maximumRadius <= 0.0 ||
            minimumRadius > maximumRadius)
        {
            throw std::invalid_argument(
                "FilletChainSearcher: invalid radius range.");
        }

        if (options.radiusAbsTolerance < 0.0 ||
            options.radiusRelTolerance < 0.0 ||
            options.tangentAngleToleranceRadians < 0.0)
        {
            throw std::invalid_argument(
                "FilletChainSearcher: tolerance must not be negative.");
        }
    }

    static bool IsRadiusInRange(
        double radius,
        double minimumRadius,
        double maximumRadius,
        const FilletSearchOptions& options)
    {
        /*
         * 范围边界主要使用绝对容差。
         *
         * 半径之间是否属于同一个规格，则在
         * AreRadiiCompatible() 中同时使用绝对和相对容差。
         */
        const double tolerance = options.radiusAbsTolerance;

        return radius >= minimumRadius - tolerance &&
               radius <= maximumRadius + tolerance;
    }

    static std::optional<FilletFaceInfo> ClassifyCandidateFace(
        const TopoDS_Face& face,
        double minimumRadius,
        double maximumRadius,
        const FilletSearchOptions& options)
    {
        try
        {
            BRepAdaptor_Surface surface(face, Standard_True);

            FilletFaceInfo info;
            info.face = face;

            switch (surface.GetType())
            {
            case GeomAbs_Cylinder:
            {
                if (!options.includeCylinder)
                {
                    return std::nullopt;
                }

                info.radius = surface.Cylinder().Radius();
                info.kind = FilletSurfaceKind::Cylinder;
                break;
            }

            case GeomAbs_Torus:
            {
                if (!options.includeTorus)
                {
                    return std::nullopt;
                }

                /*
                 * 圆环面的 MinorRadius 才是滚动圆角半径。
                 * MajorRadius 是圆环中心线到旋转轴的距离。
                 */
                info.radius = surface.Torus().MinorRadius();
                info.kind = FilletSurfaceKind::Torus;
                break;
            }

            case GeomAbs_Sphere:
            {
                if (!options.includeSphere)
                {
                    return std::nullopt;
                }

                info.radius = surface.Sphere().Radius();
                info.kind = FilletSurfaceKind::Sphere;
                break;
            }

            default:
                return std::nullopt;
            }

            if (!std::isfinite(info.radius) ||
                info.radius <= Precision::Confusion())
            {
                return std::nullopt;
            }

            if (!IsRadiusInRange(
                    info.radius,
                    minimumRadius,
                    maximumRadius,
                    options))
            {
                return std::nullopt;
            }

            return info;
        }
        catch (const Standard_Failure&)
        {
            /*
             * 某些损坏或不完整的 STEP 面可能无法创建 adaptor。
             * 单个面失败时，不应让整个搜索过程失败。
             */
            return std::nullopt;
        }
    }

    static void CollectCandidateFaces(
        const TopTools_IndexedMapOfShape& faceMap,
        double minimumRadius,
        double maximumRadius,
        const FilletSearchOptions& options,
        std::vector<std::optional<FilletFaceInfo>>& candidates)
    {
        for (int faceIndex = 1;
             faceIndex <= faceMap.Extent();
             ++faceIndex)
        {
            const TopoDS_Face face =
                TopoDS::Face(faceMap.FindKey(faceIndex));

            candidates[static_cast<std::size_t>(faceIndex)] =
                ClassifyCandidateFace(
                    face,
                    minimumRadius,
                    maximumRadius,
                    options);
        }
    }

    static bool AreRadiiCompatible(
        double firstRadius,
        double secondRadius,
        const FilletSearchOptions& options)
    {
        if (!options.requireSimilarRadius)
        {
            return true;
        }

        const double reference =
            std::max(std::abs(firstRadius), std::abs(secondRadius));

        const double tolerance =
            std::max(
                options.radiusAbsTolerance,
                reference * options.radiusRelTolerance);

        return std::abs(firstRadius - secondRadius) <= tolerance;
    }

    static bool IsG1OrHigher(GeomAbs_Shape continuity)
    {
        switch (continuity)
        {
        case GeomAbs_G1:
        case GeomAbs_C1:
        case GeomAbs_G2:
        case GeomAbs_C2:
        case GeomAbs_C3:
        case GeomAbs_CN:
            return true;

        default:
            return false;
        }
    }

    static bool IsNormalContinuousAtEdge(
        const TopoDS_Edge& edge,
        const TopoDS_Face& firstFace,
        const TopoDS_Face& secondFace,
        double angleTolerance)
    {
        try
        {
            Standard_Real firstParameter1 = 0.0;
            Standard_Real lastParameter1 = 0.0;
            Standard_Real firstParameter2 = 0.0;
            Standard_Real lastParameter2 = 0.0;

            Handle(Geom2d_Curve) pcurve1 =
                BRep_Tool::CurveOnSurface(
                    edge,
                    firstFace,
                    firstParameter1,
                    lastParameter1);

            Handle(Geom2d_Curve) pcurve2 =
                BRep_Tool::CurveOnSurface(
                    edge,
                    secondFace,
                    firstParameter2,
                    lastParameter2);

            if (pcurve1.IsNull() || pcurve2.IsNull())
            {
                return false;
            }

            if (!std::isfinite(firstParameter1) ||
                !std::isfinite(lastParameter1) ||
                !std::isfinite(firstParameter2) ||
                !std::isfinite(lastParameter2))
            {
                return false;
            }

            BRepAdaptor_Surface surface1(firstFace, Standard_True);
            BRepAdaptor_Surface surface2(secondFace, Standard_True);

            constexpr std::array<double, 3> sampleRatios {
                0.2,
                0.5,
                0.8
            };

            const double cosineLimit = std::cos(angleTolerance);
            int validSampleCount = 0;

            for (double ratio : sampleRatios)
            {
                const double parameter1 =
                    firstParameter1 +
                    ratio * (lastParameter1 - firstParameter1);

                const double parameter2 =
                    firstParameter2 +
                    ratio * (lastParameter2 - firstParameter2);

                const gp_Pnt2d uv1 = pcurve1->Value(parameter1);
                const gp_Pnt2d uv2 = pcurve2->Value(parameter2);

                BRepLProp_SLProps properties1(
                    surface1,
                    uv1.X(),
                    uv1.Y(),
                    1,
                    Precision::Confusion());

                BRepLProp_SLProps properties2(
                    surface2,
                    uv2.X(),
                    uv2.Y(),
                    1,
                    Precision::Confusion());

                if (!properties1.IsNormalDefined() ||
                    !properties2.IsNormalDefined())
                {
                    continue;
                }

                const gp_Dir normal1 = properties1.Normal();
                const gp_Dir normal2 = properties2.Normal();

                /*
                 * Face Orientation 可能导致两个法向方向相反，
                 * 因此取点积绝对值。
                 *
                 * 平行或反平行都表示切平面一致。
                 */
                const double absoluteDot =
                    std::abs(normal1.Dot(normal2));

                if (absoluteDot < cosineLimit)
                {
                    return false;
                }

                ++validSampleCount;
            }

            return validSampleCount > 0;
        }
        catch (const Standard_Failure&)
        {
            return false;
        }
    }

    static bool IsTangentiallyContinuous(
        const TopoDS_Edge& edge,
        const TopoDS_Face& firstFace,
        const TopoDS_Face& secondFace,
        const FilletSearchOptions& options)
    {
        if (firstFace.IsSame(secondFace))
        {
            return false;
        }

        /*
         * 优先使用 BRep 中保存的连续性信息。
         */
        try
        {
            const GeomAbs_Shape storedContinuity =
                BRep_Tool::Continuity(
                    edge,
                    firstFace,
                    secondFace);

            if (IsG1OrHigher(storedContinuity))
            {
                return true;
            }
        }
        catch (const Standard_Failure&)
        {
            // 转入几何法向检查。
        }

        /*
         * STEP 导入后，连续性标签可能不存在或只有 C0，
         * 此时直接比较共边两侧的曲面法向。
         */
        return IsNormalContinuousAtEdge(
            edge,
            firstFace,
            secondFace,
            options.tangentAngleToleranceRadians);
    }

    static void AddUniqueNeighbor(
        std::vector<int>& neighbors,
        int value)
    {
        if (std::find(
                neighbors.begin(),
                neighbors.end(),
                value) == neighbors.end())
        {
            neighbors.push_back(value);
        }
    }

    static std::vector<int> GetUniqueFaceIndices(
        const TopTools_ListOfShape& faceList,
        const TopTools_IndexedMapOfShape& faceMap)
    {
        std::vector<int> indices;

        for (TopTools_ListIteratorOfListOfShape iterator(faceList);
             iterator.More();
             iterator.Next())
        {
            const int index =
                faceMap.FindIndex(iterator.Value());

            if (index <= 0)
            {
                continue;
            }

            if (std::find(
                    indices.begin(),
                    indices.end(),
                    index) == indices.end())
            {
                indices.push_back(index);
            }
        }

        return indices;
    }

    static void BuildCandidateGraph(
        const TopTools_IndexedMapOfShape& faceMap,
        const TopTools_IndexedDataMapOfShapeListOfShape& edgeFaceMap,
        const std::vector<std::optional<FilletFaceInfo>>& candidates,
        const FilletSearchOptions& options,
        std::vector<std::vector<int>>& adjacency)
    {
        for (int edgeIndex = 1;
             edgeIndex <= edgeFaceMap.Extent();
             ++edgeIndex)
        {
            const TopoDS_Edge edge =
                TopoDS::Edge(edgeFaceMap.FindKey(edgeIndex));

            const std::vector<int> adjacentFaces =
                GetUniqueFaceIndices(
                    edgeFaceMap.FindFromIndex(edgeIndex),
                    faceMap);

            for (std::size_t i = 0;
                 i < adjacentFaces.size();
                 ++i)
            {
                const int firstIndex = adjacentFaces[i];

                if (!candidates[
                        static_cast<std::size_t>(firstIndex)])
                {
                    continue;
                }

                for (std::size_t j = i + 1;
                     j < adjacentFaces.size();
                     ++j)
                {
                    const int secondIndex = adjacentFaces[j];

                    if (!candidates[
                            static_cast<std::size_t>(secondIndex)])
                    {
                        continue;
                    }

                    const FilletFaceInfo& firstCandidate =
                        *candidates[
                            static_cast<std::size_t>(firstIndex)];

                    const FilletFaceInfo& secondCandidate =
                        *candidates[
                            static_cast<std::size_t>(secondIndex)];

                    if (!AreRadiiCompatible(
                            firstCandidate.radius,
                            secondCandidate.radius,
                            options))
                    {
                        continue;
                    }

                    const TopoDS_Face firstFace =
                        TopoDS::Face(
                            faceMap.FindKey(firstIndex));

                    const TopoDS_Face secondFace =
                        TopoDS::Face(
                            faceMap.FindKey(secondIndex));

                    if (!IsTangentiallyContinuous(
                            edge,
                            firstFace,
                            secondFace,
                            options))
                    {
                        continue;
                    }

                    AddUniqueNeighbor(
                        adjacency[
                            static_cast<std::size_t>(firstIndex)],
                        secondIndex);

                    AddUniqueNeighbor(
                        adjacency[
                            static_cast<std::size_t>(secondIndex)],
                        firstIndex);
                }
            }
        }
    }

    static std::vector<std::vector<int>>
    FindConnectedComponents(
        const std::vector<std::optional<FilletFaceInfo>>& candidates,
        const std::vector<std::vector<int>>& adjacency)
    {
        const int faceCount =
            static_cast<int>(candidates.size()) - 1;

        std::vector<char>
            visited(static_cast<std::size_t>(faceCount + 1), false);

        std::vector<std::vector<int>> components;

        for (int startIndex = 1;
             startIndex <= faceCount;
             ++startIndex)
        {
            if (visited[static_cast<std::size_t>(startIndex)] ||
                !candidates[static_cast<std::size_t>(startIndex)])
            {
                continue;
            }

            std::vector<int> component;
            std::queue<int> pending;

            visited[static_cast<std::size_t>(startIndex)] = true;
            pending.push(startIndex);

            while (!pending.empty())
            {
                const int current = pending.front();
                pending.pop();

                component.push_back(current);

                for (int neighbor :
                     adjacency[static_cast<std::size_t>(current)])
                {
                    if (visited[
                            static_cast<std::size_t>(neighbor)])
                    {
                        continue;
                    }

                    visited[
                        static_cast<std::size_t>(neighbor)] = true;

                    pending.push(neighbor);
                }
            }

            components.push_back(std::move(component));
        }

        return components;
    }

    static bool ContainsIndex(
        const std::vector<int>& values,
        int value)
    {
        return std::find(
                   values.begin(),
                   values.end(),
                   value) != values.end();
    }

    static bool IsBranchedComponent(
        const std::vector<int>& component,
        const std::vector<std::vector<int>>& adjacency)
    {
        for (int faceIndex : component)
        {
            int innerDegree = 0;

            for (int neighbor :
                 adjacency[static_cast<std::size_t>(faceIndex)])
            {
                if (ContainsIndex(component, neighbor))
                {
                    ++innerDegree;
                }
            }

            if (innerDegree > 2)
            {
                return true;
            }
        }

        return false;
    }

    static std::vector<int> OrderComponentFaces(
        const std::vector<int>& component,
        const std::vector<std::vector<int>>& adjacency)
    {
        if (component.empty())
        {
            return {};
        }

        const bool branched =
            IsBranchedComponent(component, adjacency);

        /*
         * 有分支时没有唯一的线性顺序，使用 BFS 顺序。
         */
        if (branched)
        {
            std::vector<int> result;
            std::vector<int> pending {component.front()};

            while (!pending.empty())
            {
                const int current = pending.front();
                pending.erase(pending.begin());

                if (ContainsIndex(result, current))
                {
                    continue;
                }

                result.push_back(current);

                for (int neighbor :
                     adjacency[static_cast<std::size_t>(current)])
                {
                    if (ContainsIndex(component, neighbor) &&
                        !ContainsIndex(result, neighbor))
                    {
                        pending.push_back(neighbor);
                    }
                }
            }

            return result;
        }

        /*
         * 对普通链，优先从度数为 1 的端点开始。
         * 如果没有端点，说明它可能是一个环，从任意面开始。
         */
        int start = component.front();

        for (int faceIndex : component)
        {
            int innerDegree = 0;

            for (int neighbor :
                 adjacency[static_cast<std::size_t>(faceIndex)])
            {
                if (ContainsIndex(component, neighbor))
                {
                    ++innerDegree;
                }
            }

            if (innerDegree == 1)
            {
                start = faceIndex;
                break;
            }
        }

        std::vector<int> ordered;
        int previous = -1;
        int current = start;

        while (current > 0 &&
               !ContainsIndex(ordered, current))
        {
            ordered.push_back(current);

            int next = -1;

            for (int neighbor :
                 adjacency[static_cast<std::size_t>(current)])
            {
                if (!ContainsIndex(component, neighbor) ||
                    neighbor == previous ||
                    ContainsIndex(ordered, neighbor))
                {
                    continue;
                }

                next = neighbor;
                break;
            }

            previous = current;
            current = next;
        }

        /*
         * 理论上非分支连通分量都能在线性遍历中覆盖。
         * 这里保留兜底，防止异常拓扑造成遗漏。
         */
        for (int faceIndex : component)
        {
            if (!ContainsIndex(ordered, faceIndex))
            {
                ordered.push_back(faceIndex);
            }
        }

        return ordered;
    }

    static bool ShareVertex(
        const TopoDS_Edge& firstEdge,
        const TopoDS_Edge& secondEdge)
    {
        TopoDS_Vertex firstStart;
        TopoDS_Vertex firstEnd;
        TopoDS_Vertex secondStart;
        TopoDS_Vertex secondEnd;

        TopExp::Vertices(
            firstEdge,
            firstStart,
            firstEnd);

        TopExp::Vertices(
            secondEdge,
            secondStart,
            secondEnd);

        const auto sameVertex =
            [](const TopoDS_Vertex& first,
               const TopoDS_Vertex& second)
            {
                return !first.IsNull() &&
                       !second.IsNull() &&
                       first.IsSame(second);
            };

        return sameVertex(firstStart, secondStart) ||
               sameVertex(firstStart, secondEnd) ||
               sameVertex(firstEnd, secondStart) ||
               sameVertex(firstEnd, secondEnd);
    }

    static int CountConnectedEdgeGroups(
        const std::vector<TopoDS_Edge>& edges)
    {
        const int edgeCount =
            static_cast<int>(edges.size());

        if (edgeCount == 0)
        {
            return 0;
        }

        std::vector<char>
            visited(static_cast<std::size_t>(edgeCount), false);

        int groupCount = 0;

        for (int start = 0;
             start < edgeCount;
             ++start)
        {
            if (visited[static_cast<std::size_t>(start)])
            {
                continue;
            }

            ++groupCount;

            std::queue<int> pending;
            pending.push(start);
            visited[static_cast<std::size_t>(start)] = true;

            while (!pending.empty())
            {
                const int current = pending.front();
                pending.pop();

                for (int candidate = 0;
                     candidate < edgeCount;
                     ++candidate)
                {
                    if (visited[
                            static_cast<std::size_t>(candidate)])
                    {
                        continue;
                    }

                    if (!ShareVertex(
                            edges[static_cast<std::size_t>(current)],
                            edges[static_cast<std::size_t>(candidate)]))
                    {
                        continue;
                    }

                    visited[
                        static_cast<std::size_t>(candidate)] = true;

                    pending.push(candidate);
                }
            }
        }

        return groupCount;
    }

    static std::vector<TopoDS_Edge> MapToEdges(
        const TopTools_IndexedMapOfShape& map)
    {
        std::vector<TopoDS_Edge> edges;
        edges.reserve(static_cast<std::size_t>(map.Extent()));

        for (int index = 1;
             index <= map.Extent();
             ++index)
        {
            edges.push_back(
                TopoDS::Edge(map.FindKey(index)));
        }

        return edges;
    }

    static FilletChain BuildChainResult(
        const std::vector<int>& component,
        const TopTools_IndexedMapOfShape& faceMap,
        const TopTools_IndexedDataMapOfShapeListOfShape& edgeFaceMap,
        const std::vector<std::optional<FilletFaceInfo>>& candidates,
        const std::vector<std::vector<int>>& adjacency,
        const FilletSearchOptions& options)
    {
        FilletChain chain;

        if (component.empty())
        {
            return chain;
        }

        const int faceCount = faceMap.Extent();

        std::vector<char>
            insideComponent(
                static_cast<std::size_t>(faceCount + 1),
                false);

        for (int faceIndex : component)
        {
            insideComponent[
                static_cast<std::size_t>(faceIndex)] = true;
        }

        const std::vector<int> orderedFaces =
            OrderComponentFaces(component, adjacency);

        chain.faces.reserve(orderedFaces.size());

        double radiusSum = 0.0;
        chain.minimumRadius =
            candidates[
                static_cast<std::size_t>(component.front())]->radius;

        chain.maximumRadius = chain.minimumRadius;

        for (int faceIndex : orderedFaces)
        {
            const FilletFaceInfo& candidate =
                *candidates[
                    static_cast<std::size_t>(faceIndex)];

            chain.faces.push_back(candidate.face);

            chain.minimumRadius =
                std::min(
                    chain.minimumRadius,
                    candidate.radius);

            chain.maximumRadius =
                std::max(
                    chain.maximumRadius,
                    candidate.radius);

            radiusSum += candidate.radius;
        }

        chain.meanRadius =
            radiusSum /
            static_cast<double>(component.size());

        chain.isBranched =
            IsBranchedComponent(component, adjacency);

        TopTools_IndexedMapOfShape boundaryEdgeMap;
        TopTools_IndexedMapOfShape tangentSupportEdgeMap;
        TopTools_IndexedMapOfShape endEdgeMap;

        for (int faceIndex : component)
        {
            const TopoDS_Face currentFace =
                TopoDS::Face(faceMap.FindKey(faceIndex));

            for (TopExp_Explorer edgeExplorer(
                     currentFace,
                     TopAbs_EDGE);
                 edgeExplorer.More();
                 edgeExplorer.Next())
            {
                const TopoDS_Edge edge =
                    TopoDS::Edge(edgeExplorer.Current());

                /*
                 * 圆柱面、圆环面等周期曲面的 seam edge
                 * 并不是真正的外部边界。
                 */
                if (BRep_Tool::IsClosed(edge, currentFace))
                {
                    continue;
                }

                const int edgeMapIndex =
                    edgeFaceMap.FindIndex(edge);

                if (edgeMapIndex <= 0)
                {
                    boundaryEdgeMap.Add(edge);
                    endEdgeMap.Add(edge);
                    continue;
                }

                const std::vector<int> adjacentFaceIndices =
                    GetUniqueFaceIndices(
                        edgeFaceMap.FindFromIndex(edgeMapIndex),
                        faceMap);

                int insideFaceCount = 0;

                for (int adjacentIndex : adjacentFaceIndices)
                {
                    if (insideComponent[
                            static_cast<std::size_t>(
                                adjacentIndex)])
                    {
                        ++insideFaceCount;
                    }
                }

                /*
                 * 边两侧都属于当前圆角链，说明它是圆角链内部
                 * 被 STEP 拆分产生的连接边。
                 */
                if (insideFaceCount >= 2)
                {
                    continue;
                }

                boundaryEdgeMap.Add(edge);

                bool hasTangentSupport = false;

                for (int adjacentIndex : adjacentFaceIndices)
                {
                    if (insideComponent[
                            static_cast<std::size_t>(
                                adjacentIndex)])
                    {
                        continue;
                    }

                    const TopoDS_Face outsideFace =
                        TopoDS::Face(
                            faceMap.FindKey(adjacentIndex));

                    if (IsTangentiallyContinuous(
                            edge,
                            currentFace,
                            outsideFace,
                            options))
                    {
                        hasTangentSupport = true;
                        break;
                    }
                }

                if (hasTangentSupport)
                {
                    tangentSupportEdgeMap.Add(edge);
                }
                else
                {
                    /*
                     * 没有外部面，或者与外部面不是切向连续，
                     * 都作为圆角链的开放端部处理。
                     */
                    endEdgeMap.Add(edge);
                }
            }
        }

        chain.boundaryEdges =
            MapToEdges(boundaryEdgeMap);

        chain.tangentSupportEdges =
            MapToEdges(tangentSupportEdgeMap);

        chain.endEdges =
            MapToEdges(endEdgeMap);

        chain.tangentSupportSideCount =
            CountConnectedEdgeGroups(
                chain.tangentSupportEdges);

        chain.isClosed =
            chain.endEdges.empty() &&
            chain.tangentSupportSideCount >=
                options.minimumTangentSupportSides;

        return chain;
    }
};

} // namespace occ







#include "FilletChainSearcher.hxx"

void SearchFillets(const TopoDS_Shape& shape)
{
    occ::FilletSearchOptions options;

    // 半径相差 3% 以内认为属于同一规格。
    options.radiusRelTolerance = 0.03;

    // STEP 模型精度一般可根据模型单位调整。
    options.radiusAbsTolerance = 1.0e-3;

    // 法向夹角 5° 内认为是切向连续。
    options.tangentAngleToleranceRadians =
        5.0 * 3.14159265358979323846 / 180.0;

    // 默认要求圆角至少与两组支撑边相切。
    options.minimumTangentSupportSides = 2;

    const double minimumRadius = 1.0;
    const double maximumRadius = 5.0;

    const std::vector<occ::FilletChain> chains =
        occ::FilletChainSearcher::Search(
            shape,
            minimumRadius,
            maximumRadius,
            options);

    for (std::size_t i = 0; i < chains.size(); ++i)
    {
        const occ::FilletChain& chain = chains[i];

        std::cout
            << "Fillet chain: " << i
            << "\n  face count: " << chain.faces.size()
            << "\n  radius range: "
            << chain.minimumRadius
            << " - "
            << chain.maximumRadius
            << "\n  support sides: "
            << chain.tangentSupportSideCount
            << "\n  closed: "
            << std::boolalpha
            << chain.isClosed
            << "\n  branched: "
            << chain.isBranched
            << "\n";
    }
}