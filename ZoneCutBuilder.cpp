#include "ZoneCutBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>

#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <BRepLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>

#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRepFeat_SplitShape.hxx>

#include <Geom_Surface.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_BSplineCurve.hxx>

#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <Geom2dAPI_InterCurveCurve.hxx>

#include <GCE2d_MakeSegment.hxx>

#include <TColgp_Array1OfPnt2d.hxx>

#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>
#include <gp_Vec2d.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Lin2d.hxx>

#include <Precision.hxx>
#include <TopAbs_State.hxx>

ZoneCutBuilder::ZoneCutBuilder(
	const TopoDS_Shape& shape,
	const std::vector<TopoDS_Edge>& inputEdges)
	: myShape(shape),
	myInputEdges(inputEdges)
{
}

void ZoneCutBuilder::SetOffsetDistance(double distance)
{
	myOffsetDistance = distance;
}

void ZoneCutBuilder::SetCount(int count)
{
	myCount = count;
}

void ZoneCutBuilder::SetExtendToBoundary(bool flag)
{
	myExtendToBoundary = flag;
}

void ZoneCutBuilder::SetCreateWireBody(bool flag)
{
	myCreateWireBody = flag;
}

const TopoDS_Shape& ZoneCutBuilder::Result() const
{
	return myResult;
}

const std::vector<TopoDS_Edge>& ZoneCutBuilder::GeneratedEdges() const
{
	return myGeneratedEdges;
}

void ZoneCutBuilder::ValidateInput() const
{
	if (myShape.IsNull())
	{
		throw std::runtime_error("ZoneCutBuilder: input shape is null.");
	}

	if (myInputEdges.empty())
	{
		throw std::runtime_error("ZoneCutBuilder: input edge list is empty.");
	}

	if (std::abs(myOffsetDistance) <= Precision::Confusion())
	{
		throw std::runtime_error("ZoneCutBuilder: offset distance is zero.");
	}

	if (myCount <= 0)
	{
		throw std::runtime_error("ZoneCutBuilder: offset count must be greater than zero.");
	}
}

void ZoneCutBuilder::Build()
{
	ValidateInput();

	myChains.clear();
	myGeneratedCurves.clear();
	myGeneratedEdges.clear();
	myResult.Nullify();

	CollectFaceEdgeChains();

	for (const FaceEdgeChain& chain : myChains)
	{
		if (chain.edges.empty())
		{
			continue;
		}

		std::vector<EdgeOnFace> preparedEdges;

		try
		{
			for (const ChainEdgeRef& ref : chain.edges)
			{
				preparedEdges.push_back(PrepareEdgeOnFace(chain.face, ref));
			}
		}
		catch (const std::exception&)
		{
			continue;
		}

		for (int k = 1; k <= myCount; ++k)
		{
			const double currentOffset = myOffsetDistance * static_cast<double>(k);

			std::vector<OffsetCurveOnFace> offsetCurves;

			try
			{
				for (const EdgeOnFace& eof : preparedEdges)
				{
					int side = DetectInnerOffsetSide(eof);

					OffsetCurveOnFace offsetCurve =
						BuildOffsetCurveOnFace(
							eof,
							currentOffset,
							side);

					offsetCurves.push_back(offsetCurve);
				}

				offsetCurves = ConnectOffsetCurves(chain.face, offsetCurves);

				if (myExtendToBoundary)
				{
					offsetCurves = ExtendChainToBoundary(
						chain.face,
						offsetCurves,
						chain.closed);
				}

				for (OffsetCurveOnFace& oc : offsetCurves)
				{
					if (oc.curve2d.IsNull())
					{
						continue;
					}

					oc.resultEdge = MakeEdgeOnFace(
						oc.face,
						oc.curve2d,
						oc.first,
						oc.last);

					if (!oc.resultEdge.IsNull())
					{
						myGeneratedCurves.push_back(oc);
						myGeneratedEdges.push_back(oc.resultEdge);
					}
				}
			}
			catch (const std::exception&)
			{
				break;
			}
		}
	}

	if (myGeneratedEdges.empty())
	{
		throw std::runtime_error(
			"ZoneCutBuilder: all offset attempts failed.");
	}

	if (myCreateWireBody)
	{
		myResult = BuildWireBodyResult();
	}
	else
	{
		myResult = BuildImprintedResult();
	}
}

