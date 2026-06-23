#pragma once

/**
 * SymmetryPlaneDetector.h
 *
 * 功能：
 *   从一个 OCCT TopoDS_Shape 中自动检测平面对称面。
 *
 * 输入：
 *   const TopoDS_Shape& inputShape
 *     - 任意 OCCT 形体，例如 Box、STEP 导入模型、机械零件、螺栓等。
 *     - 调用方不需要额外告诉算法模型类型。
 *
 * 输出：
 *   std::vector<TopoDS_Shape> planeShapes
 *     - 表示检测到的一个或多个对称平面的有限矩形面片。
 *     - 每个元素实际类型通常是 TopoDS_Face，但以 TopoDS_Shape 返回。
 *     - 注意：数学平面是无限大的，TopoDS_Shape 必须是有限几何，因此这里输出的是足够大的平面 Face。
 *
 *   兼容接口：
 *     - FindSymmetryPlaneShape(...) 返回最优的一个对称平面。
 *     - FindSymmetryPlaneShapes(...) 返回所有可接受的对称平面。
 *
 * 依赖：
 *   Open CASCADE Technology，简称 OCCT。
 *   不依赖 Eigen。
 */

#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>

#include <cstddef>
#include <vector>

/**
 * 对称面质量等级。
 */
enum class SymmetryQuality
{
    Strict,       // 严格对称：细节也基本镜像一致
    Approximate,  // 近似对称：主体近似对称，但局部细节可能破坏严格对称
    Rejected      // 不接受：误差超过容差
};

/**
 * 对称面检测参数。
 */
struct SymmetryDetectionOptions
{
    /**
     * 输入采样网格精度。
     * 数值越小，采样越细，检测越慢。
     * 建议根据模型尺寸设置，例如 bboxDiagonal * 0.002 ~ bboxDiagonal * 0.01。
     */
    double meshDeflection = 0.5;

    /**
     * 最大采样点数量。
     * 当前最近邻搜索是 O(n^2)，点数过大会变慢。
     */
    std::size_t maxSamples = 5000;

    /**
     * 严格对称容差。
     * 使用 rmsError / bboxDiagonal 归一化后的误差。
     */
    double strictTolerance = 1e-4;

    /**
     * 近似对称容差。
     * 使用 rmsError / bboxDiagonal 归一化后的误差。
     */
    double approximateTolerance = 5e-3;

    /**
     * 对每条圆柱轴生成多少个经过轴线的候选平面。
     * 36 表示每 10 度生成一个候选面。
     */
    int axisPlaneCandidateCount = 36;

    /**
     * 输出平面大小比例。
     * 输出平面半尺寸 = bboxDiagonal * planeSizeScale。
     */
    double planeSizeScale = 1.5;

    /**
     * 是否使用包围盒中心平面作为候选面。
     */
    bool useBoundingBoxCenterPlanes = true;

    /**
     * 是否使用圆柱面轴线生成候选面。
     */
    bool useCylinderAxisPlanes = true;

    /**
     * 是否使用平面 Face 对生成中间候选面。
     */
    bool usePlanarFacePairMidPlanes = true;
};

/**
 * 单个候选对称面的检测结果。
 */
struct SymmetryPlaneShapeResult
{
    /**
     * 输出：有限平面面片。
     * 实际上是 TopoDS_Face，但用 TopoDS_Shape 保存，方便统一返回。
     */
    TopoDS_Shape planeShape;

    /**
     * 输出：数学平面。
     * 如果外部只需要 TopoDS_Shape，可以忽略此字段。
     */
    gp_Pln plane;

    /**
     * 输出：镜像后的点云到原点云的 RMS 误差，单位与模型一致。
     */
    double rmsError = 0.0;

    /**
     * 输出：归一化误差。
     * normalizedError = rmsError / bboxDiagonal。
     */
    double normalizedError = 0.0;

    /**
     * 输出：该候选平面是否可作为严格或近似对称面。
     */
    SymmetryQuality quality = SymmetryQuality::Rejected;
};

/**
 * 无 Eigen 版本的 OCCT 平面对称检测器。
 *
 * 类定义放在头文件中，算法实现放在 SymmetryPlaneDetector.cpp 中。
 */
class SymmetryPlaneDetector
{
public:
    /**
     * 输入：
     *   inputShape: 需要检测对称面的 OCCT 形体。
     *   options: 检测参数。
     *
     * 输出：
     *   std::vector<SymmetryPlaneShapeResult>
     *     - 所有候选平面，按 normalizedError 从小到大排序。
     *     - 每个 result.planeShape 是对应的 TopoDS_Shape 平面。
     *     - result.quality 为 Strict 或 Approximate 时，表示该平面可接受。
     */
    static std::vector<SymmetryPlaneShapeResult> Detect(
        const TopoDS_Shape& inputShape,
        const SymmetryDetectionOptions& options = SymmetryDetectionOptions());

    /**
     * 输入：
     *   inputShape: 需要检测对称面的 OCCT 形体。
     *   options: 检测参数。
     *
     * 输出：
     *   TopoDS_Shape
     *     - 最优且可接受的对称平面 Face。
     *     - 如果没有找到可接受的对称面，则返回空 TopoDS_Shape。
     */
    static TopoDS_Shape DetectBestPlaneShape(
        const TopoDS_Shape& inputShape,
        const SymmetryDetectionOptions& options = SymmetryDetectionOptions());

    /**
     * 输入：
     *   inputShape: 需要检测对称面的 OCCT 形体。
     *   options: 检测参数。
     *
     * 输出：
     *   std::vector<TopoDS_Shape>
     *     - 所有可接受的对称平面 Face。
     *     - 包含 Strict 和 Approximate 两类结果。
     *     - 已按 normalizedError 从小到大排序。
     *     - 如果没有找到可接受的对称面，则返回空 vector。
     */
    static std::vector<TopoDS_Shape> DetectPlaneShapes(
        const TopoDS_Shape& inputShape,
        const SymmetryDetectionOptions& options = SymmetryDetectionOptions());
};

/**
 * 简化接口。
 *
 * 输入：
 *   inputShape: 任意 TopoDS_Shape。
 *
 * 输出：
 *   TopoDS_Shape: 最优对称平面 Face；未找到时返回空 Shape。
 */
TopoDS_Shape FindSymmetryPlaneShape(const TopoDS_Shape& inputShape);

/**
 * 简化接口：返回多个对称平面。
 *
 * 输入：
 *   inputShape: 任意 TopoDS_Shape。
 *
 * 输出：
 *   std::vector<TopoDS_Shape>
 *     - 所有可接受的对称平面 Face。
 *     - 如果没有找到，则返回空 vector。
 */
std::vector<TopoDS_Shape> FindSymmetryPlaneShapes(const TopoDS_Shape& inputShape);
