#pragma once

#include <vector>
#include <stdexcept>

#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Compound.hxx>

#include <Geom2d_Curve.hxx>

/*1. 根据输入 Edge 找相邻 Face
2. 按 Face 分组
3. 对每个 Face 上的 Edge 组成连通链
4. 获取每条 Edge 在 Face 上的 pcurve
5. 判断 Face 内侧偏移方向
6. 在 Face 上生成偏移曲线
7. 对相邻偏移线用切线延伸求交点，产生尖角交汇
8. 可选：两端延伸到 Face 边界
9. 用 2D pcurve + Face surface 生成新 TopoDS_Edge
10. 可选输出线体 Compound
11. 可选用 BRepFeat_SplitShape 刻印回 shape*/

class ZoneCutBuilder
{
public:
	ZoneCutBuilder(
		const TopoDS_Shape& shape,
		const std::vector<TopoDS_Edge>& inputEdges);

	void SetOffsetDistance(double distance);
	void SetCount(int count);
	void SetExtendToBoundary(bool flag);
	void SetCreateWireBody(bool flag);

	void Build();

	const TopoDS_Shape& Result() const;
	const std::vector<TopoDS_Edge>& GeneratedEdges() const;

private:
	struct EdgeOnFace
	{
		TopoDS_Edge edge;
		TopoDS_Face face;

		Handle(Geom2d_Curve) pcurve;
		double first = 0.0;
		double last = 0.0;

		bool reversed = false;
	};

	struct ChainEdgeRef
	{
		TopoDS_Edge edge;
		bool reversed = false;
	};

	struct FaceEdgeChain
	{
		TopoDS_Face face;
		std::vector<ChainEdgeRef> edges;
		bool closed = false;
	};

	struct OffsetCurveOnFace
	{
		TopoDS_Face face;

		Handle(Geom2d_Curve) curve2d;
		double first = 0.0;
		double last = 0.0;

		TopoDS_Edge resultEdge;
	};

private:
	void ValidateInput() const;

	void CollectFaceEdgeChains();
	std::vector<FaceEdgeChain> BuildConnectedChains(
		const TopoDS_Face& face,
		const std::vector<TopoDS_Edge>& edges) const;

	EdgeOnFace PrepareEdgeOnFace(
		const TopoDS_Face& face,
		const ChainEdgeRef& ref) const;

	int DetectInnerOffsetSide(
		const EdgeOnFace& edgeOnFace) const;

	OffsetCurveOnFace BuildOffsetCurveOnFace(
		const EdgeOnFace& edgeOnFace,
		double offsetDistance,
		int offsetSide) const;

	std::vector<OffsetCurveOnFace> ConnectOffsetCurves(
		const TopoDS_Face& face,
		const std::vector<OffsetCurveOnFace>& inputCurves,
		bool closed) const;

	std::vector<OffsetCurveOnFace> ExtendChainToBoundary(
		const TopoDS_Face& face,
		const std::vector<OffsetCurveOnFace>& inputCurves,
		bool closed) const;

	OffsetCurveOnFace ExtendCurveToPoint(
		const OffsetCurveOnFace& curve,
		const gp_Pnt2d& targetPoint,
		bool extendEnd) const;

	bool FindExtensionPointOnBoundary(
		const TopoDS_Face& face,
		const Handle(Geom2d_Curve)& curve,
		double parameter,
		bool forward,
		gp_Pnt2d& boundaryPoint) const;

	TopoDS_Edge MakeEdgeOnFace(
		const TopoDS_Face& face,
		const Handle(Geom2d_Curve)& curve2d,
		double first,
		double last) const;

	TopoDS_Shape BuildWireBodyResult() const;
	TopoDS_Shape BuildImprintedResult() const;

	static bool SameVertex(
		const TopoDS_Vertex& a,
		const TopoDS_Vertex& b);

	static void GetEdgeVertices(
		const TopoDS_Edge& edge,
		TopoDS_Vertex& v1,
		TopoDS_Vertex& v2);

	static gp_Pnt2d CurvePoint2d(
		const Handle(Geom2d_Curve)& curve,
		double parameter);

	static double Distance2d(
		const gp_Pnt2d& a,
		const gp_Pnt2d& b);

	static int EstimateSampleCount(
		const TopoDS_Edge& edge);

private:
	TopoDS_Shape myShape;
	std::vector<TopoDS_Edge> myInputEdges;

	double myOffsetDistance = 1.0;
	int myCount = 1;
	bool myExtendToBoundary = false;
	bool myCreateWireBody = true;

	std::vector<FaceEdgeChain> myChains;
	std::vector<OffsetCurveOnFace> myGeneratedCurves;
	std::vector<TopoDS_Edge> myGeneratedEdges;

	TopoDS_Shape myResult;
};