void ZoneCutBuilder::CollectFaceEdgeChains()
{
	TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;

	TopExp::MapShapesAndAncestors(
		myShape,
		TopAbs_EDGE,
		TopAbs_FACE,
		edgeFaceMap);

	struct FaceGroup
	{
		TopoDS_Face face;
		std::vector<TopoDS_Edge> edges;
	};

	std::vector<FaceGroup> groups;

	for (const TopoDS_Edge& edge : myInputEdges)
	{
		if (edge.IsNull())
		{
			continue;
		}

		if (!edgeFaceMap.Contains(edge))
		{
			continue;
		}

		const TopTools_ListOfShape& faces = edgeFaceMap.FindFromKey(edge);

		for (TopTools_ListIteratorOfListOfShape it(faces); it.More(); it.Next())
		{
			TopoDS_Face face = TopoDS::Face(it.Value());

			bool found = false;

			for (FaceGroup& group : groups)
			{
				if (group.face.IsSame(face))
				{
					group.edges.push_back(edge);
					found = true;
					break;
				}
			}

			if (!found)
			{
				FaceGroup group;
				group.face = face;
				group.edges.push_back(edge);
				groups.push_back(group);
			}
		}
	}

	for (const FaceGroup& group : groups)
	{
		std::vector<FaceEdgeChain> chains =
			BuildConnectedChains(group.face, group.edges);

		for (const FaceEdgeChain& chain : chains)
		{
			myChains.push_back(chain);
		}
	}
}

std::vector<ZoneCutBuilder::FaceEdgeChain>
ZoneCutBuilder::BuildConnectedChains(
	const TopoDS_Face& face,
	const std::vector<TopoDS_Edge>& edges) const
{
	std::vector<FaceEdgeChain> result;

	std::vector<bool> used(edges.size(), false);

	for (std::size_t seed = 0; seed < edges.size(); ++seed)
	{
		if (used[seed])
		{
			continue;
		}

		FaceEdgeChain chain;
		chain.face = face;

		used[seed] = true;

		TopoDS_Vertex startV;
		TopoDS_Vertex endV;

		GetEdgeVertices(edges[seed], startV, endV);

		ChainEdgeRef seedRef;
		seedRef.edge = edges[seed];
		seedRef.reversed = false;

		chain.edges.push_back(seedRef);

		bool expanded = true;

		while (expanded)
		{
			expanded = false;

			for (std::size_t i = 0; i < edges.size(); ++i)
			{
				if (used[i])
				{
					continue;
				}

				TopoDS_Vertex a;
				TopoDS_Vertex b;
				GetEdgeVertices(edges[i], a, b);

				if (SameVertex(endV, a))
				{
					ChainEdgeRef ref;
					ref.edge = edges[i];
					ref.reversed = false;

					chain.edges.push_back(ref);
					endV = b;

					used[i] = true;
					expanded = true;
					break;
				}
				else if (SameVertex(endV, b))
				{
					ChainEdgeRef ref;
					ref.edge = edges[i];
					ref.reversed = true;

					chain.edges.push_back(ref);
					endV = a;

					used[i] = true;
					expanded = true;
					break;
				}
				else if (SameVertex(startV, b))
				{
					ChainEdgeRef ref;
					ref.edge = edges[i];
					ref.reversed = false;

					chain.edges.insert(chain.edges.begin(), ref);
					startV = a;

					used[i] = true;
					expanded = true;
					break;
				}
				else if (SameVertex(startV, a))
				{
					ChainEdgeRef ref;
					ref.edge = edges[i];
					ref.reversed = true;

					chain.edges.insert(chain.edges.begin(), ref);
					startV = b;

					used[i] = true;
					expanded = true;
					break;
				}
			}
		}

		chain.closed = SameVertex(startV, endV);

		result.push_back(chain);
	}

	return result;
}

