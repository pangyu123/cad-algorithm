/// @file BoundingCylinder_Usage.cpp
/// @brief BoundingCylinder 使用示例

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "BoundingCylinder.h"

#include <TopoDS_Shape.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepAlgoAPI_Fuse.hxx>

#include <iostream>
#include <cmath>

// ===========================================================================
//  基础用法
// ===========================================================================
void Example1_BasicUsage()
{
    // 创建一个 L 形组合体
    TopoDS_Shape box1 = BRepPrimAPI_MakeBox(10.0, 50.0, 10.0).Shape();
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(50.0, 10.0, 10.0).Shape();
    TopoDS_Shape shape = BRepAlgoAPI_Fuse(box1, box2).Shape();

    // 默认选项：自动选择最小体积方案
    BoundingCylinder::Result result = BoundingCylinder::Compute(shape);

    std::cout << "=== Basic Usage ===\n";
    std::cout << "Radius: " << result.radius << '\n';
    std::cout << "Height: " << result.height << '\n';
    std::cout << "Axis: (" << result.axis.X() << ", "
              << result.axis.Y() << ", "
              << result.axis.Z() << ")\n";
    std::cout << "Sample points: " << result.sampleCount << '\n';
    std::cout << "Volume: "
              << (M_PI * result.radius * result.radius * result.height)
              << "\n\n";
}

// ===========================================================================
//  自定义选项
// ===========================================================================
void Example2_CustomOptions()
{
    TopoDS_Shape shape = BRepPrimAPI_MakeBox(10.0, 30.0, 20.0).Shape();

    BoundingCylinder::Options options;

    // 使用 PCA 第一主方向作为圆柱轴
    options.axisPolicy =
        BoundingCylinder::AxisPolicy::PcaFirstPrincipal;

    // 圆柱径向留 0.5 mm 余量
    options.radialClearance = 0.5;

    // 轴向两端各留 0.5 mm 余量
    options.axialClearance = 0.5;

    BoundingCylinder::Result result =
        BoundingCylinder::Compute(shape, options);

    std::cout << "=== Custom Options ===\n";
    std::cout << "Radius: " << result.radius
              << " (includes " << options.radialClearance
              << " mm clearance)\n";
    std::cout << "Height: " << result.height
              << " (includes " << options.axialClearance
              << " mm axial clearance)\n";
    std::cout << "Axis source: " << result.axisSource << '\n';
    std::cout << "\n";
}

// ===========================================================================
//  所有轴策略对比
// ===========================================================================
void Example3_ComparePolicies()
{
    TopoDS_Shape shape = BRepPrimAPI_MakeBox(10.0, 50.0, 20.0).Shape();

    std::cout << "=== Axis Policy Comparison ===\n";
    std::cout << "Shape: box 10 x 50 x 20\n\n";

    for (const auto& policy : {
        BoundingCylinder::AxisPolicy::PcaFirstPrincipal,
        BoundingCylinder::AxisPolicy::MinimumVolume,
        BoundingCylinder::AxisPolicy::ObbLongestAxis
    })
    {
        BoundingCylinder::Options options;
        options.axisPolicy = policy;

        BoundingCylinder::Result result =
            BoundingCylinder::Compute(shape, options);

        const char* name = "";

        switch (policy)
        {
        case BoundingCylinder::AxisPolicy::PcaFirstPrincipal:
            name = "PCA 1st";
            break;

        case BoundingCylinder::AxisPolicy::MinimumVolume:
            name = "MinVolume";
            break;

        case BoundingCylinder::AxisPolicy::ObbLongestAxis:
            name = "OBB Longest";
            break;
        }

        std::cout << "  " << name << ": "
                  << "r=" << result.radius
                  << ", h=" << result.height
                  << ", V=" << (M_PI * result.radius * result.radius * result.height)
                  << ", axisSource=" << result.axisSource
                  << '\n';
    }

    std::cout << "\n";
}

// ===========================================================================
//  球形测试
// ===========================================================================
void Example4_Sphere()
{
    const double sphereRadius = 25.0;
    TopoDS_Shape sphere =
        BRepPrimAPI_MakeSphere(sphereRadius).Shape();

    BoundingCylinder::Result result =
        BoundingCylinder::Compute(sphere);

    std::cout << "=== Sphere (r=" << sphereRadius << ") ===\n";
    std::cout << "Bounding cylinder radius: " << result.radius << '\n';
    std::cout << "Bounding cylinder height: " << result.height << '\n';

    // 理想情况下，球体的包围圆柱应该是
    // radius = 25, height = 50（高度等于直径）
    const double minIdealVol =
        M_PI * sphereRadius * sphereRadius * 2.0 * sphereRadius;

    const double actualVol =
        M_PI * result.radius * result.radius * result.height;

    std::cout << "Ideal min volume: " << minIdealVol << '\n';
    std::cout << "Actual volume:    " << actualVol << '\n';
    std::cout << "Efficiency:       "
              << (minIdealVol / actualVol * 100.0) << "%\n";
    std::cout << "\n";
}

// ===========================================================================
int main()
{
    Example1_BasicUsage();
    Example2_CustomOptions();
    Example3_ComparePolicies();
    Example4_Sphere();

    return 0;
}
