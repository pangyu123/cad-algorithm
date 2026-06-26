#pragma once

/**
 * PointCloudToFace.h
 *
 * 功能：
 *   输入一个点集，生成一个 OCCT TopoDS_Face。
 *
 * 支持两种常见输入：
 *   1. 有序边界点：点已经按轮廓顺序排列，直接生成闭合 Wire，再生成 Face。
 *   2. 无序共面点集：自动投影到平面，计算二维凸包，再生成一个凸多边形 Face。
 *
 * 重要说明：
 *   - 只输入无序点集时，无法唯一恢复凹形边界。
 *   - 因此无序点集默认生成“包住点集的共面凸包面”。
 *   - 如果要生成凹形面，需要额外输入边界顺序或拓扑连接关系。
 *
 * 输入：
 *   std::vector<gp_Pnt> points
 *
 * 输出：
 *   TopoDS_Face
 *     - 生成成功时返回有效 Face。
 *     - 点数不足、点集共线、点集不共面或构造失败时，返回空 TopoDS_Face。
 *
 * 依赖：
 *   Open CASCADE Technology，简称 OCCT。
 */

#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>

#include <vector>

struct PointCloudToFaceOptions
{
    /**
     * 几何容差。
     * 用于判断重复点、共线、共面等。
     */
    double tolerance = 1e-6;

    /**
     * true：认为输入 points 已经是有序边界点。
     * false：认为输入 points 是无序点集，会自动生成二维凸包边界。
     */
    bool inputPointsAreOrderedBoundary = false;
};

class PointCloudToFace
{
public:
    /**
     * 输入：
     *   points:
     *     点集。
     *     如果 options.inputPointsAreOrderedBoundary=true，则 points 应按边界顺序排列。
     *     如果 options.inputPointsAreOrderedBoundary=false，则 points 可以是无序共面点集。
     *
     *   options:
     *     构造参数。
     *
     * 输出：
     *   TopoDS_Face:
     *     由点集生成的面。
     *     失败时返回空 TopoDS_Face。
     */
    static TopoDS_Face BuildFace(
        const std::vector<gp_Pnt>& points,
        const PointCloudToFaceOptions& options = PointCloudToFaceOptions());
};

/**
 * 简化接口。
 *
 * 输入：
 *   points: 无序共面点集。
 *
 * 输出：
 *   TopoDS_Face: 共面凸包面。
 */
TopoDS_Face BuildFaceFromPoints(const std::vector<gp_Pnt>& points);