ZoneCutBuilder::EdgeOnFace
ZoneCutBuilder::PrepareEdgeOnFace(
	const TopoDS_Face& face,
	const ChainEdgeRef& ref) const
{
	EdgeOnFace eof;
	eof.edge = ref.edge;
	eof.face = face;
	eof.reversed = ref.reversed;

	double first = 0.0;
	double last = 0.0;

	Handle(Geom2d_Curve) pc =
		BRep_Tool::CurveOnSurface(ref.edge, face, first, last);

	if (pc.IsNull())
	{
		throw std::runtime_error(
			"ZoneCutBuilder: failed to get pcurve of edge on face.");
	}

	Handle(Geom2d_TrimmedCurve) trimmed =
		new Geom2d_TrimmedCurve(pc, first, last);

	if (ref.reversed)
	{
		trimmed->Reverse();
	}

	eof.pcurve = trimmed;
	eof.first = trimmed->FirstParameter();
	eof.last = trimmed->LastParameter();

	return eof;
}

int ZoneCutBuilder::DetectInnerOffsetSide(
	const EdgeOnFace& edgeOnFace) const
{
	const double mid =
		0.5 * (edgeOnFace.first + edgeOnFace.last);

	gp_Pnt2d uv;
	gp_Vec2d tangent;

	edgeOnFace.pcurve->D1(mid, uv, tangent);

	if (tangent.Magnitude() <= Precision::Confusion())
	{
		return +1;
	}

	tangent.Normalize();

	gp_Vec2d left(-tangent.Y(), tangent.X());
	gp_Vec2d right(tangent.Y(), -tangent.X());

	const double eps = 1.0e-6;

	gp_Pnt2d leftPoint = uv.Translated(left * eps);
	gp_Pnt2d rightPoint = uv.Translated(right * eps);

	BRepTopAdaptor_FClass2d classifier(
		edgeOnFace.face,
		Precision::Confusion());

	TopAbs_State leftState =
		classifier.Perform(leftPoint, false);

	TopAbs_State rightState =
		classifier.Perform(rightPoint, false);

	if (leftState == TopAbs_IN || leftState == TopAbs_ON)
	{
		return +1;
	}

	if (rightState == TopAbs_IN || rightState == TopAbs_ON)
	{
		return -1;
	}

	return +1;
}

ZoneCutBuilder::OffsetCurveOnFace
ZoneCutBuilder::BuildOffsetCurveOnFace(
	const EdgeOnFace& edgeOnFace,
	double offsetDistance,
	int offsetSide) const
{
	OffsetCurveOnFace result;
	result.face = edgeOnFace.face;

	Handle(Geom_Surface) surface =
		BRep_Tool::Surface(edgeOnFace.face);

	if (surface.IsNull())
	{
		throw std::runtime_error(
			"ZoneCutBuilder: face surface is null.");
	}

	BRepTopAdaptor_FClass2d classifier(
		edgeOnFace.face,
		Precision::Confusion());

	const int sampleCount =
		EstimateSampleCount(edgeOnFace.edge);

	std::vector<gp_Pnt2d> uvPoints;
	uvPoints.reserve(sampleCount + 1);

	for (int i = 0; i <= sampleCount; ++i)
	{
		const double t =
			edgeOnFace.first +
			(edgeOnFace.last - edgeOnFace.first)
			* static_cast<double>(i)
			/ static_cast<double>(sampleCount);

		gp_Pnt2d uv;
		gp_Vec2d d2;

		edgeOnFace.pcurve->D1(t, uv, d2);

		if (d2.Magnitude() <= Precision::Confusion())
		{
			continue;
		}

		gp_Pnt p;
		gp_Vec dU;
		gp_Vec dV;

		surface->D1(
			uv.X(),
			uv.Y(),
			p,
			dU,
			dV);

		gp_Vec tangent3d =
			dU * d2.X() + dV * d2.Y();

		if (tangent3d.Magnitude() <= Precision::Confusion())
		{
			continue;
		}

		tangent3d.Normalize();

		gp_Vec normal =
			dU.Crossed(dV);

		if (normal.Magnitude() <= Precision::Confusion())
		{
			continue;
		}

		normal.Normalize();

		gp_Vec offsetDir =
			normal.Crossed(tangent3d);

		if (offsetDir.Magnitude() <= Precision::Confusion())
		{
			continue;
		}

		offsetDir.Normalize();

		if (offsetSide < 0)
		{
			offsetDir.Reverse();
		}

		gp_Pnt targetPoint =
			p.Translated(offsetDir * offsetDistance);

		GeomAPI_ProjectPointOnSurf projector(
			targetPoint,
			surface);

		if (projector.NbPoints() <= 0)
		{
			continue;
		}

		double u2 = 0.0;
		double v2 = 0.0;

		projector.LowerDistanceParameters(u2, v2);

		gp_Pnt2d uv2(u2, v2);

		TopAbs_State state =
			classifier.Perform(uv2, false);

		if (state == TopAbs_IN || state == TopAbs_ON)
		{
			uvPoints.push_back(uv2);
		}
	}

	if (uvPoints.size() < 2)
	{
		throw std::runtime_error(
			"ZoneCutBuilder: offset curve generation failed.");
	}

	TColgp_Array1OfPnt2d array(
		1,
		static_cast<Standard_Integer>(uvPoints.size()));

	for (std::size_t i = 0; i < uvPoints.size(); ++i)
	{
		array.SetValue(
			static_cast<Standard_Integer>(i + 1),
			uvPoints[i]);
	}

	Geom2dAPI_Interpolate interpolation(
		array,
		false,
		Precision::Confusion());

	interpolation.Perform();

	if (!interpolation.IsDone())
	{
		throw std::runtime_error(
			"ZoneCutBuilder: 2D offset curve interpolation failed.");
	}

	result.curve2d = interpolation.Curve();
	result.first = result.curve2d->FirstParameter();
	result.last = result.curve2d->LastParameter();

	return result;
}

