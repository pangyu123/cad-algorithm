#pragma once

/**
 * ZoneCutOffsetEdge.h
 *
 * 功能：
 *   基于 OCCT 实现类似 ANSA Zone Cut 的局部偏移边生成。
 *
 * 支持两种输入：
 *   1. 单条输入边 TopoDS_Edge
 *   2. 多条相邻输入边 std::vector<TopoDS_Edge>
 *
 * 输入：
 *   solidShape:
 *     包含输入边的实体、壳、面组或复合模型。
 *
 *   inputEdges:
 *     一条或多条相邻边。
 *     多条边通常表示一段连续边链。
 *
 *   offsetDistance:
 *     偏移距离，单位与模型单位一致。
 *
 * 输出：
 *   对每个邻接面，生成该面上的一组偏移边。
 *   如果输入多条边，则在同一个邻接面上会生成多条对应的 offset edge。
 *
 * 说明：
 *   - 对平面，偏移结果较准确。
 *   - 对曲面，算法采用“沿面内垂直方向偏移 + 投影回面 + BSpline 拟合”的近似方法。
 *   - 如果偏移点超出 trimmed face，且 requireInsideFace=true，则该段可能失败。
 */

#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <vector>

struct ZoneCutOffsetOptions
{
    /**
     * 每条输入边上的采样点数量。
     * 越大越精细，但越慢。
     */
    int sampleCount = 40;

    /**
     * 几何容差。
     */
    double tolerance = 1e-6;

    /**
     * 投影点是否必须位于 trimmed face 内部。
     * true：偏移点必须在 Face 有效区域内。
     * false：只要求投影到 underlying surface。
     */
    bool requireInsideFace = true;

    /**
     * 是否把同一邻接面上的多条 offset edge 组合为一个 compound。
     * true：offsetShape 中保存 compound。
     * false：offsetShape 为空，只通过 offsetEdges 返回。
     */
    bool buildCompoundShape = true;
};

struct ZoneCutOffsetResult
{
    /**
     * 当前结果所属的邻接面。
     */
    TopoDS_Face sourceFace;

    /**
     * 在 sourceFace 上参与偏移的输入边。
     * 单边输入时通常只有一条。
     * 多边输入时可能包含多条。
     */
    std::vector<TopoDS_Edge> sourceEdges;

    /**
     * 在 sourceFace 上生成的偏移边。
     * 与 sourceEdges 大致一一对应。
     */
    std::vector<TopoDS_Edge> offsetEdges;

    /**
     * 输出 Shape。
     * 如果 buildCompoundShape=true，则这里是由 offsetEdges 组成的 TopoDS_Compound。
     * 如果只有一条 offset edge，也会直接返回该 edge。
     */
    TopoDS_Shape offsetShape;

    /**
     * 是否至少成功生成了一条偏移边。
     */
    bool success = false;
};

class ZoneCutOffsetEdge
{
public:
    /**
     * 单条边版本。
     *
     * 输入：
     *   solidShape: 包含 inputEdge 的实体、壳或复合模型。
     *   inputEdge: 需要偏移的边。
     *   offsetDistance: 偏移距离。
     *
     * 输出：
     *   每个邻接面对应一个 ZoneCutOffsetResult。
     */
    static std::vector<ZoneCutOffsetResult> GenerateOffsetEdgesOnAdjacentFaces(
        const TopoDS_Shape& solidShape,
        const TopoDS_Edge& inputEdge,
        double offsetDistance,
        const ZoneCutOffsetOptions& options = ZoneCutOffsetOptions());

    /**
     * 多条相邻边版本。
     *
     * 输入：
     *   solidShape: 包含 inputEdges 的实体、壳或复合模型。
     *   inputEdges: 多条相邻边，通常是一段连续边链。
     *   offsetDistance: 偏移距离。
     *
     * 输出：
     *   每个邻接面对应一个 ZoneCutOffsetResult。
     *   result.offsetEdges 中包含该邻接面上生成的多条偏移边。
     */
    static std::vector<ZoneCutOffsetResult> GenerateOffsetEdgesOnAdjacentFaces(
        const TopoDS_Shape& solidShape,
        const std::vector<TopoDS_Edge>& inputEdges,
        double offsetDistance,
        const ZoneCutOffsetOptions& options = ZoneCutOffsetOptions());
};
