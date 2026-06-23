#pragma once

/**
 * PointCloudToClosedShape.h
 *
 * 功能：
 *   输入一个三维点集，生成一个闭合的 OCCT TopoDS_Shape 几何模型。
 *
 * 默认策略：
 *   点集 -> 三维凸包 -> 闭合 Shell -> TopoDS_Solid
 *
 * 重要说明：
 *   如果输入只有无序点集，没有三角面索引、法向、边界或点连接关系，
 *   则无法唯一恢复原始凹形结构、孔洞或内腔。
 *   因此本实现生成的是包住所有输入点的闭合凸包模型。
 *
 * 输入：
 *   std::vector<gp_Pnt> points
 *     - 无序三维点集。
 *
 * 输出：
 *   TopoDS_Shape
 *     - 优先返回 TopoDS_Solid。
 *     - 如果实体生成失败，则返回缝合后的闭合 Shell。
 *     - 如果点集不足或无法形成三维闭合体，则返回空 TopoDS_Shape。
 *
 * 依赖：
 *   Open CASCADE Technology，简称 OCCT。
 */

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <vector>

struct PointCloudToClosedShapeOptions
{
    /**
     * 几何容差。
     * 用于判断重复点、共线、共面、缝合 Shell 等。
     */
    double tolerance = 1e-6;

    /**
     * 是否尝试生成 TopoDS_Solid。
     * true：优先返回 Solid，失败时返回 Shell。
     * false：直接返回缝合后的 Shell。
     */
    bool makeSolid = true;
};

class PointCloudToClosedShape
{
public:
    /**
     * 输入：
     *   points:
     *     无序三维点集。
     *   options:
     *     构造参数。
     *
     * 输出：
     *   TopoDS_Shape:
     *     由点集凸包生成的闭合几何模型。
     *     优先返回 TopoDS_Solid。
     *     如果 Solid 构造失败，则返回缝合后的 Shell。
     *     如果无法构造闭合体，则返回空 TopoDS_Shape。
     */
    static TopoDS_Shape BuildConvexClosedShape(
        const std::vector<gp_Pnt>& points,
        const PointCloudToClosedShapeOptions& options = PointCloudToClosedShapeOptions());
};

/**
 * 简化接口。
 *
 * 输入：
 *   points:
 *     无序三维点集。
 *
 * 输出：
 *   TopoDS_Shape:
 *     闭合凸包模型。
 */
TopoDS_Shape BuildClosedShapeFromPoints(const std::vector<gp_Pnt>& points);