std::vector<ZoneCutBuilder::OffsetCurveOnFace>
ZoneCutBuilder::ConnectOffsetCurves(
	const TopoDS_Face& face,
	const std::vector<OffsetCurveOnFace>& inputCurves) const
{
	if (inputCurves.size() <= 1)
	{
		return inputCurves;
	}

	const double gapTol = 1.0e-5;
	const std::size_t n = inputCurves.size();

	// Phase 1: compute tangent-ray intersections for all adjacent pairs
	struct Connection
	{
		bool valid = false;
		gp_Pnt2d point;
	};

	std::vector<Connection> connections(n - 1);

	for (std::size_t i = 0; i + 1 < n; ++i)
	{
		const OffsetCurveOnFace& curveA = inputCurves[i];
		const OffsetCurveOnFace& curveB = inputCurves[i + 1];

		gp_Pnt2d endA = CurvePoint2d(curveA.curve2d, curveA.last);
		gp_Pnt2d startB = CurvePoint2d(curveB.curve2d, curveB.first);

		if (Distance2d(endA, startB) <= gapTol)
		{
			continue;
		}

		gp_Pnt2d dummy;
		gp_Vec2d tanEndA;
		gp_Vec2d tanStartB;

		curveA.curve2d->D1(curveA.last, dummy, tanEndA);
		curveB.curve2d->D1(curveB.first, dummy, tanStartB);

		if (tanEndA.SquareMagnitude() <= Precision::SquareConfusion()
			|| tanStartB.SquareMagnitude() <= Precision::SquareConfusion())
		{
			continue;
		}

		tanEndA.Normalize();
		tanStartB.Normalize();

		const double cross = tanEndA.X() * tanStartB.Y()
			- tanEndA.Y() * tanStartB.X();

		if (std::abs(cross) < 1.0e-10)
		{
			continue; // parallel tangents — leave gap as-is
		}

		// Solve: endA + s * tanEndA = startB - t * tanStartB
		//        s * tanEndA.X + t * tanStartB.X = startB.X - endA.X
		//        s * tanEndA.Y + t * tanStartB.Y = startB.Y - endA.Y
		const double dx = startB.X() - endA.X();
		const double dy = startB.Y() - endA.Y();

		const double s = (dx * tanStartB.Y() - dy * tanStartB.X()) / cross;
		const double t = (tanEndA.X() * dy - tanEndA.Y() * dx) / cross;

		if (s < -gapTol || t < -gapTol)
		{
			continue; // intersection behind one of the curves
		}

		Connection conn;
		conn.valid = true;
		conn.point.SetX(endA.X() + std::max(0.0, s) * tanEndA.X());
		conn.point.SetY(endA.Y() + std::max(0.0, s) * tanEndA.Y());

		connections[i] = conn;
	}

	// Phase 2: extend / trim curves to intersection points
	std::vector<OffsetCurveOnFace> output;
	output.reserve(n);

	for (std::size_t i = 0; i < n; ++i)
	{
		OffsetCurveOnFace curve = inputCurves[i];

		// extend end to forward connection
		if (i < connections.size() && connections[i].valid)
		{
			curve = ExtendCurveToPoint(curve, connections[i].point, true);
		}

		// extend start to backward connection
		if (i > 0 && connections[i - 1].valid)
		{
			curve = ExtendCurveToPoint(curve, connections[i - 1].point, false);
		}

		output.push_back(curve);
	}

	return output;
}

std::vector<ZoneCutBuilder::OffsetCurveOnFace>
ZoneCutBuilder::ExtendChainToBoundary(
	const TopoDS_Face& face,
	const std::vector<OffsetCurveOnFace>& inputCurves,
	bool closed) const
{
	if (inputCurves.empty())
	{
		return inputCurves;
	}

	if (closed)
	{
		return inputCurves;
	}

	std::vector<OffsetCurveOnFace> output = inputCurves;

	const OffsetCurveOnFace& firstCurve = inputCurves.front();
	const OffsetCurveOnFace& lastCurve = inputCurves.back();

	gp_Pnt2d startBoundaryPoint;
	gp_Pnt2d endBoundaryPoint;

	bool hasStartExtension =
		FindExtensionPointOnBoundary(
			face,
			firstCurve.curve2d,
			firstCurve.first,
			false,
			startBoundaryPoint);

	bool hasEndExtension =
		FindExtensionPointOnBoundary(
			face,
			lastCurve.curve2d,
			lastCurve.last,
			true,
			endBoundaryPoint);

	if (hasStartExtension)
	{
		gp_Pnt2d startPoint =
			CurvePoint2d(
				firstCurve.curve2d,
				firstCurve.first);

		if (Distance2d(startBoundaryPoint, startPoint) > Precision::Confusion())
		{
			output.front() = ExtendCurveToPoint(
				output.front(), startBoundaryPoint, false);
		}
	}

	if (hasEndExtension)
	{
		gp_Pnt2d endPoint =
			CurvePoint2d(
				lastCurve.curve2d,
				lastCurve.last);

		if (Distance2d(endPoint, endBoundaryPoint) > Precision::Confusion())
		{
			output.back() = ExtendCurveToPoint(
				output.back(), endBoundaryPoint, true);
		}
	}

	return output;
}

ZoneCutBuilder::OffsetCurveOnFace
ZoneCutBuilder::ExtendCurveToPoint(
	const OffsetCurveOnFace& curve,
	const gp_Pnt2d& targetPoint,
	bool extendEnd) const
{
	if (curve.curve2d.IsNull())
	{
		return curve;
	}

	const double checkParam = extendEnd ? curve.last : curve.first;

	gp_Pnt2d currentPt;
	gp_Vec2d tangent;

	curve.curve2d->D1(checkParam, currentPt, tangent);

	if (Distance2d(currentPt, targetPoint) <= Precision::Confusion())
	{
		return curve;
	}

	// Sample original curve
	const int origSamples = std::max(32,
		static_cast<int>(
			std::abs(curve.last - curve.first) * 8.0));

	std::vector<gp_Pnt2d> points;
	points.reserve(origSamples + 16);

	for (int i = 0; i <= origSamples; ++i)
	{
		const double t = curve.first
			+ (curve.last - curve.first)
			* static_cast<double>(i) / static_cast<double>(origSamples);

		gp_Pnt2d p;
		curve.curve2d->D0(t, p);
		points.push_back(p);
	}

	// Generate extension points along the straight line to target
	// (target lies on the tangent ray by construction of the caller)
	const double extDist = currentPt.Distance(targetPoint);
	const int extCount = std::max(2, static_cast<int>(extDist * 16.0));

	if (extendEnd)
	{
		for (int i = 1; i <= extCount; ++i)
		{
			const double frac = static_cast<double>(i)
				/ static_cast<double>(extCount + 1);
			points.push_back(gp_Pnt2d(
				currentPt.X() + (targetPoint.X() - currentPt.X()) * frac,
				currentPt.Y() + (targetPoint.Y() - currentPt.Y()) * frac));
		}
		points.push_back(targetPoint);
	}
	else
	{
		std::vector<gp_Pnt2d> extPoints;
		extPoints.reserve(extCount + 2);

		extPoints.push_back(targetPoint);

		for (int i = extCount; i >= 1; --i)
		{
			const double frac = static_cast<double>(i)
				/ static_cast<double>(extCount + 1);
			extPoints.push_back(gp_Pnt2d(
				targetPoint.X() + (currentPt.X() - targetPoint.X()) * frac,
				targetPoint.Y() + (currentPt.Y() - targetPoint.Y()) * frac));
		}

		points.insert(points.begin(), extPoints.begin(), extPoints.end());
	}

	// Re-interpolate through the combined point set
	TColgp_Array1OfPnt2d array(
		1, static_cast<Standard_Integer>(points.size()));

	for (std::size_t i = 0; i < points.size(); ++i)
	{
		array.SetValue(
			static_cast<Standard_Integer>(i + 1),
			points[i]);
	}

	Geom2dAPI_Interpolate interpolation(
		array,
		false,
		Precision::Confusion());

	interpolation.Perform();

	if (!interpolation.IsDone())
	{
		return curve; // fallback: return unchanged
	}

	OffsetCurveOnFace result;
	result.face = curve.face;
	result.curve2d = interpolation.Curve();
	result.first = result.curve2d->FirstParameter();
	result.last = result.curve2d->LastParameter();

	return result;
}

bool ZoneCutBuilder::FindExtensionPointOnBoundary(
	const TopoDS_Face& face,
	const Handle(Geom2d_Curve)& curve,
	double parameter,
	bool forward,
	gp_Pnt2d& boundaryPoint) const
{
	if (curve.IsNull())
	{
		return false;
	}

	gp_Pnt2d p;
	gp_Vec2d tangent;

	curve->D1(parameter, p, tangent);

	if (tangent.Magnitude() <= Precision::Confusion())
	{
		return false;
	}

	tangent.Normalize();

	if (!forward)
	{
		tangent.Reverse();
	}

	gp_Dir2d dir(tangent);
	gp_Lin2d ray(p, dir);

	Handle(Geom2d_Line) rayCurve =
		new Geom2d_Line(ray);

	bool found = false;
	double bestDistance = std::numeric_limits<double>::max();
	gp_Pnt2d bestPoint;

	for (TopExp_Explorer wireExp(face, TopAbs_WIRE);
		wireExp.More();
		wireExp.Next())
	{
		TopoDS_Wire wire =
			TopoDS::Wire(wireExp.Current());

		for (TopExp_Explorer edgeExp(wire, TopAbs_EDGE);
			edgeExp.More();
			edgeExp.Next())
		{
			TopoDS_Edge boundaryEdge =
				TopoDS::Edge(edgeExp.Current());

			double bf = 0.0;
			double bl = 0.0;

			Handle(Geom2d_Curve) boundaryPCurve =
				BRep_Tool::CurveOnSurface(
					boundaryEdge,
					face,
					bf,
					bl);

			if (boundaryPCurve.IsNull())
			{
				continue;
			}

			Handle(Geom2d_TrimmedCurve) trimmedBoundary =
				new Geom2d_TrimmedCurve(
					boundaryPCurve,
					bf,
					bl);

			Geom2dAPI_InterCurveCurve intersector(
				rayCurve,
				trimmedBoundary,
				Precision::Confusion());

			const int nbPoints = intersector.NbPoints();

			for (int i = 1; i <= nbPoints; ++i)
			{
				gp_Pnt2d q =
					intersector.Point(i);

				gp_Vec2d pq(p, q);

				if (pq.Magnitude() <= Precision::Confusion())
				{
					continue;
				}

				gp_Vec2d pqNorm = pq;
				pqNorm.Normalize();

				const double dot =
					pqNorm.Dot(tangent);

				if (dot <= 0.0)
				{
					continue;
				}

				const double dist =
					p.Distance(q);

				if (dist < bestDistance)
				{
					bestDistance = dist;
					bestPoint = q;
					found = true;
				}
			}
		}
	}

	if (found)
	{
		boundaryPoint = bestPoint;
	}

	return found;
}

TopoDS_Edge ZoneCutBuilder::MakeEdgeOnFace(
	const TopoDS_Face& face,
	const Handle(Geom2d_Curve)& curve2d,
	double first,
	double last) const
{
	if (curve2d.IsNull())
	{
		return TopoDS_Edge();
	}

	Handle(Geom_Surface) surface =
		BRep_Tool::Surface(face);

	if (surface.IsNull())
	{
		return TopoDS_Edge();
	}

	BRepBuilderAPI_MakeEdge edgeMaker(
		curve2d,
		surface,
		first,
		last);

	if (!edgeMaker.IsDone())
	{
		return TopoDS_Edge();
	}

	TopoDS_Edge edge = edgeMaker.Edge();

	BRepLib::BuildCurve3d(edge);
	BRepLib::SameParameter(edge, Precision::Confusion(), Standard_True);

	return edge;
}

TopoDS_Shape ZoneCutBuilder::BuildWireBodyResult() const
{
	BRep_Builder builder;
	TopoDS_Compound compound;

	builder.MakeCompound(compound);

	for (const TopoDS_Edge& edge : myGeneratedEdges)
	{
		if (!edge.IsNull())
		{
			builder.Add(compound, edge);
		}
	}

	return compound;
}

TopoDS_Shape ZoneCutBuilder::BuildImprintedResult() const
{
	BRepFeat_SplitShape splitter(myShape);

	for (const OffsetCurveOnFace& oc : myGeneratedCurves)
	{
		if (oc.resultEdge.IsNull() || oc.face.IsNull())
		{
			continue;
		}

		splitter.Add(oc.resultEdge, oc.face);
	}

	splitter.Build();

	if (!splitter.IsDone())
	{
		throw std::runtime_error(
			"ZoneCutBuilder: imprint failed in BRepFeat_SplitShape.");
	}

	return splitter.Shape();
}

bool ZoneCutBuilder::SameVertex(
	const TopoDS_Vertex& a,
	const TopoDS_Vertex& b)
{
	if (a.IsNull() || b.IsNull())
	{
		return false;
	}

	return a.IsSame(b);
}

void ZoneCutBuilder::GetEdgeVertices(
	const TopoDS_Edge& edge,
	TopoDS_Vertex& v1,
	TopoDS_Vertex& v2)
{
	TopExp::Vertices(edge, v1, v2, Standard_False);
}

gp_Pnt2d ZoneCutBuilder::CurvePoint2d(
	const Handle(Geom2d_Curve)& curve,
	double parameter)
{
	gp_Pnt2d p;
	curve->D0(parameter, p);
	return p;
}

double ZoneCutBuilder::Distance2d(
	const gp_Pnt2d& a,
	const gp_Pnt2d& b)
{
	return a.Distance(b);
}

int ZoneCutBuilder::EstimateSampleCount(
	const TopoDS_Edge& edge)
{
	double first = 0.0;
	double last = 0.0;

	Handle(Geom_Curve) curve3d =
		BRep_Tool::Curve(edge, first, last);

	if (curve3d.IsNull())
	{
		return 32;
	}

	const double span = std::abs(last - first);

	int sampleCount = static_cast<int>(span * 8.0);

	if (sampleCount < 32)
	{
		sampleCount = 32;
	}

	if (sampleCount > 256)
	{
		sampleCount = 256;
	}

	return sampleCount;
}

// todo
/*1. 偏移曲线自交检测
2. 偏移曲线与 Face 内孔边界的裁剪
3. 曲面周期参数处理，例如圆柱面、环面
4. [DONE] 相邻偏移 Edge 使用切线延伸求交点修剪，替代桥接连接段
5. 大偏移距离失败回退策略
6. Plane Face 使用精确 2D offset，提高速度和精度
7. 对 BRepFeat_SplitShape 失败时 fallback 到 BOPAlgo_Splitter
8. 对输入分叉 Edge 链做更严格的拓扑检查*/