#include "fillet/ImprovedFilletRecognizer.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomLProp_SLProps.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_CanonicalRecognition.hxx>
#include <ShapeAnalysis_ShapeTolerance.hxx>
#include <ShapeAnalysis_Surface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Sphere.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fillet::improved {
namespace {

struct Classified {
  bool recognized = false;
  GeometrySource source = GeometrySource::AnalyticCylinder;
  double radius = 0.0;
  double gap = 0.0;
  double minimumRadius = 0.0;
  double maximumRadius = 0.0;
  bool variableRadius = false;
  bool doubleCurved = false;
  Convexity convexity = Convexity::Unknown;
  double angularCoverage = 0.0;
  double secondaryCurvatureRatio = 0.0;
  double sameSignCurvatureFraction = 0.0;
  std::vector<RadiusSampleEvidence> radiusSamples;
  RadiusTraceMethod radiusTraceMethod = RadiusTraceMethod::None;
  double radiusTraceCoverage = 0.0;
  double radiusTraceMinimumTangentAlignment = -1.0;
  double radiusTraceNormalizedLength = 0.0;
  double radiusTraceSeedU = 0.0;
  double radiusTraceSeedV = 0.0;
  int radiusTraceSeedAttempts = 0;
  int radiusTraceAdaptiveStepReductions = 0;
  double radiusTraceBestStreamlineCoverage = 0.0;
  double radiusTraceBestStreamlineNormalizedLength = 0.0;
  int radiusTraceBestStreamlineSampleCount = 0;
  int radiusTraceTotalAttemptedStepReductions = 0;
  int radiusTraceValidSeedCount = 0;
  double radiusTraceMaximumCrossSeedRelativeDeviation = 0.0;
  bool radiusTraceStableAcrossSeeds = false;
};

std::int64_t quantize(double value, double quantum) {
  if (!std::isfinite(value)) return 0;
  const long double scaled = static_cast<long double>(value) /
                             std::max(quantum, 1.0e-15);
  const long double low = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
  const long double high = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
  return static_cast<std::int64_t>(std::llround(std::max(low, std::min(high, scaled))));
}

void hashInteger(std::uint64_t& hash, std::int64_t value) {
  constexpr std::uint64_t prime = 1099511628211ULL;
  const auto bits = static_cast<std::uint64_t>(value);
  for (int byte = 0; byte < 8; ++byte) {
    hash ^= (bits >> (byte * 8)) & 0xffULL;
    hash *= prime;
  }
}

std::string geometryFingerprint(const TopoDS_Face& face, const Classified& classified,
                                double modelDiagonal, double linearTolerance) {
  Bnd_Box box;
  BRepBndLib::AddOptimal(face, box, Standard_False, Standard_False);
  std::array<double, 6> bounds{};
  if (!box.IsVoid()) box.Get(bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5]);
  GProp_GProps properties;
  BRepGProp::SurfaceProperties(face, properties);
  const gp_Pnt centre = properties.CentreOfMass();
  const double lengthQuantum = std::max(linearTolerance, modelDiagonal * 1.0e-9);
  const double areaQuantum = std::max(lengthQuantum * lengthQuantum, 1.0e-18);
  std::uint64_t hash = 1469598103934665603ULL;
  for (double value : bounds) hashInteger(hash, quantize(value, lengthQuantum));
  hashInteger(hash, quantize(centre.X(), lengthQuantum));
  hashInteger(hash, quantize(centre.Y(), lengthQuantum));
  hashInteger(hash, quantize(centre.Z(), lengthQuantum));
  hashInteger(hash, quantize(properties.Mass(), areaQuantum));
  hashInteger(hash, quantize(classified.radius, lengthQuantum));
  hashInteger(hash, classified.recognized ? static_cast<int>(classified.source) + 1 : 0);
  hashInteger(hash, static_cast<int>(face.Orientation()));
  std::ostringstream stream;
  stream << "face-" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return stream.str();
}

bool isG1(GeomAbs_Shape value) {
  return value == GeomAbs_G1 || value == GeomAbs_C1 || value == GeomAbs_G2 ||
         value == GeomAbs_C2 || value == GeomAbs_C3 || value == GeomAbs_CN;
}

bool normalsAreTangent(const TopoDS_Edge& edge, const TopoDS_Face& a,
                       const TopoDS_Face& b, double angleTolerance,
                       double linearTolerance) {
  try {
    if (isG1(BRep_Tool::Continuity(edge, a, b))) return true;
  } catch (const Standard_Failure&) {}
  try {
    Standard_Real fa = 0.0, la = 0.0, fb = 0.0, lb = 0.0;
    const Handle(Geom2d_Curve) ca = BRep_Tool::CurveOnSurface(edge, a, fa, la);
    const Handle(Geom2d_Curve) cb = BRep_Tool::CurveOnSurface(edge, b, fb, lb);
    if (ca.IsNull() || cb.IsNull()) return false;
    BRepAdaptor_Surface sa(a, Standard_True), sb(b, Standard_True);
    const double cosineLimit = std::cos(angleTolerance);
    int valid = 0;
    constexpr std::array<double, 5> ratios{0.1, 0.3, 0.5, 0.7, 0.9};
    for (double ratio : ratios) {
      const gp_Pnt2d uva = ca->Value(fa + ratio * (la - fa));
      const gp_Pnt2d uvb = cb->Value(fb + ratio * (lb - fb));
      BRepLProp_SLProps pa(sa, uva.X(), uva.Y(), 1, linearTolerance);
      BRepLProp_SLProps pb(sb, uvb.X(), uvb.Y(), 1, linearTolerance);
      if (!pa.IsNormalDefined() || !pb.IsNormalDefined()) continue;
      if (std::abs(pa.Normal().Dot(pb.Normal())) < cosineLimit) return false;
      ++valid;
    }
    return valid >= 2;
  } catch (const Standard_Failure&) {
    return false;
  }
}

struct DirectionStatistics {
  int requested = 0;
  int valid = 0;
  double coverage = 0.0;
  double minimum = -1.0;
  double mean = -1.0;
  double standardDeviation = 0.0;
  std::vector<double> alignments;
  std::vector<double> projectionGaps;
  std::vector<DirectionSampleEvidence> samples;
};

DirectionStatistics summarizeAlignments(const std::vector<double>& alignments,
                                        int requested) {
  DirectionStatistics statistics;
  statistics.requested = requested;
  statistics.valid = static_cast<int>(alignments.size());
  statistics.coverage = requested > 0
      ? static_cast<double>(statistics.valid) / requested : 0.0;
  if (alignments.empty()) return statistics;
  statistics.minimum = *std::min_element(alignments.begin(), alignments.end());
  statistics.mean = std::accumulate(alignments.begin(), alignments.end(), 0.0) /
                    alignments.size();
  double variance = 0.0;
  for (double alignment : alignments)
    variance += (alignment - statistics.mean) * (alignment - statistics.mean);
  statistics.standardDeviation = std::sqrt(variance / alignments.size());
  statistics.alignments = alignments;
  return statistics;
}

struct PrincipalDirectionEvidence {
  gp_Dir direction;
  double secondaryCurvatureRatio = 0.0;
};

std::optional<PrincipalDirectionEvidence> principalSpineDirection(
    BRepLProp_SLProps& properties, double maximumUmbilicRatio) {
  if (!properties.IsCurvatureDefined()) return std::nullopt;
  const double maximum = std::abs(properties.MaxCurvature());
  const double minimum = std::abs(properties.MinCurvature());
  const double dominant = std::max(maximum, minimum);
  const double curvatureRatio = dominant > 1.0e-14
      ? std::min(maximum, minimum) / dominant : 1.0;
  if (dominant <= 1.0e-14 || curvatureRatio >= maximumUmbilicRatio)
    return std::nullopt;
  gp_Dir maxDirection, minDirection;
  properties.CurvatureDirections(maxDirection, minDirection);
  return PrincipalDirectionEvidence{
      maximum < minimum ? maxDirection : minDirection, curvatureRatio};
}

DirectionStatistics spineDirectionStatistics(const TopoDS_Edge& edge,
                                              const TopoDS_Face& a,
                                              const TopoDS_Face& b,
                                              double linearTolerance,
                                              const Options& options) {
  DirectionStatistics statistics;
  statistics.requested = std::max(3, options.spineDirectionSampleCount);
  try {
    Standard_Real fa = 0.0, la = 0.0, fb = 0.0, lb = 0.0;
    const Handle(Geom2d_Curve) ca = BRep_Tool::CurveOnSurface(edge, a, fa, la);
    const Handle(Geom2d_Curve) cb = BRep_Tool::CurveOnSurface(edge, b, fb, lb);
    if (ca.IsNull() || cb.IsNull()) return statistics;
    BRepAdaptor_Surface sa(a, Standard_True), sb(b, Standard_True);
    std::vector<double> alignments;
    std::vector<DirectionSampleEvidence> samples;
    for (int sample = 0; sample < statistics.requested; ++sample) {
      const double ratio = static_cast<double>(sample + 1) / (statistics.requested + 1);
      const gp_Pnt2d uva = ca->Value(fa + ratio * (la - fa));
      const gp_Pnt2d uvb = cb->Value(fb + ratio * (lb - fb));
      BRepLProp_SLProps pa(sa, uva.X(), uva.Y(), 2, linearTolerance);
      BRepLProp_SLProps pb(sb, uvb.X(), uvb.Y(), 2, linearTolerance);
      const auto spineA = principalSpineDirection(
          pa, options.maximumUmbilicCurvatureRatioForDirection);
      const auto spineB = principalSpineDirection(
          pb, options.maximumUmbilicCurvatureRatioForDirection);
      if (!spineA || !spineB) continue;
      DirectionSampleEvidence evidence;
      const gp_Pnt pointA = sa.Value(uva.X(), uva.Y());
      const gp_Pnt pointB = sb.Value(uvb.X(), uvb.Y());
      evidence.firstX = pointA.X(); evidence.firstY = pointA.Y(); evidence.firstZ = pointA.Z();
      evidence.secondX = pointB.X(); evidence.secondY = pointB.Y(); evidence.secondZ = pointB.Z();
      evidence.firstU = uva.X(); evidence.firstV = uva.Y();
      evidence.secondU = uvb.X(); evidence.secondV = uvb.Y();
      evidence.gap = pointA.Distance(pointB);
      evidence.alignment = std::abs(spineA->direction.Dot(spineB->direction));
      evidence.firstSecondaryCurvatureRatio = spineA->secondaryCurvatureRatio;
      evidence.secondSecondaryCurvatureRatio = spineB->secondaryCurvatureRatio;
      evidence.firstDirectionX = spineA->direction.X();
      evidence.firstDirectionY = spineA->direction.Y();
      evidence.firstDirectionZ = spineA->direction.Z();
      evidence.secondDirectionX = spineB->direction.X();
      evidence.secondDirectionY = spineB->direction.Y();
      evidence.secondDirectionZ = spineB->direction.Z();
      alignments.push_back(evidence.alignment);
      samples.push_back(evidence);
    }
    DirectionStatistics summarized = summarizeAlignments(alignments, statistics.requested);
    summarized.samples = std::move(samples);
    return summarized;
  } catch (const Standard_Failure&) {}
  return statistics;
}

struct ProjectedDirectionSample {
  gp_Pnt2d uvA;
  gp_Pnt2d uvB;
  double gap = 0.0;
};

DirectionStatistics projectedSpineDirectionStatistics(
    const TopoDS_Face& a, const TopoDS_Face& b, double linearTolerance,
    const Options& options) {
  const int requested = std::max(3, options.spineDirectionSampleCount);
  try {
    BRepAdaptor_Surface adaptorA(a, Standard_True), adaptorB(b, Standard_True);
    const Handle(Geom_Surface) surfaceA = BRep_Tool::Surface(a);
    const Handle(Geom_Surface) surfaceB = BRep_Tool::Surface(b);
    if (surfaceA.IsNull() || surfaceB.IsNull()) {
      DirectionStatistics statistics;
      statistics.requested = requested;
      return statistics;
    }
    ShapeAnalysis_Surface analysisA(surfaceA), analysisB(surfaceB);
    const double gapTolerance = linearTolerance * options.geometricSupportGapFactor;
    std::vector<ProjectedDirectionSample> candidates;
    int attempts = 0;
    const auto collect = [&](const TopoDS_Face& source,
                             BRepAdaptor_Surface& sourceAdaptor,
                             const TopoDS_Face& target,
                             ShapeAnalysis_Surface& targetAnalysis,
                             bool sourceIsA) {
      for (TopExp_Explorer explorer(source, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
        if (BRep_Tool::Degenerated(edge)) continue;
        Standard_Real first = 0.0, last = 0.0;
        const Handle(Geom2d_Curve) pcurve =
            BRep_Tool::CurveOnSurface(edge, source, first, last);
        if (pcurve.IsNull() || !std::isfinite(first) || !std::isfinite(last)) continue;
        for (int sample = 0; sample < requested; ++sample) {
          if (++attempts > options.maximumProjectedDirectionAttempts) return;
          const double ratio = static_cast<double>(sample + 1) / (requested + 1);
          const gp_Pnt2d uvSource = pcurve->Value(first + ratio * (last - first));
          const gp_Pnt point = sourceAdaptor.Value(uvSource.X(), uvSource.Y());
          const gp_Pnt2d uvTarget = targetAnalysis.ValueOfUV(point, gapTolerance);
          if (targetAnalysis.Gap() > gapTolerance) continue;
          BRepClass_FaceClassifier classifier(target, uvTarget, gapTolerance);
          if (classifier.State() != TopAbs_IN && classifier.State() != TopAbs_ON) continue;
          ProjectedDirectionSample projected;
          projected.uvA = sourceIsA ? uvSource : uvTarget;
          projected.uvB = sourceIsA ? uvTarget : uvSource;
          projected.gap = targetAnalysis.Gap();
          candidates.push_back(projected);
        }
      }
    };
    collect(a, adaptorA, b, analysisB, true);
    if (attempts <= options.maximumProjectedDirectionAttempts)
      collect(b, adaptorB, a, analysisA, false);
    std::sort(candidates.begin(), candidates.end(),
              [](const ProjectedDirectionSample& left,
                 const ProjectedDirectionSample& right) { return left.gap < right.gap; });
    std::vector<double> alignments;
    std::vector<double> projectionGaps;
    std::vector<DirectionSampleEvidence> samples;
    for (const auto& candidate : candidates) {
      BRepLProp_SLProps propertiesA(adaptorA, candidate.uvA.X(), candidate.uvA.Y(), 2,
                                    linearTolerance);
      BRepLProp_SLProps propertiesB(adaptorB, candidate.uvB.X(), candidate.uvB.Y(), 2,
                                    linearTolerance);
      const auto spineA = principalSpineDirection(
          propertiesA, options.maximumUmbilicCurvatureRatioForDirection);
      const auto spineB = principalSpineDirection(
          propertiesB, options.maximumUmbilicCurvatureRatioForDirection);
      if (!spineA || !spineB) continue;
      DirectionSampleEvidence evidence;
      const gp_Pnt pointA = adaptorA.Value(candidate.uvA.X(), candidate.uvA.Y());
      const gp_Pnt pointB = adaptorB.Value(candidate.uvB.X(), candidate.uvB.Y());
      evidence.firstX = pointA.X(); evidence.firstY = pointA.Y(); evidence.firstZ = pointA.Z();
      evidence.secondX = pointB.X(); evidence.secondY = pointB.Y(); evidence.secondZ = pointB.Z();
      evidence.firstU = candidate.uvA.X(); evidence.firstV = candidate.uvA.Y();
      evidence.secondU = candidate.uvB.X(); evidence.secondV = candidate.uvB.Y();
      evidence.gap = candidate.gap;
      evidence.alignment = std::abs(spineA->direction.Dot(spineB->direction));
      evidence.firstSecondaryCurvatureRatio = spineA->secondaryCurvatureRatio;
      evidence.secondSecondaryCurvatureRatio = spineB->secondaryCurvatureRatio;
      evidence.firstDirectionX = spineA->direction.X();
      evidence.firstDirectionY = spineA->direction.Y();
      evidence.firstDirectionZ = spineA->direction.Z();
      evidence.secondDirectionX = spineB->direction.X();
      evidence.secondDirectionY = spineB->direction.Y();
      evidence.secondDirectionZ = spineB->direction.Z();
      alignments.push_back(evidence.alignment);
      projectionGaps.push_back(candidate.gap);
      samples.push_back(evidence);
      if (static_cast<int>(alignments.size()) >= requested) break;
    }
    DirectionStatistics statistics = summarizeAlignments(alignments, requested);
    statistics.projectionGaps = std::move(projectionGaps);
    statistics.samples = std::move(samples);
    return statistics;
  } catch (const Standard_Failure&) {
    DirectionStatistics statistics;
    statistics.requested = requested;
    return statistics;
  }
}

void assignDirectionStatistics(Link& link, const DirectionStatistics& direction,
                               DirectionEvidenceSource source,
                               const Options& options) {
  link.directionEvidenceSource = source;
  link.spineDirectionRequestedSamples = direction.requested;
  link.spineDirectionValidSamples = direction.valid;
  link.spineDirectionSampleCoverage = direction.coverage;
  link.minimumSpineDirectionAlignment = direction.minimum;
  link.spineDirectionAlignment = direction.mean;
  link.spineDirectionAlignmentStandardDeviation = direction.standardDeviation;
  link.spineDirectionAlignmentSamples = direction.alignments;
  link.spineDirectionProjectionGapSamples = direction.projectionGaps;
  link.directionSamples = direction.samples;
  if (direction.coverage >= options.minimumSpineDirectionSampleCoverage) {
    link.spineDirectionEvaluated = true;
    link.spineDirectionCompatible =
        direction.minimum >= options.minimumSpineDirectionAlignment;
  }
}

DirectionStatistics mergeDirectionStatistics(const DirectionStatistics& initial,
                                             const DirectionStatistics& refined) {
  std::vector<double> alignments = initial.alignments;
  alignments.insert(alignments.end(), refined.alignments.begin(), refined.alignments.end());
  DirectionStatistics merged = summarizeAlignments(
      alignments, initial.requested + refined.requested);
  merged.projectionGaps = initial.projectionGaps;
  merged.projectionGaps.insert(merged.projectionGaps.end(), refined.projectionGaps.begin(),
                               refined.projectionGaps.end());
  merged.samples = initial.samples;
  merged.samples.insert(merged.samples.end(), refined.samples.begin(), refined.samples.end());
  return merged;
}

Diagnostics diagnose(const TopoDS_Shape& shape, const Options& options) {
  Diagnostics result;
  TopTools_IndexedMapOfShape faces, edges;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  result.faceCount = faces.Extent();
  result.edgeCount = edges.Extent();
  for (int i = 1; i <= edges.Extent(); ++i)
    if (BRep_Tool::Degenerated(TopoDS::Edge(edges.FindKey(i)))) ++result.degeneratedEdgeCount;

  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  for (int i = 1; i <= edgeFaces.Extent(); ++i) {
    std::set<int> owners;
    for (TopTools_ListIteratorOfListOfShape it(edgeFaces.FindFromIndex(i)); it.More(); it.Next())
      owners.insert(faces.FindIndex(it.Value()));
    if (owners.size() > 2) ++result.nonManifoldEdgeCount;
  }

  Bnd_Box box;
  BRepBndLib::AddOptimal(shape, box, Standard_False, Standard_False);
  if (!box.IsVoid()) {
    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    result.modelDiagonal = gp_Pnt(xmin, ymin, zmin).Distance(gp_Pnt(xmax, ymax, zmax));
  }
  if (result.modelDiagonal > 0.0) {
    const double minimumEdgeLength =
        result.modelDiagonal * options.minimumResolvedEdgeLengthToModelDiagonal;
    for (int i = 1; i <= edges.Extent(); ++i) {
      const TopoDS_Edge edge = TopoDS::Edge(edges.FindKey(i));
      if (BRep_Tool::Degenerated(edge)) continue;
      GProp_GProps properties;
      BRepGProp::LinearProperties(edge, properties);
      if (properties.Mass() < minimumEdgeLength) ++result.shortEdgeCount;
    }
    const double minimumFaceArea = result.modelDiagonal * result.modelDiagonal *
                                   options.minimumResolvedFaceAreaToModelDiagonalSquared;
    for (int i = 1; i <= faces.Extent(); ++i) {
      GProp_GProps properties;
      BRepGProp::SurfaceProperties(TopoDS::Face(faces.FindKey(i)), properties);
      if (properties.Mass() < minimumFaceArea) ++result.sliverFaceCount;
    }
  }
  ShapeAnalysis_ShapeTolerance tolerances;
  result.minimumTolerance = tolerances.Tolerance(shape, -1);
  result.averageTolerance = tolerances.Tolerance(shape, 0);
  result.maximumTolerance = tolerances.Tolerance(shape, 1);
  result.effectiveLinearTolerance = std::max({
      options.absoluteTolerance,
      result.modelDiagonal * options.relativeTolerance,
      result.maximumTolerance});
  try { result.valid = BRepCheck_Analyzer(shape, Standard_True, Standard_True).IsValid(); }
  catch (const Standard_Failure&) { result.valid = false; }
  return result;
}

Classified curvatureClassify(const TopoDS_Face& face, const Options& options,
                             double linearTolerance) {
  Classified result;
  try {
    BRepAdaptor_Surface surface(face, Standard_True);
    Standard_Real umin, umax, vmin, vmax;
    BRepTools::UVBounds(face, umin, umax, vmin, vmax);
    if (!std::isfinite(umin) || !std::isfinite(umax) ||
        !std::isfinite(vmin) || !std::isfinite(vmax) || umin >= umax || vmin >= vmax)
      return result;
    std::vector<double> radii;
    std::vector<double> signedDominant;
    std::vector<double> secondaryRatios;
    struct TraceCandidate {
      RadiusSampleEvidence evidence;
      int uIndex = 0;
      int vIndex = 0;
      bool followsU = false;
    };
    std::vector<TraceCandidate> traceCandidates;
    int sameSignCurvatureSamples = 0;
    const int grid = std::max(3, options.curvatureGridSize);
    for (int iu = 0; iu < grid; ++iu) for (int iv = 0; iv < grid; ++iv) {
      const double u = umin + (iu + 0.5) * (umax - umin) / grid;
      const double v = vmin + (iv + 0.5) * (vmax - vmin) / grid;
      BRepClass_FaceClassifier classifier(face, gp_Pnt2d(u, v), linearTolerance);
      if (classifier.State() != TopAbs_IN && classifier.State() != TopAbs_ON) continue;
      BRepLProp_SLProps props(surface, u, v, 2, linearTolerance);
      if (!props.IsCurvatureDefined()) continue;
      const double k1 = props.MaxCurvature(), k2 = props.MinCurvature();
      const double dominant = std::abs(k1) >= std::abs(k2) ? k1 : k2;
      const double secondary = std::abs(k1) >= std::abs(k2) ? k2 : k1;
      if (!std::isfinite(dominant) || std::abs(dominant) <= 1.0e-14) continue;
      radii.push_back(1.0 / std::abs(dominant));
      signedDominant.push_back(dominant);
      secondaryRatios.push_back(std::abs(secondary / dominant));
      if (k1 * k2 > 0.0) ++sameSignCurvatureSamples;
      const auto spine = principalSpineDirection(
          props, options.maximumUmbilicCurvatureRatioForDirection);
      if (spine) {
        gp_Pnt point;
        gp_Vec derivativeU, derivativeV;
        surface.D1(u, v, point, derivativeU, derivativeV);
        if (derivativeU.SquareMagnitude() > 1.0e-28 &&
            derivativeV.SquareMagnitude() > 1.0e-28) {
          RadiusSampleEvidence evidence;
          evidence.u = u; evidence.v = v;
          evidence.x = point.X(); evidence.y = point.Y(); evidence.z = point.Z();
          evidence.radius = radii.back();
          evidence.secondaryCurvatureRatio = std::abs(secondary / dominant);
          evidence.spineDirectionX = spine->direction.X();
          evidence.spineDirectionY = spine->direction.Y();
          evidence.spineDirectionZ = spine->direction.Z();
          const double uAlignment = std::abs(spine->direction.Dot(gp_Dir(derivativeU)));
          const double vAlignment = std::abs(spine->direction.Dot(gp_Dir(derivativeV)));
          traceCandidates.push_back({evidence, iu, iv, uAlignment >= vAlignment});
        }
      }
    }
    if (static_cast<int>(radii.size()) < options.minimumCurvatureSamples) return result;
    std::sort(radii.begin(), radii.end());
    const double median = radii[radii.size() / 2];
    const double mean = std::accumulate(radii.begin(), radii.end(), 0.0) / radii.size();
    double variance = 0.0;
    for (double value : radii) variance += (value - mean) * (value - mean);
    const double coefficient = std::sqrt(variance / radii.size()) / std::max(mean, 1.0e-14);
    std::sort(secondaryRatios.begin(), secondaryRatios.end());
    const double secondaryMedian = secondaryRatios[secondaryRatios.size() / 2];
    const double sameSignFraction = static_cast<double>(sameSignCurvatureSamples) /
                                    secondaryRatios.size();
    const bool bandLike = secondaryMedian <= options.maximumSecondaryCurvatureRatio;
    const bool cornerLike = options.enableDoubleCurvedCornerCandidates &&
        secondaryMedian >= options.minimumDoubleCurvatureRatio &&
        sameSignFraction >= options.minimumSameSignCurvatureFraction;
    if (coefficient > options.maximumRadiusCoefficientOfVariation ||
        (!bandLike && !cornerLike)) return result;
    const double signedMean = std::accumulate(signedDominant.begin(), signedDominant.end(), 0.0) /
                              signedDominant.size();
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    const double orientedSign = reversed ? -signedMean : signedMean;
    result.recognized = true;
    result.source = GeometrySource::CurvatureField;
    result.radius = median;
    result.minimumRadius = radii.front();
    result.maximumRadius = radii.back();
    result.variableRadius = coefficient > 0.05;
    result.doubleCurved = cornerLike;
    result.secondaryCurvatureRatio = secondaryMedian;
    result.sameSignCurvatureFraction = sameSignFraction;
    result.convexity = orientedSign < 0.0 ? Convexity::Convex : Convexity::Concave;
    if (bandLike && !traceCandidates.empty()) {
      const int followsUVotes = static_cast<int>(std::count_if(
          traceCandidates.begin(), traceCandidates.end(),
          [](const TraceCandidate& sample) { return sample.followsU; }));
      const bool followsU = followsUVotes * 2 >= static_cast<int>(traceCandidates.size());
      std::map<int, std::vector<TraceCandidate>> rows;
      for (const auto& candidate : traceCandidates)
        rows[followsU ? candidate.vIndex : candidate.uIndex].push_back(candidate);
      const int centre = grid / 2;
      const auto best = std::max_element(rows.begin(), rows.end(),
          [centre](const auto& first, const auto& second) {
            if (first.second.size() != second.second.size())
              return first.second.size() < second.second.size();
            return std::abs(first.first - centre) > std::abs(second.first - centre);
          });
      if (best != rows.end() &&
          static_cast<int>(best->second.size()) >= options.minimumRadiusTraceSamples) {
        const double transverseParameter = followsU
            ? best->second.front().evidence.v : best->second.front().evidence.u;
        const int requested = std::max(options.minimumRadiusTraceSamples,
                                       options.radiusTraceSampleCount);
        const auto seedCandidate = std::min_element(
            best->second.begin(), best->second.end(), [centre, followsU](const auto& first,
                                                                        const auto& second) {
              const int firstIndex = followsU ? first.uIndex : first.vIndex;
              const int secondIndex = followsU ? second.uIndex : second.vIndex;
              return std::abs(firstIndex - centre) < std::abs(secondIndex - centre);
            });
        struct FlowState {
          RadiusSampleEvidence evidence;
          gp_Dir direction{1.0, 0.0, 0.0};
          double uStep = 0.0;
          double vStep = 0.0;
        };
        const double uRange = umax - umin, vRange = vmax - vmin;
        const double traceProgressTolerance = std::max(
            Precision::Confusion(), linearTolerance * 1.0e-4);
        const auto evaluateFlow = [&](double u, double v,
                                      const std::optional<gp_Dir>& reference)
            -> std::optional<FlowState> {
          if (u < umin || u > umax || v < vmin || v > vmax) return std::nullopt;
          BRepClass_FaceClassifier classifier(face, gp_Pnt2d(u, v), linearTolerance);
          if (classifier.State() != TopAbs_IN && classifier.State() != TopAbs_ON)
            return std::nullopt;
          BRepLProp_SLProps properties(surface, u, v, 2, linearTolerance);
          if (!properties.IsCurvatureDefined()) return std::nullopt;
          const auto spine = principalSpineDirection(
              properties, options.maximumUmbilicCurvatureRatioForDirection);
          if (!spine) return std::nullopt;
          gp_Dir direction = spine->direction;
          if (reference && direction.Dot(*reference) < 0.0) direction.Reverse();
          gp_Pnt point;
          gp_Vec derivativeU, derivativeV;
          surface.D1(u, v, point, derivativeU, derivativeV);
          const double metricUU = derivativeU.Dot(derivativeU);
          const double metricUV = derivativeU.Dot(derivativeV);
          const double metricVV = derivativeV.Dot(derivativeV);
          const double determinant = metricUU * metricVV - metricUV * metricUV;
          const double metricScale = metricUU * metricVV;
          if (!(metricScale > std::numeric_limits<double>::min()) ||
              !(determinant > 0.0) || determinant / metricScale <= 1.0e-12)
            return std::nullopt;
          const double projectionU = derivativeU.Dot(gp_Vec(direction));
          const double projectionV = derivativeV.Dot(gp_Vec(direction));
          double uStep = (projectionU * metricVV - projectionV * metricUV) / determinant;
          double vStep = (projectionV * metricUU - projectionU * metricUV) / determinant;
          const double normalizedSpeed = std::sqrt(
              (uStep / uRange) * (uStep / uRange) +
              (vStep / vRange) * (vStep / vRange));
          if (!(normalizedSpeed > 1.0e-14)) return std::nullopt;
          const double scale = options.radiusTraceNormalizedStep / normalizedSpeed;
          uStep *= scale; vStep *= scale;
          const double k1 = properties.MaxCurvature(), k2 = properties.MinCurvature();
          const double dominant = std::abs(k1) >= std::abs(k2) ? k1 : k2;
          const double secondary = std::abs(k1) >= std::abs(k2) ? k2 : k1;
          if (!std::isfinite(dominant) || std::abs(dominant) <= 1.0e-14)
            return std::nullopt;
          FlowState state;
          state.direction = direction;
          state.uStep = uStep; state.vStep = vStep;
          state.evidence.u = u; state.evidence.v = v;
          state.evidence.x = point.X(); state.evidence.y = point.Y();
          state.evidence.z = point.Z();
          state.evidence.radius = 1.0 / std::abs(dominant);
          state.evidence.secondaryCurvatureRatio = std::abs(secondary / dominant);
          state.evidence.spineDirectionX = direction.X();
          state.evidence.spineDirectionY = direction.Y();
          state.evidence.spineDirectionZ = direction.Z();
          return state;
        };

        const double targetNormalizedLength =
            std::max((requested - 1) * options.radiusTraceNormalizedStep, 1.0e-14);
        const auto normalizedTraceLength = [&](const std::vector<RadiusSampleEvidence>& samples) {
          double length = 0.0;
          for (std::size_t index = 1; index < samples.size(); ++index) {
            const double du = (samples[index].u - samples[index - 1].u) / uRange;
            const double dv = (samples[index].v - samples[index - 1].v) / vRange;
            length += std::sqrt(du * du + dv * dv);
          }
          return length;
        };
        struct TraceAttempt {
          std::vector<RadiusSampleEvidence> samples;
          double coverage = 0.0;
          double normalizedLength = 0.0;
          double seedU = 0.0;
          double seedV = 0.0;
          int stepReductions = 0;
        };
        const auto buildStreamline = [&](double seedU, double seedV) {
          TraceAttempt attempt;
          attempt.seedU = seedU; attempt.seedV = seedV;
          const auto seed = evaluateFlow(seedU, seedV, std::nullopt);
          if (!seed) return attempt;
          const int halfCount = (requested - 1) / 2;
          const auto integrate = [&](gp_Dir initialDirection) {
            std::vector<RadiusSampleEvidence> samples;
            auto current = evaluateFlow(seedU, seedV, initialDirection);
            for (int index = 0; current && index < halfCount; ++index) {
              bool advanced = false;
              double factor = 1.0;
              for (int reduction = 0;
                   reduction <= options.radiusTraceMaximumStepReductions; ++reduction) {
                if (reduction > 0) ++attempt.stepReductions;
                const auto midpoint = evaluateFlow(
                    current->evidence.u + 0.5 * factor * current->uStep,
                    current->evidence.v + 0.5 * factor * current->vStep,
                    current->direction);
                const auto next = midpoint ? evaluateFlow(
                    current->evidence.u + factor * midpoint->uStep,
                    current->evidence.v + factor * midpoint->vStep,
                    midpoint->direction) : std::nullopt;
                if (next) {
                  const gp_Pnt previousPoint(current->evidence.x, current->evidence.y,
                                             current->evidence.z);
                  const gp_Pnt nextPoint(next->evidence.x, next->evidence.y, next->evidence.z);
                  const gp_Vec chord(previousPoint, nextPoint);
                  const double tangentAlignment = chord.SquareMagnitude() > 1.0e-28
                      ? std::min(std::abs(gp_Dir(chord).Dot(current->direction)),
                                 std::abs(gp_Dir(chord).Dot(next->direction))) : 0.0;
                  if (previousPoint.Distance(nextPoint) > traceProgressTolerance &&
                      tangentAlignment >= options.minimumRadiusTraceTangentAlignment) {
                    samples.push_back(next->evidence);
                    current = next;
                    advanced = true;
                    break;
                  }
                }
                factor *= options.radiusTraceStepReductionFactor;
              }
              if (!advanced) break;
            }
            return samples;
          };
          std::vector<RadiusSampleEvidence> forward = integrate(seed->direction);
          gp_Dir reverseDirection = seed->direction;
          reverseDirection.Reverse();
          std::vector<RadiusSampleEvidence> backward = integrate(reverseDirection);
          std::reverse(backward.begin(), backward.end());
          for (auto& sample : backward) {
            sample.spineDirectionX = -sample.spineDirectionX;
            sample.spineDirectionY = -sample.spineDirectionY;
            sample.spineDirectionZ = -sample.spineDirectionZ;
          }
          attempt.samples.insert(attempt.samples.end(), backward.begin(), backward.end());
          attempt.samples.push_back(seed->evidence);
          attempt.samples.insert(attempt.samples.end(), forward.begin(), forward.end());
          attempt.normalizedLength = normalizedTraceLength(attempt.samples);
          attempt.coverage = std::min(
              static_cast<double>(attempt.samples.size()) / requested,
              attempt.normalizedLength / targetNormalizedLength);
          return attempt;
        };

        std::vector<TraceCandidate> remainingSeeds = traceCandidates;
        std::sort(remainingSeeds.begin(), remainingSeeds.end(),
            [umin, umax, vmin, vmax](const auto& first, const auto& second) {
              const auto centreDistance = [umin, umax, vmin, vmax](const auto& candidate) {
                const double u = (candidate.evidence.u - umin) / (umax - umin) - 0.5;
                const double v = (candidate.evidence.v - vmin) / (vmax - vmin) - 0.5;
                return u * u + v * v;
              };
              return centreDistance(first) < centreDistance(second);
            });
        // Prefer one axial-centre seed from each transverse row.  Consecutive
        // points in one row commonly converge to the same integral curve and
        // therefore do not constitute independent cross-width validation.
        std::vector<std::pair<int, TraceCandidate>> transverseRowSeeds;
        for (const auto& [rowIndex, row] : rows) {
          const auto representative = std::min_element(
              row.begin(), row.end(), [centre, followsU](const auto& first,
                                                         const auto& second) {
                const int firstIndex = followsU ? first.uIndex : first.vIndex;
                const int secondIndex = followsU ? second.uIndex : second.vIndex;
                return std::abs(firstIndex - centre) < std::abs(secondIndex - centre);
              });
          if (representative != row.end())
            transverseRowSeeds.emplace_back(rowIndex, *representative);
        }
        std::sort(transverseRowSeeds.begin(), transverseRowSeeds.end(),
            [bestRow = best->first, centre](const auto& first, const auto& second) {
              if ((first.first == bestRow) != (second.first == bestRow))
                return first.first == bestRow;
              const int firstDistance = std::abs(first.first - centre);
              const int secondDistance = std::abs(second.first - centre);
              if (firstDistance != secondDistance) return firstDistance < secondDistance;
              return first.first < second.first;
            });
        std::vector<TraceCandidate> seedCandidates;
        for (const auto& [rowIndex, candidate] : transverseRowSeeds) {
          (void)rowIndex;
          seedCandidates.push_back(candidate);
        }
        for (const auto& candidate : remainingSeeds) {
          const bool duplicate = std::any_of(seedCandidates.begin(), seedCandidates.end(),
              [&candidate](const auto& existing) {
                return existing.uIndex == candidate.uIndex &&
                       existing.vIndex == candidate.vIndex;
              });
          if (!duplicate) seedCandidates.push_back(candidate);
        }
        TraceAttempt bestAttempt;
        std::vector<TraceAttempt> validAttempts;
        std::set<int> validTransverseRows;
        int totalAttemptedStepReductions = 0;
        const int seedAttempts = std::min(options.radiusTraceSeedCount,
                                          static_cast<int>(seedCandidates.size()));
        int actualSeedAttempts = 0;
        for (int seedIndex = 0; seedIndex < seedAttempts; ++seedIndex) {
          ++actualSeedAttempts;
          TraceAttempt attempt = buildStreamline(seedCandidates[seedIndex].evidence.u,
                                                 seedCandidates[seedIndex].evidence.v);
          totalAttemptedStepReductions += attempt.stepReductions;
          const bool validAttempt =
              static_cast<int>(attempt.samples.size()) >= options.minimumRadiusTraceSamples &&
              attempt.coverage >= options.minimumRadiusTraceCoverage;
          const int transverseIndex = followsU ? seedCandidates[seedIndex].vIndex
                                               : seedCandidates[seedIndex].uIndex;
          if (validAttempt && validTransverseRows.insert(transverseIndex).second)
            validAttempts.push_back(attempt);
          if (attempt.coverage > bestAttempt.coverage ||
              (attempt.coverage == bestAttempt.coverage &&
               (attempt.normalizedLength > bestAttempt.normalizedLength ||
                (attempt.normalizedLength == bestAttempt.normalizedLength &&
                 attempt.samples.size() > bestAttempt.samples.size()))))
            bestAttempt = attempt;
          if (bestAttempt.coverage >= 0.999 &&
              static_cast<int>(validAttempts.size()) >=
                  options.radiusTraceStabilitySeedCount)
            break;
        }
        result.radiusTraceSeedAttempts = actualSeedAttempts;
        result.radiusTraceBestStreamlineCoverage = bestAttempt.coverage;
        result.radiusTraceBestStreamlineNormalizedLength = bestAttempt.normalizedLength;
        result.radiusTraceBestStreamlineSampleCount =
            static_cast<int>(bestAttempt.samples.size());
        result.radiusTraceTotalAttemptedStepReductions = totalAttemptedStepReductions;
        result.radiusTraceValidSeedCount = static_cast<int>(validAttempts.size());
        if (static_cast<int>(validAttempts.size()) >=
            options.radiusTraceStabilitySeedCount) {
          const auto sampledRadii = [&](std::vector<RadiusSampleEvidence> samples,
                                        bool reverse) {
            if (reverse) std::reverse(samples.begin(), samples.end());
            std::vector<double> distances(samples.size(), 0.0);
            for (std::size_t index = 1; index < samples.size(); ++index)
              distances[index] = distances[index - 1] + gp_Pnt(
                  samples[index - 1].x, samples[index - 1].y,
                  samples[index - 1].z).Distance(gp_Pnt(
                      samples[index].x, samples[index].y, samples[index].z));
            std::vector<double> values;
            values.reserve(static_cast<std::size_t>(requested));
            const double total = distances.empty() ? 0.0 : distances.back();
            for (int sampleIndex = 0; sampleIndex < requested; ++sampleIndex) {
              const double target = requested > 1 && total > traceProgressTolerance
                  ? total * sampleIndex / (requested - 1) : 0.0;
              const auto upper = std::lower_bound(distances.begin(), distances.end(), target);
              if (upper == distances.begin() || upper == distances.end()) {
                values.push_back(upper == distances.end()
                    ? samples.back().radius : samples.front().radius);
                continue;
              }
              const std::size_t upperIndex = static_cast<std::size_t>(
                  std::distance(distances.begin(), upper));
              const double interval = distances[upperIndex] - distances[upperIndex - 1];
              const double ratio = interval > traceProgressTolerance
                  ? (target - distances[upperIndex - 1]) / interval : 0.0;
              values.push_back(samples[upperIndex - 1].radius * (1.0 - ratio) +
                               samples[upperIndex].radius * ratio);
            }
            return values;
          };
          const auto bestRadii = sampledRadii(bestAttempt.samples, false);
          const gp_Pnt bestFront(bestAttempt.samples.front().x,
                                 bestAttempt.samples.front().y,
                                 bestAttempt.samples.front().z);
          const gp_Pnt bestBack(bestAttempt.samples.back().x,
                                bestAttempt.samples.back().y,
                                bestAttempt.samples.back().z);
          int compared = 0;
          for (const auto& alternative : validAttempts) {
            if (alternative.seedU == bestAttempt.seedU &&
                alternative.seedV == bestAttempt.seedV)
              continue;
            const gp_Pnt alternativeFront(alternative.samples.front().x,
                                           alternative.samples.front().y,
                                           alternative.samples.front().z);
            const gp_Pnt alternativeBack(alternative.samples.back().x,
                                          alternative.samples.back().y,
                                          alternative.samples.back().z);
            const bool reverse = bestFront.Distance(alternativeBack) +
                                 bestBack.Distance(alternativeFront) <
                                 bestFront.Distance(alternativeFront) +
                                 bestBack.Distance(alternativeBack);
            const auto alternativeRadii = sampledRadii(alternative.samples, reverse);
            for (std::size_t index = 0; index < bestRadii.size(); ++index) {
              const double scale = std::max(
                  0.5 * (std::abs(bestRadii[index]) +
                         std::abs(alternativeRadii[index])), 1.0e-14);
              result.radiusTraceMaximumCrossSeedRelativeDeviation = std::max(
                  result.radiusTraceMaximumCrossSeedRelativeDeviation,
                  std::abs(bestRadii[index] - alternativeRadii[index]) / scale);
            }
            if (++compared + 1 >= options.radiusTraceStabilitySeedCount) break;
          }
          result.radiusTraceStableAcrossSeeds =
              compared + 1 >= options.radiusTraceStabilitySeedCount &&
              result.radiusTraceMaximumCrossSeedRelativeDeviation <=
                  options.maximumRadiusTraceCrossSeedRelativeDeviation;
        }
        std::vector<RadiusSampleEvidence> trace;
        if (static_cast<int>(bestAttempt.samples.size()) >= options.minimumRadiusTraceSamples &&
            bestAttempt.coverage >= options.minimumRadiusTraceCoverage) {
          trace = std::move(bestAttempt.samples);
          result.radiusTraceMethod = RadiusTraceMethod::PrincipalDirectionStreamline;
          result.radiusTraceCoverage = bestAttempt.coverage;
          result.radiusTraceNormalizedLength = bestAttempt.normalizedLength;
          result.radiusTraceSeedU = bestAttempt.seedU;
          result.radiusTraceSeedV = bestAttempt.seedV;
          result.radiusTraceAdaptiveStepReductions = bestAttempt.stepReductions;
        }
        if (trace.empty()) {
          for (int index = 0; index < requested; ++index) {
            const double ratio = (index + 0.5) / requested;
            const double u = followsU ? umin + ratio * uRange : transverseParameter;
            const double v = followsU ? transverseParameter : vmin + ratio * vRange;
            const auto sample = evaluateFlow(u, v, std::nullopt);
            if (sample) trace.push_back(sample->evidence);
          }
          const double normalizedLength = normalizedTraceLength(trace);
          const double coverage = std::min(static_cast<double>(trace.size()) / requested,
                                            normalizedLength / targetNormalizedLength);
          if (static_cast<int>(trace.size()) >= options.minimumRadiusTraceSamples &&
              coverage >= options.minimumRadiusTraceCoverage) {
            result.radiusTraceMethod = RadiusTraceMethod::ParameterLineFallback;
            result.radiusTraceCoverage = coverage;
            result.radiusTraceNormalizedLength = normalizedLength;
            result.radiusTraceSeedU = seedCandidate->evidence.u;
            result.radiusTraceSeedV = seedCandidate->evidence.v;
          } else {
            trace.clear();
          }
        }
        if (trace.empty()) return result;
        std::vector<double> accumulated(trace.size(), 0.0);
        for (std::size_t index = 1; index < trace.size(); ++index) {
          const gp_Pnt previous(trace[index - 1].x, trace[index - 1].y,
                                trace[index - 1].z);
          const gp_Pnt current(trace[index].x, trace[index].y, trace[index].z);
          accumulated[index] = accumulated[index - 1] + previous.Distance(current);
        }
        const double totalLength = accumulated.back();
        double minimumTangentAlignment = 1.0;
        for (std::size_t index = 0; index < trace.size(); ++index) {
          trace[index].normalizedSpineParameter = totalLength > traceProgressTolerance
              ? accumulated[index] / totalLength
              : (trace.size() > 1 ? static_cast<double>(index) / (trace.size() - 1) : 0.0);
          result.radiusSamples.push_back(trace[index]);
          if (index > 0) {
            const gp_Vec segment(gp_Pnt(trace[index - 1].x, trace[index - 1].y,
                                        trace[index - 1].z),
                                 gp_Pnt(trace[index].x, trace[index].y, trace[index].z));
            if (segment.SquareMagnitude() > 1.0e-28) {
              const gp_Dir tangent(segment);
              const gp_Dir previousDirection(trace[index - 1].spineDirectionX,
                                             trace[index - 1].spineDirectionY,
                                             trace[index - 1].spineDirectionZ);
              const gp_Dir currentDirection(trace[index].spineDirectionX,
                                            trace[index].spineDirectionY,
                                            trace[index].spineDirectionZ);
              minimumTangentAlignment = std::min({minimumTangentAlignment,
                  std::abs(tangent.Dot(previousDirection)),
                  std::abs(tangent.Dot(currentDirection))});
            }
          }
        }
        result.radiusTraceMinimumTangentAlignment = minimumTangentAlignment;
      }
    }
  } catch (const Standard_Failure&) {}
  return result;
}

Classified classify(const TopoDS_Face& face, const Options& options,
                    double canonicalTolerance, double linearTolerance) {
  Classified result;
  try {
    BRepAdaptor_Surface surface(face, Standard_True);
    switch (surface.GetType()) {
      case GeomAbs_Cylinder:
        result = {true, GeometrySource::AnalyticCylinder, surface.Cylinder().Radius(), 0.0};
        result.minimumRadius = result.maximumRadius = result.radius;
        result.secondaryCurvatureRatio = 0.0;
        result.angularCoverage = std::min(1.0, std::abs(surface.LastUParameter() -
            surface.FirstUParameter()) / (2.0 * 3.14159265358979323846));
        return result;
      case GeomAbs_Torus:
        result = {true, GeometrySource::AnalyticTorus, surface.Torus().MinorRadius(), 0.0};
        result.minimumRadius = result.maximumRadius = result.radius;
        return result;
      case GeomAbs_Sphere:
        result = {true, GeometrySource::AnalyticSphere, surface.Sphere().Radius(), 0.0};
        result.minimumRadius = result.maximumRadius = result.radius;
        result.doubleCurved = true;
        result.secondaryCurvatureRatio = 1.0;
        result.sameSignCurvatureFraction = 1.0;
        return result;
      default: break;
    }
    if (!options.enableCanonicalRecovery)
      return options.enableCurvatureField ? curvatureClassify(face, options, linearTolerance) : result;
    ShapeAnalysis_CanonicalRecognition recognition(face);
    gp_Cylinder cylinder;
    if (recognition.IsCylinder(canonicalTolerance, cylinder)) {
      result = {true, GeometrySource::RecoveredCylinder, cylinder.Radius(), recognition.GetGap()};
      result.minimumRadius = result.maximumRadius = result.radius;
      result.secondaryCurvatureRatio = 0.0;
      return result;
    }
    recognition.ClearStatus();
    gp_Sphere sphere;
    if (recognition.IsSphere(canonicalTolerance, sphere)) {
      result = {true, GeometrySource::RecoveredSphere, sphere.Radius(), recognition.GetGap()};
      result.minimumRadius = result.maximumRadius = result.radius;
      result.doubleCurved = true;
      result.secondaryCurvatureRatio = 1.0;
      result.sameSignCurvatureFraction = 1.0;
      return result;
    }
  } catch (const Standard_Failure&) {}
  return options.enableCurvatureField ? curvatureClassify(face, options, linearTolerance) : result;
}

bool radiusCompatible(double a, double b, const Options& options) {
  const double tolerance = std::max(options.absoluteTolerance,
      std::max(std::abs(a), std::abs(b)) * options.radiusRelativeTolerance);
  return std::abs(a - b) <= tolerance;
}

bool radiusMatches(const Classified& geometry, const Options& options) {
  if (!std::isfinite(geometry.radius) ||
      !std::isfinite(geometry.minimumRadius) ||
      !std::isfinite(geometry.maximumRadius)) return false;
  switch (options.radiusFilterMode) {
    case RadiusFilterMode::NominalWithinRange:
      return geometry.radius >= options.minimumRadius &&
             geometry.radius <= options.maximumRadius;
    case RadiusFilterMode::EntireProfileWithinRange:
      return geometry.minimumRadius >= options.minimumRadius &&
             geometry.maximumRadius <= options.maximumRadius;
    case RadiusFilterMode::ProfileIntersectsRange:
      return geometry.maximumRadius >= options.minimumRadius &&
             geometry.minimumRadius <= options.maximumRadius;
    case RadiusFilterMode::AnyRadius:
      return true;
  }
  return false;
}

int radiusBehaviorSeverity(RadiusBehavior behavior) {
  switch (behavior) {
    case RadiusBehavior::InsufficientEvidence: return 0;
    case RadiusBehavior::Constant: return 1;
    case RadiusBehavior::SmoothVariable: return 2;
    case RadiusBehavior::Segmented: return 3;
    case RadiusBehavior::Discontinuous: return 4;
  }
  return 0;
}

int evidenceStateSeverity(EvidenceState state) {
  switch (state) {
    case EvidenceState::Validated: return 0;
    case EvidenceState::InsufficientEvidence: return 1;
    case EvidenceState::Conflict: return 2;
  }
  return 1;
}

double evidenceStateScore(EvidenceState state, const Options& options) {
  switch (state) {
    case EvidenceState::Validated: return options.validatedEvidenceScore;
    case EvidenceState::InsufficientEvidence: return options.insufficientEvidenceScore;
    case EvidenceState::Conflict: return options.conflictEvidenceScore;
  }
  return options.insufficientEvidenceScore;
}

bool hasBandGeometry(const FaceEvidence& evidence) {
  return evidence.geometrySource == GeometrySource::AnalyticCylinder ||
         evidence.geometrySource == GeometrySource::RecoveredCylinder ||
         evidence.geometrySource == GeometrySource::AnalyticTorus ||
         (evidence.geometrySource == GeometrySource::CurvatureField && !evidence.doubleCurved);
}

bool hasAnalyticBandGeometry(const FaceEvidence& evidence) {
  return evidence.geometrySource == GeometrySource::AnalyticCylinder ||
         evidence.geometrySource == GeometrySource::RecoveredCylinder ||
         evidence.geometrySource == GeometrySource::AnalyticTorus;
}

int commonExternalSupports(const FaceEvidence& a, const FaceEvidence& b) {
  int count = 0;
  for (int id : a.externalSupportFaceIds)
    if (std::find(b.externalSupportFaceIds.begin(), b.externalSupportFaceIds.end(), id) !=
        b.externalSupportFaceIds.end())
      ++count;
  return count;
}

bool geometricallyTangent(const TopoDS_Edge& edge, const TopoDS_Face& candidate,
                          const TopoDS_Face& other, double gapTolerance,
                          double angleTolerance, double linearTolerance) {
  try {
    BRepAdaptor_Curve curve(edge);
    const double first = curve.FirstParameter(), last = curve.LastParameter();
    if (!std::isfinite(first) || !std::isfinite(last)) return false;
    const gp_Pnt pointCandidate = curve.Value(0.5 * (first + last));
    const Handle(Geom_Surface) surfaceCandidate = BRep_Tool::Surface(candidate);
    const Handle(Geom_Surface) surfaceOther = BRep_Tool::Surface(other);
    if (surfaceCandidate.IsNull() || surfaceOther.IsNull()) return false;
    ShapeAnalysis_Surface analysisCandidate(surfaceCandidate);
    ShapeAnalysis_Surface analysisOther(surfaceOther);
    const gp_Pnt2d uvCandidate = analysisCandidate.ValueOfUV(pointCandidate, gapTolerance);
    const gp_Pnt2d uvOther = analysisOther.ValueOfUV(pointCandidate, gapTolerance);
    if (analysisOther.Gap() > gapTolerance) return false;
    GeomLProp_SLProps propsCandidate(surfaceCandidate, uvCandidate.X(), uvCandidate.Y(), 1,
                                    linearTolerance);
    GeomLProp_SLProps propsOther(surfaceOther, uvOther.X(), uvOther.Y(), 1, linearTolerance);
    if (!propsCandidate.IsNormalDefined() || !propsOther.IsNormalDefined()) return false;
    return std::abs(propsCandidate.Normal().Dot(propsOther.Normal())) >= std::cos(angleTolerance);
  } catch (const Standard_Failure&) {
    return false;
  }
}

} // namespace

Result recognize(const TopoDS_Shape& shape, const Options& options) {
  using PerformanceClock = std::chrono::steady_clock;
  const auto recognitionStart = PerformanceClock::now();
  const auto elapsedMilliseconds = [](const auto& first, const auto& second) {
    return std::chrono::duration<double, std::milli>(second - first).count();
  };
  if (shape.IsNull()) throw std::invalid_argument("ImprovedFilletRecognizer: null shape");
  if (options.radiusFilterMode != RadiusFilterMode::AnyRadius &&
      (!(options.minimumRadius >= 0.0) ||
       !(options.maximumRadius >= options.minimumRadius)))
    throw std::invalid_argument("ImprovedFilletRecognizer: invalid radius range");
  if (!(options.constantRadiusRelativeSpread >= 0.0) ||
      options.minimumRadiusTraceSamples < 2 ||
      options.radiusTraceSampleCount < options.minimumRadiusTraceSamples ||
      !(options.radiusTraceNormalizedStep > 0.0) ||
      !(options.minimumRadiusTraceCoverage > 0.0 &&
        options.minimumRadiusTraceCoverage <= 1.0) ||
      !(options.minimumRadiusTraceTangentAlignment >= 0.0 &&
        options.minimumRadiusTraceTangentAlignment <= 1.0) ||
      options.radiusTraceSeedCount < 1 ||
      options.radiusTraceStabilitySeedCount < 2 ||
      options.radiusTraceStabilitySeedCount > options.radiusTraceSeedCount ||
      options.radiusTraceMaximumStepReductions < 0 ||
      !(options.radiusTraceStepReductionFactor > 0.0 &&
        options.radiusTraceStepReductionFactor < 1.0) ||
      !(options.maximumRadiusTraceCrossSeedRelativeDeviation >= 0.0) ||
      !(options.maximumRadiusProfileTransitionGapToModelDiagonal >= 0.0) ||
      !(options.minimumRadiusProfileTransitionAlignment >= -1.0 &&
        options.minimumRadiusProfileTransitionAlignment <= 1.0) ||
      !(options.radiusProfileDirectionCostWeight >= 0.0) ||
      !(options.minimumResolvedEdgeLengthToModelDiagonal >= 0.0) ||
      !(options.minimumResolvedFaceAreaToModelDiagonalSquared >= 0.0) ||
      !(options.maximumRecognitionMilliseconds >= 0.0) ||
      !(options.featureTopologyEvidenceWeight >= 0.0) ||
      !(options.featureRadiusEvidenceWeight >= 0.0) ||
      !(options.featureDirectionEvidenceWeight >= 0.0) ||
      !(options.featureTopologyEvidenceWeight + options.featureRadiusEvidenceWeight +
        options.featureDirectionEvidenceWeight > 0.0) ||
      !(options.conflictEvidenceScore >= 0.0 &&
        options.conflictEvidenceScore <= options.insufficientEvidenceScore &&
        options.insufficientEvidenceScore <= options.validatedEvidenceScore &&
        options.validatedEvidenceScore <= 1.0) ||
      !(options.maximumSmoothRadiusRelativeStep >= 0.0) ||
      !(options.discontinuousRadiusRelativeStep >=
        options.maximumSmoothRadiusRelativeStep) ||
      !(options.minimumSmoothActiveStepFraction >= 0.0 &&
        options.minimumSmoothActiveStepFraction <= 1.0) ||
      !(options.minimumSmoothMonotonicStepFraction >= 0.0 &&
        options.minimumSmoothMonotonicStepFraction <= 1.0) ||
      !(options.minimumDiscontinuityVariationFraction >= 0.0 &&
        options.minimumDiscontinuityVariationFraction <= 1.0))
    throw std::invalid_argument("ImprovedFilletRecognizer: invalid radius behavior thresholds");
  Result result;
  result.performance.budgetMilliseconds = options.maximumRecognitionMilliseconds;
  result.performance.budgetEnabled = options.maximumRecognitionMilliseconds > 0.0;
  result.diagnostics = diagnose(shape, options);
  const auto diagnosticsEnd = PerformanceClock::now();
  result.performance.diagnosticsMilliseconds =
      elapsedMilliseconds(recognitionStart, diagnosticsEnd);

  TopTools_IndexedMapOfShape faceMap;
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapes(shape, TopAbs_FACE, faceMap);
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  std::vector<Bnd_Box> faceBoxes(static_cast<std::size_t>(faceMap.Extent() + 1));
  for (int id = 1; id <= faceMap.Extent(); ++id)
    BRepBndLib::Add(TopoDS::Face(faceMap.FindKey(id)), faceBoxes[id], Standard_False);
  const double canonicalTolerance = result.diagnostics.effectiveLinearTolerance *
                                    options.canonicalToleranceFactor;
  GProp_GProps totalProperties;
  BRepGProp::SurfaceProperties(shape, totalProperties);
  const double totalArea = std::max(totalProperties.Mass(), 1.0e-30);

  std::map<int, std::size_t> acceptedByFaceId;
  std::map<std::string, int> fingerprintOccurrences;
  int geometricSupportTestsForModel = 0;
  for (int faceId = 1; faceId <= faceMap.Extent(); ++faceId) {
    const TopoDS_Face face = TopoDS::Face(faceMap.FindKey(faceId));
    FaceEvidence evidence;
    evidence.faceId = faceId;
    evidence.face = face;
    const Classified geometry = classify(face, options, canonicalTolerance,
                                         result.diagnostics.effectiveLinearTolerance);
    GProp_GProps faceProperties;
    BRepGProp::SurfaceProperties(face, faceProperties);
    evidence.surfaceArea = faceProperties.Mass();
    const double modelDiagonalSquared = result.diagnostics.modelDiagonal *
                                        result.diagnostics.modelDiagonal;
    evidence.areaToModelDiagonalSquared = modelDiagonalSquared > 0.0
        ? evidence.surfaceArea / modelDiagonalSquared : 0.0;
    evidence.numericallyResolved = modelDiagonalSquared <= 0.0 ||
        evidence.areaToModelDiagonalSquared >=
            options.minimumResolvedFaceAreaToModelDiagonalSquared;
    const std::string fingerprint = geometryFingerprint(
        face, geometry, result.diagnostics.modelDiagonal,
        result.diagnostics.effectiveLinearTolerance);
    const int occurrence = ++fingerprintOccurrences[fingerprint];
    evidence.persistentId = occurrence == 1 ? fingerprint
        : fingerprint + "-" + std::to_string(occurrence);
    if (!geometry.recognized) {
      evidence.verdict = Verdict::RejectedGeometry;
      evidence.reason = "surface is neither analytic nor canonically recoverable";
      if (options.keepRejectedEvidence) result.faces.push_back(std::move(evidence));
      continue;
    }
    evidence.geometrySource = geometry.source;
    evidence.radius = geometry.radius;
    evidence.minimumRadius = geometry.minimumRadius;
    evidence.maximumRadius = geometry.maximumRadius;
    evidence.canonicalGap = geometry.gap;
    evidence.angularCoverage = geometry.angularCoverage;
    evidence.variableRadius = geometry.variableRadius;
    evidence.doubleCurved = geometry.doubleCurved;
    evidence.secondaryCurvatureRatio = geometry.secondaryCurvatureRatio;
    evidence.sameSignCurvatureFraction = geometry.sameSignCurvatureFraction;
    evidence.radiusSamples = geometry.radiusSamples;
    evidence.radiusTraceMethod = geometry.radiusTraceMethod;
    evidence.radiusTraceCoverage = geometry.radiusTraceCoverage;
    evidence.radiusTraceMinimumTangentAlignment =
        geometry.radiusTraceMinimumTangentAlignment;
    evidence.radiusTraceNormalizedLength = geometry.radiusTraceNormalizedLength;
    evidence.radiusTraceSeedU = geometry.radiusTraceSeedU;
    evidence.radiusTraceSeedV = geometry.radiusTraceSeedV;
    evidence.radiusTraceSeedAttempts = geometry.radiusTraceSeedAttempts;
    evidence.radiusTraceAdaptiveStepReductions =
        geometry.radiusTraceAdaptiveStepReductions;
    evidence.radiusTraceBestStreamlineCoverage =
        geometry.radiusTraceBestStreamlineCoverage;
    evidence.radiusTraceBestStreamlineNormalizedLength =
        geometry.radiusTraceBestStreamlineNormalizedLength;
    evidence.radiusTraceBestStreamlineSampleCount =
        geometry.radiusTraceBestStreamlineSampleCount;
    evidence.radiusTraceTotalAttemptedStepReductions =
        geometry.radiusTraceTotalAttemptedStepReductions;
    evidence.radiusTraceValidSeedCount = geometry.radiusTraceValidSeedCount;
    evidence.radiusTraceMaximumCrossSeedRelativeDeviation =
        geometry.radiusTraceMaximumCrossSeedRelativeDeviation;
    evidence.radiusTraceStableAcrossSeeds = geometry.radiusTraceStableAcrossSeeds;
    evidence.convexity = geometry.convexity;
    evidence.areaRatio = evidence.surfaceArea / totalArea;
    evidence.radiusToModelDiagonal = result.diagnostics.modelDiagonal > 0.0
        ? evidence.radius / result.diagnostics.modelDiagonal : 0.0;
    if (!evidence.numericallyResolved) {
      evidence.verdict = Verdict::RejectedNumericallyUnresolved;
      evidence.reason = "face area is below the scale-normalized resolution threshold";
      if (options.keepRejectedEvidence) result.faces.push_back(std::move(evidence));
      continue;
    }
    if (!radiusMatches(geometry, options)) {
      evidence.verdict = Verdict::RejectedRadius;
      evidence.reason = "radius outside requested range";
      if (options.keepRejectedEvidence) result.faces.push_back(std::move(evidence));
      continue;
    }

    std::set<int> supportIds;
    std::set<int> inferredSupportIds;
    std::set<int> tangentEdges;
    std::set<int> nonTangentEdges;
    std::vector<TopoDS_Edge> boundaryEdges;
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
      const TopoDS_Edge edge = TopoDS::Edge(ex.Current());
      if (BRep_Tool::IsClosed(edge, face) || BRep_Tool::Degenerated(edge)) continue;
      boundaryEdges.push_back(edge);
      const int edgeIndex = edgeFaces.FindIndex(edge);
      if (edgeIndex <= 0) { nonTangentEdges.insert(edgeIndex); continue; }
      bool foundSupport = false;
      for (TopTools_ListIteratorOfListOfShape it(edgeFaces.FindFromIndex(edgeIndex)); it.More(); it.Next()) {
        const int otherId = faceMap.FindIndex(it.Value());
        if (otherId <= 0 || otherId == faceId) continue;
        const TopoDS_Face other = TopoDS::Face(faceMap.FindKey(otherId));
        if (normalsAreTangent(edge, face, other, options.tangentAngleToleranceRadians,
                              result.diagnostics.effectiveLinearTolerance)) {
          supportIds.insert(otherId);
          foundSupport = true;
        }
      }
      (foundSupport ? tangentEdges : nonTangentEdges).insert(edgeIndex);
    }
    if (options.enableGeometricSupportRecovery &&
        static_cast<int>(supportIds.size()) < options.minimumSupportFaces) {
      const double gapTolerance = result.diagnostics.effectiveLinearTolerance *
                                  options.geometricSupportGapFactor;
      int tests = 0;
      for (const TopoDS_Edge& edge : boundaryEdges) {
        Bnd_Box edgeBox;
        BRepBndLib::Add(edge, edgeBox, Standard_False);
        edgeBox.Enlarge(gapTolerance);
        for (int otherId = 1; otherId <= faceMap.Extent(); ++otherId) {
          if (otherId == faceId || supportIds.count(otherId) ||
              edgeBox.IsOut(faceBoxes[otherId])) continue;
          if (++tests > options.maximumGeometricSupportTestsPerFace ||
              ++geometricSupportTestsForModel > options.maximumGeometricSupportTestsPerModel) break;
          const TopoDS_Face other = TopoDS::Face(faceMap.FindKey(otherId));
          if (geometricallyTangent(edge, face, other, gapTolerance,
                  options.tangentAngleToleranceRadians,
                  result.diagnostics.effectiveLinearTolerance)) {
            supportIds.insert(otherId);
            inferredSupportIds.insert(otherId);
          }
        }
        if (tests > options.maximumGeometricSupportTestsPerFace ||
            geometricSupportTestsForModel > options.maximumGeometricSupportTestsPerModel ||
            static_cast<int>(supportIds.size()) >= options.minimumSupportFaces) break;
      }
    }
    evidence.supportFaceIds.assign(supportIds.begin(), supportIds.end());
    evidence.inferredSupportFaceIds.assign(inferredSupportIds.begin(), inferredSupportIds.end());
    evidence.tangentBoundaryCount = static_cast<int>(tangentEdges.size());
    evidence.nonTangentBoundaryCount = static_cast<int>(nonTangentEdges.size());
    const int primarySignals =
        (geometry.source == GeometrySource::AnalyticCylinder &&
         geometry.angularCoverage >= options.primaryCylinderAngularCoverage ? 1 : 0) +
        (evidence.radiusToModelDiagonal >= options.primaryRadiusToDiagonal ? 1 : 0) +
        (evidence.areaRatio >= options.primaryAreaRatio ? 1 : 0);
    evidence.likelyPrimarySurface = primarySignals >= 2;
    const bool enoughSupports = static_cast<int>(supportIds.size()) >= options.minimumSupportFaces;
    const bool accepted = enoughSupports && !evidence.likelyPrimarySurface;
    evidence.verdict = accepted ? Verdict::Accepted
        : (evidence.likelyPrimarySurface ? Verdict::RejectedLikelyPrimarySurface
                                         : Verdict::RejectedNoSupports);
    evidence.reason = accepted ? "two or more distinct tangent support faces"
        : (evidence.likelyPrimarySurface ? "multiple primary-surface risk signals"
                                         : "fewer than two distinct tangent support faces");
    const double geometryScore = geometry.gap == 0.0 ? 0.4 : 0.3;
    const double supportScore = std::min(0.4, supportIds.size() * 0.2);
    const double tangentScore = tangentEdges.empty() ? 0.0 : 0.2;
    const double fieldPenalty = geometry.source == GeometrySource::CurvatureField ? 0.1 : 0.0;
    evidence.confidence = std::max(0.0, geometryScore + supportScore + tangentScore - fieldPenalty);
    result.faces.push_back(std::move(evidence));
    if (accepted) acceptedByFaceId[faceId] = result.faces.size() - 1;
  }

  // A tangent neighbor can itself be a fillet patch.  Such a neighbor is useful
  // for the direct adjacency graph but is not a structural support surface and
  // must not create a synthetic shared-support link.
  for (auto& evidence : result.faces) {
    for (int supportId : evidence.supportFaceIds)
      if (!acceptedByFaceId.count(supportId))
        evidence.externalSupportFaceIds.push_back(supportId);
  }
  const auto faceAnalysisEnd = PerformanceClock::now();
  result.performance.faceAnalysisMilliseconds =
      elapsedMilliseconds(diagnosticsEnd, faceAnalysisEnd);
  result.performance.geometricSupportTestCount = geometricSupportTestsForModel;

  std::vector<std::set<int>> graph(faceMap.Extent() + 1);
  std::set<std::pair<int, int>> shortSharedEdgePairs;
  for (int edgeIndex = 1; edgeIndex <= edgeFaces.Extent(); ++edgeIndex) {
    std::set<int> acceptedOwners;
    for (TopTools_ListIteratorOfListOfShape it(edgeFaces.FindFromIndex(edgeIndex)); it.More(); it.Next()) {
      const int id = faceMap.FindIndex(it.Value());
      if (acceptedByFaceId.count(id)) acceptedOwners.insert(id);
    }
    const std::vector<int> accepted(acceptedOwners.begin(), acceptedOwners.end());
    for (std::size_t i = 0; i < accepted.size(); ++i)
      for (std::size_t j = i + 1; j < accepted.size(); ++j) {
        const int a = accepted[i], b = accepted[j];
        const auto& ea = result.faces[acceptedByFaceId[a]];
        const auto& eb = result.faces[acceptedByFaceId[b]];
        Link link;
        link.firstFaceId = a; link.secondFaceId = b; link.sharedEdge = true;
        GProp_GProps edgeProperties;
        BRepGProp::LinearProperties(TopoDS::Edge(edgeFaces.FindKey(edgeIndex)),
                                   edgeProperties);
        link.sharedEdgeLength = edgeProperties.Mass();
        link.sharedEdgeLengthToModelDiagonal = result.diagnostics.modelDiagonal > 0.0
            ? link.sharedEdgeLength / result.diagnostics.modelDiagonal : 0.0;
        link.rejectedShortSharedEdge = result.diagnostics.modelDiagonal > 0.0 &&
            link.sharedEdgeLengthToModelDiagonal <
                options.minimumResolvedEdgeLengthToModelDiagonal;
        if (link.rejectedShortSharedEdge)
          shortSharedEdgePairs.insert(std::minmax(a, b));
        link.radiusCompatible = radiusCompatible(ea.radius, eb.radius, options);
        link.tangent = normalsAreTangent(TopoDS::Edge(edgeFaces.FindKey(edgeIndex)), ea.face, eb.face,
                                        options.tangentAngleToleranceRadians,
                                        result.diagnostics.effectiveLinearTolerance);
        link.sharedSupportPair = commonExternalSupports(ea, eb) >= 2;
        if (hasBandGeometry(ea) && hasBandGeometry(eb)) {
          const DirectionStatistics direction = spineDirectionStatistics(
              TopoDS::Edge(edgeFaces.FindKey(edgeIndex)), ea.face, eb.face,
              result.diagnostics.effectiveLinearTolerance, options);
          assignDirectionStatistics(link, direction,
                                    DirectionEvidenceSource::SharedEdge, options);
        }
        link.usedInGraph = !link.rejectedShortSharedEdge && link.radiusCompatible &&
                           (link.tangent || link.sharedSupportPair);
        result.links.push_back(link);
        if (link.usedInGraph) {
          graph[a].insert(b); graph[b].insert(a);
        }
      }
  }
  // Composite connection: split patches with the same two supports can belong
  // to one chain even when no direct candidate-candidate edge survives import.
  for (auto a = acceptedByFaceId.begin(); a != acceptedByFaceId.end(); ++a)
    for (auto b = std::next(a); b != acceptedByFaceId.end(); ++b) {
      if (graph[a->first].count(b->first)) continue;
      if (shortSharedEdgePairs.count(std::minmax(a->first, b->first))) continue;
      const auto& ea = result.faces[a->second];
      const auto& eb = result.faces[b->second];
      Bnd_Box proximityBox = faceBoxes[a->first];
      const double compositeGapTolerance =
          result.diagnostics.effectiveLinearTolerance *
          options.geometricSupportGapFactor;
      proximityBox.Enlarge(compositeGapTolerance);
      const bool spatiallyAdjacent = !proximityBox.IsOut(faceBoxes[b->first]);
      bool boundaryAdjacent = false;
      if (spatiallyAdjacent) {
        try {
          BRepExtrema_DistShapeShape distance(ea.face, eb.face);
          distance.Perform();
          boundaryAdjacent = distance.IsDone() &&
                             distance.Value() <= compositeGapTolerance;
        } catch (const Standard_Failure&) {}
      }
      if (boundaryAdjacent && commonExternalSupports(ea, eb) >= 2 &&
          radiusCompatible(ea.radius, eb.radius, options)) {
        Link link;
        link.firstFaceId = a->first;
        link.secondFaceId = b->first;
        link.radiusCompatible = true;
        link.sharedSupportPair = true;
        link.usedInGraph = true;
        if (hasBandGeometry(ea) && hasBandGeometry(eb)) {
          const DirectionStatistics direction = projectedSpineDirectionStatistics(
              ea.face, eb.face, result.diagnostics.effectiveLinearTolerance, options);
          assignDirectionStatistics(link, direction,
                                    DirectionEvidenceSource::ProjectedBoundary, options);
          if (link.spineDirectionEvaluated && !link.spineDirectionCompatible &&
              options.directionConflictRefinementSampleCount >
                  options.spineDirectionSampleCount) {
            link.directionRefinementAttempted = true;
            link.initialDirectionValidSamples = link.spineDirectionValidSamples;
            link.initialMinimumSpineDirectionAlignment =
                link.minimumSpineDirectionAlignment;
            Options refinedOptions = options;
            refinedOptions.spineDirectionSampleCount =
                options.directionConflictRefinementSampleCount;
            refinedOptions.maximumProjectedDirectionAttempts =
                options.maximumProjectedDirectionRefinementAttempts;
            const DirectionStatistics refined = projectedSpineDirectionStatistics(
                ea.face, eb.face, result.diagnostics.effectiveLinearTolerance,
                refinedOptions);
            if (refined.coverage >= options.minimumSpineDirectionSampleCoverage) {
              const DirectionStatistics merged =
                  mergeDirectionStatistics(direction, refined);
              assignDirectionStatistics(link, merged,
                                        DirectionEvidenceSource::ProjectedBoundary, options);
              link.directionRefinementAttempted = true;
              link.directionRefinementAccepted = true;
              link.initialDirectionValidSamples = direction.valid;
              link.initialMinimumSpineDirectionAlignment = direction.minimum;
            }
          }
        }
        result.links.push_back(link);
        graph[a->first].insert(b->first); graph[b->first].insert(a->first);
      }
    }

  const auto facePairKey = [](int first, int second) {
    return std::make_pair(std::min(first, second), std::max(first, second));
  };
  std::map<std::pair<int, int>, std::vector<std::size_t>> linksByFacePair;
  for (std::size_t linkIndex = 0; linkIndex < result.links.size(); ++linkIndex)
    linksByFacePair[facePairKey(result.links[linkIndex].firstFaceId,
                                result.links[linkIndex].secondFaceId)].push_back(linkIndex);
  result.performance.linkPairIndexEntryCount =
      static_cast<int>(linksByFacePair.size());
  const std::vector<std::size_t> emptyLinkIndexes;
  const auto linkIndexesForFaces = [&](int first, int second)
      -> const std::vector<std::size_t>& {
    ++result.performance.linkPairLookupCount;
    const auto found = linksByFacePair.find(facePairKey(first, second));
    if (found == linksByFacePair.end()) return emptyLinkIndexes;
    result.performance.linkCandidatesVisitedThroughIndex +=
        static_cast<int>(found->second.size());
    return found->second;
  };

  for (const auto& entry : acceptedByFaceId) {
    auto& evidence = result.faces[entry.second];
    const std::size_t degree = graph[entry.first].size();
    evidence.topologyRole = degree == 0 ? TopologyRole::IsolatedPatch
        : degree == 1 ? TopologyRole::TerminalPatch
        : degree == 2 ? TopologyRole::BandPatch
                      : TopologyRole::JunctionPatch;
    const bool sphereLike = evidence.geometrySource == GeometrySource::AnalyticSphere ||
                            evidence.geometrySource == GeometrySource::RecoveredSphere ||
                            evidence.doubleCurved;
    const bool bandLike = evidence.geometrySource == GeometrySource::AnalyticCylinder ||
                          evidence.geometrySource == GeometrySource::RecoveredCylinder ||
                          evidence.geometrySource == GeometrySource::AnalyticTorus ||
                          (evidence.geometrySource == GeometrySource::CurvatureField &&
                           !evidence.doubleCurved);
    if (evidence.topologyRole == TopologyRole::JunctionPatch) {
      evidence.roleConsistency = sphereLike ? RoleConsistency::Consistent
                                             : RoleConsistency::NeedsReview;
      evidence.roleReason = sphereLike ? "junction topology agrees with double-curved geometry"
          : "junction topology lacks sphere-like or double-curved geometry";
    } else if (evidence.topologyRole == TopologyRole::BandPatch) {
      evidence.roleConsistency = bandLike ? RoleConsistency::Consistent
                                           : RoleConsistency::NeedsReview;
      evidence.roleReason = bandLike ? "band topology agrees with rolling-surface geometry"
          : "band topology conflicts with sphere-like geometry";
    } else {
      evidence.roleConsistency = RoleConsistency::Unknown;
      evidence.roleReason = "isolated or terminal topology is not sufficient for role validation";
    }
  }

  std::set<int> visited;
  for (const auto& entry : acceptedByFaceId) {
    const int root = entry.first;
    if (visited.count(root)) continue;
    Chain chain;
    std::queue<int> pending; pending.push(root); visited.insert(root);
    double radiusSum = 0.0, confidenceSum = 0.0;
    chain.minimumRadius = result.faces[entry.second].minimumRadius;
    chain.maximumRadius = result.faces[entry.second].maximumRadius;
    bool hasEnd = false;
    std::set<int> supports;
    while (!pending.empty()) {
      const int id = pending.front(); pending.pop();
      const auto& face = result.faces[acceptedByFaceId[id]];
      chain.faceIds.push_back(id);
      chain.minimumRadius = std::min(chain.minimumRadius, face.minimumRadius);
      chain.maximumRadius = std::max(chain.maximumRadius, face.maximumRadius);
      radiusSum += face.radius; confidenceSum += face.confidence;
      hasEnd = hasEnd || face.nonTangentBoundaryCount > 0;
      supports.insert(face.externalSupportFaceIds.begin(), face.externalSupportFaceIds.end());
      chain.isBranched = chain.isBranched || graph[id].size() > 2;
      for (int next : graph[id]) if (!visited.count(next)) {
        visited.insert(next); pending.push(next);
      }
    }
    chain.supportFaceIds.assign(supports.begin(), supports.end());
    chain.meanRadius = radiusSum / chain.faceIds.size();
    chain.confidence = confidenceSum / chain.faceIds.size();
    const bool supportValidated = static_cast<int>(chain.supportFaceIds.size()) >=
                                  options.minimumSupportFaces;
    chain.verdict = supportValidated ? ChainVerdict::Accepted
        : ChainVerdict::NeedsReviewInsufficientExternalSupports;
    if (!supportValidated) chain.confidence *= 0.5;
    chain.isClosed = !hasEnd;
    result.chains.push_back(std::move(chain));
  }

  // Decompose the accepted-face graph into maximal non-branching paths.
  // This preserves junction semantics that a plain connected component loses.
  using EdgeKey = std::pair<int, int>;
  const auto edgeKey = [](int a, int b) { return std::minmax(a, b); };
  std::set<EdgeKey> visitedEdges;
  for (const auto& entry : acceptedByFaceId) {
    const int node = entry.first;
    if (graph[node].empty()) {
      result.paths.push_back({{node}, node, node, false, false, false});
      continue;
    }
    if (graph[node].size() == 2) continue;
    for (int neighbor : graph[node]) {
      if (visitedEdges.count(edgeKey(node, neighbor))) continue;
      Path path;
      path.startNodeFaceId = node;
      path.startsAtBranch = graph[node].size() > 2;
      path.faceIds.push_back(node);
      int previous = node, current = neighbor;
      visitedEdges.insert(edgeKey(previous, current));
      while (true) {
        path.faceIds.push_back(current);
        if (graph[current].size() != 2) break;
        const auto iterator = graph[current].begin();
        int next = *iterator;
        if (next == previous) next = *std::next(iterator);
        if (visitedEdges.count(edgeKey(current, next))) break;
        previous = current;
        current = next;
        visitedEdges.insert(edgeKey(previous, current));
      }
      path.endNodeFaceId = current;
      path.endsAtBranch = graph[current].size() > 2;
      result.paths.push_back(std::move(path));
    }
  }
  // Remaining edges belong to components where every node has degree two.
  for (const auto& entry : acceptedByFaceId) {
    const int root = entry.first;
    for (int neighbor : graph[root]) {
      if (visitedEdges.count(edgeKey(root, neighbor))) continue;
      Path cycle;
      cycle.isClosed = true;
      cycle.startNodeFaceId = root;
      cycle.faceIds.push_back(root);
      int previous = root, current = neighbor;
      visitedEdges.insert(edgeKey(previous, current));
      while (current != root) {
        cycle.faceIds.push_back(current);
        int next = -1;
        for (int candidate : graph[current])
          if (candidate != previous && !visitedEdges.count(edgeKey(current, candidate))) {
            next = candidate; break;
          }
        if (next < 0) { cycle.isClosed = false; break; }
        previous = current; current = next;
        visitedEdges.insert(edgeKey(previous, current));
      }
      cycle.endNodeFaceId = current;
      result.paths.push_back(std::move(cycle));
    }
  }

  // Give every path a deterministic graph direction before orienting its
  // geometric samples.  A one-branch path points away from the junction;
  // otherwise persistent geometry IDs break the otherwise arbitrary tie.
  const auto persistentIdFor = [&](int faceId) -> const std::string& {
    return result.faces[acceptedByFaceId.at(faceId)].persistentId;
  };
  for (auto& path : result.paths) {
    if (path.faceIds.empty()) continue;
    if (path.isClosed) {
      const auto root = std::min_element(path.faceIds.begin(), path.faceIds.end(),
          [&](int first, int second) {
            return persistentIdFor(first) < persistentIdFor(second);
          });
      std::rotate(path.faceIds.begin(), root, path.faceIds.end());
      if (path.faceIds.size() > 2 &&
          persistentIdFor(path.faceIds.back()) < persistentIdFor(path.faceIds[1]))
        std::reverse(path.faceIds.begin() + 1, path.faceIds.end());
      path.startNodeFaceId = path.endNodeFaceId = path.faceIds.front();
      continue;
    }
    bool reverse = path.endsAtBranch && !path.startsAtBranch;
    if (path.startsAtBranch == path.endsAtBranch &&
        persistentIdFor(path.endNodeFaceId) < persistentIdFor(path.startNodeFaceId))
      reverse = true;
    if (reverse) {
      std::reverse(path.faceIds.begin(), path.faceIds.end());
      std::swap(path.startNodeFaceId, path.endNodeFaceId);
      std::swap(path.startsAtBranch, path.endsAtBranch);
    }
  }

  const auto topologyEnd = PerformanceClock::now();
  result.performance.topologyMilliseconds =
      elapsedMilliseconds(faceAnalysisEnd, topologyEnd);

  // Each maximal non-branching path owns one ordered radius profile.  For a
  // freeform face, min/max are sampled curvature evidence rather than a CAD
  // design-law reconstruction, so SmoothVariable remains a candidate label.
  for (std::size_t pathIndex = 0; pathIndex < result.paths.size(); ++pathIndex) {
    const Path& path = result.paths[pathIndex];
    RadiusProfile profile;
    profile.pathId = static_cast<int>(pathIndex + 1);
    profile.faceIds = path.faceIds;
    const auto sampleDistance = [](const RadiusSampleEvidence& first,
                                   const RadiusSampleEvidence& second) {
      return gp_Pnt(first.x, first.y, first.z).Distance(
          gp_Pnt(second.x, second.y, second.z));
    };
    struct TraceFaceSamples {
      int faceId = 0;
      std::size_t pathFaceIndex = 0;
      std::vector<RadiusSampleEvidence> samples;
    };
    std::vector<TraceFaceSamples> traceFaces;
    for (std::size_t faceIndex = 0; faceIndex < path.faceIds.size(); ++faceIndex) {
      const int faceId = path.faceIds[faceIndex];
      const auto evidence = acceptedByFaceId.find(faceId);
      if (evidence == acceptedByFaceId.end()) continue;
      const FaceEvidence& face = result.faces[evidence->second];
      profile.nominalRadii.push_back(face.radius);
      profile.minimumRadii.push_back(face.minimumRadius);
      profile.maximumRadii.push_back(face.maximumRadius);
      if (face.variableRadius) ++profile.variableFaceCount;
      if (face.radiusTraceMethod == RadiusTraceMethod::PrincipalDirectionStreamline)
        ++profile.streamlineFaceCount;
      else if (face.radiusTraceMethod == RadiusTraceMethod::ParameterLineFallback)
        ++profile.parameterLineFallbackFaceCount;
      if (face.radiusTraceMethod == RadiusTraceMethod::PrincipalDirectionStreamline) {
        if (face.radiusTraceValidSeedCount < options.radiusTraceStabilitySeedCount)
          ++profile.insufficientSeedStabilityTraceFaceCount;
        else if (face.radiusTraceStableAcrossSeeds)
          ++profile.stableAcrossSeedsTraceFaceCount;
        else
          ++profile.unstableAcrossSeedsTraceFaceCount;
      } else if (face.radiusTraceMethod == RadiusTraceMethod::ParameterLineFallback) {
        ++profile.insufficientSeedStabilityTraceFaceCount;
      }
      const bool traceAlignmentAccepted = face.radiusSamples.empty() ||
          face.radiusTraceMinimumTangentAlignment >=
              options.minimumRadiusTraceTangentAlignment;
      if (!face.radiusSamples.empty() && !traceAlignmentAccepted)
        ++profile.rejectedLowAlignmentTraceFaceCount;
      std::vector<RadiusSampleEvidence> orderedSamples = traceAlignmentAccepted
          ? face.radiusSamples : std::vector<RadiusSampleEvidence>{};
      if (!orderedSamples.empty())
        traceFaces.push_back({faceId, faceIndex, std::move(orderedSamples)});
    }
    if (!traceFaces.empty()) {
      profile.orientationTraceFaceCount = static_cast<int>(traceFaces.size());
      const auto endpoint = [](const TraceFaceSamples& trace, int orientation,
                               bool atEnd) -> const RadiusSampleEvidence& {
        const bool useBack = (orientation == 0) == atEnd;
        return useBack ? trace.samples.back() : trace.samples.front();
      };
      const auto orientedDirection = [&](const TraceFaceSamples& trace, int orientation,
                                         bool atEnd) {
        const auto& sample = endpoint(trace, orientation, atEnd);
        gp_Vec direction(sample.spineDirectionX, sample.spineDirectionY,
                         sample.spineDirectionZ);
        if (orientation == 1) direction.Reverse();
        return direction;
      };
      const double directionWeight = options.radiusProfileDirectionCostWeight;
      const double distanceScale = std::max(result.diagnostics.modelDiagonal, 1.0e-14);
      const auto transitionGap = [&](std::size_t firstIndex, int firstOrientation,
                                     std::size_t secondIndex, int secondOrientation) {
        return sampleDistance(endpoint(traceFaces[firstIndex], firstOrientation, true),
                              endpoint(traceFaces[secondIndex], secondOrientation, false)) /
               distanceScale;
      };
      const auto transitionAlignment = [&](std::size_t firstIndex, int firstOrientation,
                                           std::size_t secondIndex, int secondOrientation) {
        const gp_Vec firstDirection = orientedDirection(
            traceFaces[firstIndex], firstOrientation, true);
        const gp_Vec secondDirection = orientedDirection(
            traceFaces[secondIndex], secondOrientation, false);
        if (firstDirection.SquareMagnitude() <= 1.0e-28 ||
            secondDirection.SquareMagnitude() <= 1.0e-28)
          return -1.0;
        return std::max(-1.0, std::min(1.0,
            gp_Dir(firstDirection).Dot(gp_Dir(secondDirection))));
      };
      struct AnalyticBridgeOrientationMetric {
        bool attempted = false;
        bool available = false;
        double signedAlignment = -1.0;
        double normalizedGap = 0.0;
        double cost = 0.0;
      };
      const auto bridgeFaceIds = [&](std::size_t firstIndex,
                                     std::size_t secondIndex,
                                     bool closure) {
        std::vector<int> faces;
        if (closure) {
          for (std::size_t index = traceFaces[firstIndex].pathFaceIndex;
               index < path.faceIds.size(); ++index)
            faces.push_back(path.faceIds[index]);
          for (std::size_t index = 0;
               index <= traceFaces[secondIndex].pathFaceIndex; ++index)
            faces.push_back(path.faceIds[index]);
        } else {
          for (std::size_t index = traceFaces[firstIndex].pathFaceIndex;
               index <= traceFaces[secondIndex].pathFaceIndex; ++index)
            faces.push_back(path.faceIds[index]);
        }
        return faces;
      };
      struct LinkAxisAnchor {
        gp_Vec axis;
        double u = 0.0;
        double v = 0.0;
      };
      const auto representativeLinkAxis = [](const Link& link, int faceId)
          -> std::optional<LinkAxisAnchor> {
        gp_Vec sum;
        std::optional<gp_Dir> reference;
        double sumU = 0.0, sumV = 0.0;
        int count = 0;
        for (const DirectionSampleEvidence& sample : link.directionSamples) {
          gp_Vec axis;
          double u = 0.0, v = 0.0;
          if (faceId == link.firstFaceId) {
            axis.SetCoord(sample.firstDirectionX, sample.firstDirectionY,
                          sample.firstDirectionZ);
            u = sample.firstU; v = sample.firstV;
          } else if (faceId == link.secondFaceId) {
            axis.SetCoord(sample.secondDirectionX, sample.secondDirectionY,
                          sample.secondDirectionZ);
            u = sample.secondU; v = sample.secondV;
          } else {
            continue;
          }
          if (axis.SquareMagnitude() <= 1.0e-28) continue;
          const gp_Dir direction(axis);
          if (!reference) reference = direction;
          if (direction.Dot(*reference) < 0.0) axis.Reverse();
          sum += axis.Normalized();
          sumU += u; sumV += v; ++count;
        }
        if (sum.SquareMagnitude() <= 1.0e-28 || count == 0) return std::nullopt;
        return LinkAxisAnchor{sum.Normalized(), sumU / count, sumV / count};
      };
      const auto analyticBridgeMetric = [&](std::size_t firstIndex,
                                            int firstOrientation,
                                            std::size_t secondIndex,
                                            int secondOrientation,
                                            bool closure) {
        AnalyticBridgeOrientationMetric metric;
        const std::vector<int> faces = bridgeFaceIds(firstIndex, secondIndex, closure);
        if (faces.size() <= 2) return metric;
        for (std::size_t index = 1; index + 1 < faces.size(); ++index) {
          const auto faceEvidence = acceptedByFaceId.find(faces[index]);
          if (faceEvidence == acceptedByFaceId.end() ||
              !hasAnalyticBandGeometry(result.faces[faceEvidence->second]))
            return metric;
        }
        metric.attempted = true;
        gp_Vec current = orientedDirection(traceFaces[firstIndex],
                                            firstOrientation, true);
        if (current.SquareMagnitude() <= 1.0e-28) return metric;
        double sign = 1.0;
        double minimumMagnitude = 1.0;
        const auto accumulateAxis = [&](const gp_Vec& next, double magnitudeCap,
                                        bool penalizeMagnitude = true) {
          if (next.SquareMagnitude() <= 1.0e-28 ||
              current.SquareMagnitude() <= 1.0e-28)
            return false;
          const double dot = gp_Dir(current).Dot(gp_Dir(next));
          if (dot < 0.0) sign = -sign;
          if (penalizeMagnitude)
            minimumMagnitude = std::min(minimumMagnitude,
                                        std::min(std::abs(dot), magnitudeCap));
          current = next;
          return true;
        };
        const auto transportAcrossAnalyticFace = [&](int faceId,
                                                     const LinkAxisAnchor& from,
                                                     const LinkAxisAnchor& to) {
          const auto faceEvidence = acceptedByFaceId.find(faceId);
          if (faceEvidence == acceptedByFaceId.end()) return false;
          try {
            BRepAdaptor_Surface surface(
                result.faces[faceEvidence->second].face, Standard_True);
            double deltaU = to.u - from.u;
            if (surface.IsUPeriodic()) {
              const double period = surface.UPeriod();
              if (deltaU > 0.5 * period) deltaU -= period;
              if (deltaU < -0.5 * period) deltaU += period;
            }
            double deltaV = to.v - from.v;
            if (surface.IsVPeriodic()) {
              const double period = surface.VPeriod();
              if (deltaV > 0.5 * period) deltaV -= period;
              if (deltaV < -0.5 * period) deltaV += period;
            }
            constexpr int transportSteps = 12;
            for (int step = 1; step < transportSteps; ++step) {
              const double ratio = static_cast<double>(step) / transportSteps;
              BRepLProp_SLProps properties(
                  surface, from.u + ratio * deltaU, from.v + ratio * deltaV,
                  2, result.diagnostics.effectiveLinearTolerance);
              const auto spine = principalSpineDirection(
                  properties, options.maximumUmbilicCurvatureRatioForDirection);
              if (!spine || !accumulateAxis(gp_Vec(spine->direction), 1.0, false))
                return false;
            }
            return accumulateAxis(to.axis, 1.0, false);
          } catch (const Standard_Failure&) {
            return false;
          }
        };
        std::optional<LinkAxisAnchor> previousSecondAnchor;
        for (std::size_t hop = 1; hop < faces.size(); ++hop) {
          const Link* selected = nullptr;
          bool insufficient = false;
          for (std::size_t linkIndex :
               linkIndexesForFaces(faces[hop - 1], faces[hop])) {
            const Link& link = result.links[linkIndex];
            if (!link.usedInGraph) continue;
            if (link.spineDirectionRequestedSamples > 0 &&
                !link.spineDirectionEvaluated)
              insufficient = true;
            if (!link.spineDirectionEvaluated || link.directionSamples.empty()) continue;
            if (!selected || link.minimumSpineDirectionAlignment <
                                 selected->minimumSpineDirectionAlignment)
              selected = &link;
          }
          if (!selected || insufficient) return metric;
          const auto firstAxis = representativeLinkAxis(*selected, faces[hop - 1]);
          const auto secondAxis = representativeLinkAxis(*selected, faces[hop]);
          if (!firstAxis || !secondAxis) return metric;
          const bool reachedFirstAxis = previousSecondAnchor
              ? transportAcrossAnalyticFace(faces[hop - 1],
                                            *previousSecondAnchor, *firstAxis)
              : accumulateAxis(firstAxis->axis, 1.0);
          if (!reachedFirstAxis ||
              !accumulateAxis(secondAxis->axis,
                              std::max(0.0, selected->minimumSpineDirectionAlignment)))
            return metric;
          previousSecondAnchor = secondAxis;
          double hopGap = 0.0;
          if (!selected->directionSamples.empty()) {
            hopGap = std::numeric_limits<double>::infinity();
            for (const auto& sample : selected->directionSamples)
              hopGap = std::min(hopGap, sample.gap);
            if (!std::isfinite(hopGap)) hopGap = 0.0;
          }
          metric.normalizedGap += hopGap / distanceScale;
        }
        const gp_Vec target = orientedDirection(traceFaces[secondIndex],
                                                 secondOrientation, false);
        if (!accumulateAxis(target, 1.0)) return metric;
        metric.available = true;
        metric.signedAlignment = sign * minimumMagnitude;
        metric.cost = metric.normalizedGap +
                      directionWeight * (1.0 - metric.signedAlignment);
        return metric;
      };
      const auto transitionCost = [&](std::size_t firstIndex, int firstOrientation,
                                      std::size_t secondIndex, int secondOrientation) {
        bool closure = path.isClosed && firstIndex + 1 == traceFaces.size() &&
                       secondIndex == 0;
        const AnalyticBridgeOrientationMetric bridge = analyticBridgeMetric(
            firstIndex, firstOrientation, secondIndex, secondOrientation, closure);
        if (bridge.available) return bridge.cost;
        const double gap = transitionGap(firstIndex, firstOrientation,
                                         secondIndex, secondOrientation);
        const double alignment = transitionAlignment(firstIndex, firstOrientation,
                                                      secondIndex, secondOrientation);
        return gap + directionWeight * (1.0 - alignment);
      };
      const auto totalOrientationCost = [&](const std::vector<int>& orientations) {
        double cost = 0.0;
        for (std::size_t index = 1; index < traceFaces.size(); ++index)
          cost += transitionCost(index - 1, orientations[index - 1],
                                 index, orientations[index]);
        if (path.isClosed && traceFaces.size() > 1)
          cost += transitionCost(traceFaces.size() - 1, orientations.back(),
                                 0, orientations.front());
        return cost;
      };
      const auto greedyOrientations = [&](int firstOrientation) {
        std::vector<int> orientations(traceFaces.size(), 0);
        orientations[0] = firstOrientation;
        for (std::size_t index = 1; index < traceFaces.size(); ++index) {
          const double forward = transitionCost(index - 1, orientations[index - 1],
                                                index, 0);
          const double reverse = transitionCost(index - 1, orientations[index - 1],
                                                index, 1);
          orientations[index] = reverse < forward ? 1 : 0;
        }
        return orientations;
      };
      std::vector<int> greedy = greedyOrientations(0);
      std::vector<int> alternateGreedy = greedyOrientations(1);
      if (totalOrientationCost(alternateGreedy) < totalOrientationCost(greedy))
        greedy = std::move(alternateGreedy);
      profile.greedyOrientationTransitionCost = totalOrientationCost(greedy);

      const double infinity = std::numeric_limits<double>::infinity();
      std::vector<int> bestOrientations(traceFaces.size(), 0);
      double bestCost = infinity;
      for (int fixedFirst = 0; fixedFirst < (path.isClosed ? 2 : 1); ++fixedFirst) {
        std::vector<std::array<double, 2>> cost(traceFaces.size(),
                                                {infinity, infinity});
        std::vector<std::array<int, 2>> parent(traceFaces.size(), {-1, -1});
        if (path.isClosed)
          cost[0][fixedFirst] = 0.0;
        else
          cost[0] = {0.0, 0.0};
        for (std::size_t index = 1; index < traceFaces.size(); ++index) {
          for (int orientation = 0; orientation < 2; ++orientation) {
            for (int previous = 0; previous < 2; ++previous) {
              const double candidate = cost[index - 1][previous] +
                  transitionCost(index - 1, previous, index, orientation);
              if (candidate < cost[index][orientation]) {
                cost[index][orientation] = candidate;
                parent[index][orientation] = previous;
              }
            }
          }
        }
        for (int last = 0; last < 2; ++last) {
          double candidate = cost.back()[last];
          if (path.isClosed && traceFaces.size() > 1)
            candidate += transitionCost(traceFaces.size() - 1, last, 0, fixedFirst);
          if (candidate >= bestCost) continue;
          bestCost = candidate;
          bestOrientations.back() = last;
          for (std::size_t index = traceFaces.size() - 1; index > 0; --index)
            bestOrientations[index - 1] = parent[index][bestOrientations[index]];
        }
      }
      profile.globalOrientationOptimized = traceFaces.size() > 1;
      profile.orientationTransitionCost = std::isfinite(bestCost) ? bestCost : 0.0;
      if (path.isClosed && traceFaces.size() > 1) {
        const auto& last = endpoint(traceFaces.back(), bestOrientations.back(), true);
        const auto& first = endpoint(traceFaces.front(), bestOrientations.front(), false);
        profile.orientationClosureGap = sampleDistance(last, first);
      }
      const auto appendTransitionEvidence = [&](std::size_t firstIndex,
                                                std::size_t secondIndex,
                                                bool closure) {
        RadiusProfile::OrientationTransitionEvidence evidence;
        evidence.firstFaceId = traceFaces[firstIndex].faceId;
        evidence.secondFaceId = traceFaces[secondIndex].faceId;
        evidence.closure = closure;
        if (closure) {
          evidence.skippedFaceCount = static_cast<int>(
              path.faceIds.size() - 1 - traceFaces[firstIndex].pathFaceIndex +
              traceFaces[secondIndex].pathFaceIndex);
        } else {
          evidence.skippedFaceCount = static_cast<int>(
              traceFaces[secondIndex].pathFaceIndex -
              traceFaces[firstIndex].pathFaceIndex - 1);
        }
        evidence.normalizedGap = transitionGap(
            firstIndex, bestOrientations[firstIndex],
            secondIndex, bestOrientations[secondIndex]);
        evidence.directionAlignment = transitionAlignment(
            firstIndex, bestOrientations[firstIndex],
            secondIndex, bestOrientations[secondIndex]);
        const AnalyticBridgeOrientationMetric optimizedBridge = analyticBridgeMetric(
            firstIndex, bestOrientations[firstIndex], secondIndex,
            bestOrientations[secondIndex], closure);
        evidence.analyticBridgeIncludedInOptimization = optimizedBridge.available;
        evidence.analyticBridgeSignedAlignment = optimizedBridge.signedAlignment;
        evidence.analyticBridgeOrientationCost = optimizedBridge.cost;
        if (optimizedBridge.available)
          ++profile.analyticBridgeOptimizedOrientationTransitionCount;
        evidence.cost = transitionCost(
            firstIndex, bestOrientations[firstIndex],
            secondIndex, bestOrientations[secondIndex]);
        if (evidence.skippedFaceCount > 0) {
          const std::vector<int> bridgeFaces =
              bridgeFaceIds(firstIndex, secondIndex, closure);
          bool allIntermediateAnalytic = true;
          for (std::size_t index = 1; index + 1 < bridgeFaces.size(); ++index) {
            const auto faceEvidence = acceptedByFaceId.find(bridgeFaces[index]);
            if (faceEvidence == acceptedByFaceId.end() ||
                !hasAnalyticBandGeometry(result.faces[faceEvidence->second])) {
              allIntermediateAnalytic = false;
              break;
            }
          }
          evidence.analyticBridgeAttempted = allIntermediateAnalytic;
          bool bridgeConflict = false, bridgeInsufficient = false;
          if (evidence.analyticBridgeAttempted) {
            evidence.analyticBridgeHopCount =
                static_cast<int>(bridgeFaces.size()) - 1;
            for (std::size_t hop = 1; hop < bridgeFaces.size(); ++hop) {
              bool hopEvaluated = false, hopInsufficient = false,
                   hopConflict = false;
              for (std::size_t linkIndex :
                   linkIndexesForFaces(bridgeFaces[hop - 1], bridgeFaces[hop])) {
                const Link& link = result.links[linkIndex];
                if (!link.usedInGraph) continue;
                if (link.spineDirectionRequestedSamples > 0 &&
                    !link.spineDirectionEvaluated)
                  hopInsufficient = true;
                if (!link.spineDirectionEvaluated) continue;
                hopEvaluated = true;
                if (evidence.minimumAnalyticBridgeAlignment < 0.0)
                  evidence.minimumAnalyticBridgeAlignment =
                      link.minimumSpineDirectionAlignment;
                else
                  evidence.minimumAnalyticBridgeAlignment = std::min(
                      evidence.minimumAnalyticBridgeAlignment,
                      link.minimumSpineDirectionAlignment);
                if (!link.spineDirectionCompatible) hopConflict = true;
              }
              if (hopEvaluated) ++evidence.analyticBridgeEvaluatedHopCount;
              if (!hopEvaluated || hopInsufficient) bridgeInsufficient = true;
              if (hopConflict) bridgeConflict = true;
            }
            if (optimizedBridge.available &&
                optimizedBridge.signedAlignment <
                    options.minimumRadiusProfileTransitionAlignment)
              bridgeConflict = true;
            evidence.analyticBridgeValidated = !bridgeConflict && !bridgeInsufficient &&
                evidence.analyticBridgeEvaluatedHopCount ==
                    evidence.analyticBridgeHopCount;
          }
          if (evidence.analyticBridgeValidated) {
            evidence.accepted = true;
            evidence.issue = OrientationTransitionIssue::None;
            ++profile.acceptedOrientationTransitionCount;
            ++profile.analyticBridgedOrientationTransitionCount;
          } else if (bridgeConflict) {
            evidence.issue = OrientationTransitionIssue::DirectionMismatch;
            ++profile.directionConflictOrientationTransitionCount;
            ++profile.analyticBridgeConflictOrientationTransitionCount;
          } else {
            evidence.issue = OrientationTransitionIssue::UnobservedIntermediateFaces;
            ++profile.unobservedOrientationTransitionCount;
          }
        } else {
          const bool gapConflict = evidence.normalizedGap >
              options.maximumRadiusProfileTransitionGapToModelDiagonal;
          const bool directionConflict = evidence.directionAlignment <
              options.minimumRadiusProfileTransitionAlignment;
          if (gapConflict) ++profile.gapConflictOrientationTransitionCount;
          if (directionConflict) ++profile.directionConflictOrientationTransitionCount;
          evidence.accepted = !gapConflict && !directionConflict;
          if (evidence.accepted) {
            evidence.issue = OrientationTransitionIssue::None;
            ++profile.acceptedOrientationTransitionCount;
          } else if (gapConflict && directionConflict) {
            evidence.issue = OrientationTransitionIssue::GapAndDirectionMismatch;
          } else if (gapConflict) {
            evidence.issue = OrientationTransitionIssue::GapTooLarge;
          } else {
            evidence.issue = OrientationTransitionIssue::DirectionMismatch;
          }
        }
        profile.orientationTransitions.push_back(evidence);
      };
      for (std::size_t index = 1; index < traceFaces.size(); ++index)
        appendTransitionEvidence(index - 1, index, false);
      if (path.isClosed && traceFaces.size() > 1)
        appendTransitionEvidence(traceFaces.size() - 1, 0, true);
      profile.orientationTransitionsValidated =
          !profile.orientationTransitions.empty() &&
          profile.acceptedOrientationTransitionCount ==
              static_cast<int>(profile.orientationTransitions.size());
      for (std::size_t index = 0; index < traceFaces.size(); ++index) {
        auto samples = std::move(traceFaces[index].samples);
        if (bestOrientations[index] == 1) {
          ++profile.orientationReversedFaceCount;
          std::reverse(samples.begin(), samples.end());
          for (auto& sample : samples) {
            sample.spineDirectionX = -sample.spineDirectionX;
            sample.spineDirectionY = -sample.spineDirectionY;
            sample.spineDirectionZ = -sample.spineDirectionZ;
          }
        }
        for (const auto& sample : samples)
          profile.samples.push_back({traceFaces[index].faceId, 0.0, sample});
      }
    }
    if (!profile.nominalRadii.empty()) {
      profile.startRadius = profile.nominalRadii.front();
      profile.endRadius = profile.nominalRadii.back();
      profile.minimumRadius = *std::min_element(profile.minimumRadii.begin(),
                                                profile.minimumRadii.end());
      profile.maximumRadius = *std::max_element(profile.maximumRadii.begin(),
                                                profile.maximumRadii.end());
      for (std::size_t index = 1; index < profile.nominalRadii.size(); ++index) {
        const double first = profile.nominalRadii[index - 1];
        const double second = profile.nominalRadii[index];
        const double scale = std::max(0.5 * (std::abs(first) + std::abs(second)),
                                      1.0e-14);
        profile.maximumRelativeStep = std::max(
            profile.maximumRelativeStep, std::abs(first - second) / scale);
      }
      double behaviorSpread = 0.0;
      double behaviorStep = profile.maximumRelativeStep;
      if (static_cast<int>(profile.samples.size()) >= options.minimumRadiusTraceSamples) {
        std::vector<double> accumulated(profile.samples.size(), 0.0);
        std::vector<double> signedSteps;
        double sampledMinimum = profile.samples.front().evidence.radius;
        double sampledMaximum = sampledMinimum;
        double sampledSum = sampledMinimum;
        for (std::size_t index = 1; index < profile.samples.size(); ++index) {
          accumulated[index] = accumulated[index - 1] + sampleDistance(
              profile.samples[index - 1].evidence, profile.samples[index].evidence);
          const double first = profile.samples[index - 1].evidence.radius;
          const double second = profile.samples[index].evidence.radius;
          const double difference = std::abs(first - second);
          signedSteps.push_back(second - first);
          const double scale = std::max(0.5 * (std::abs(first) + std::abs(second)),
                                        1.0e-14);
          profile.maximumSampleRelativeStep = std::max(
              profile.maximumSampleRelativeStep, difference / scale);
          profile.sampledTotalVariation += difference;
          sampledMinimum = std::min(sampledMinimum, second);
          sampledMaximum = std::max(sampledMaximum, second);
          sampledSum += second;
        }
        const double totalLength = accumulated.back();
        for (std::size_t index = 0; index < profile.samples.size(); ++index)
          profile.samples[index].pathParameter = totalLength >
                  result.diagnostics.effectiveLinearTolerance
              ? accumulated[index] / totalLength
              : (profile.samples.size() > 1
                    ? static_cast<double>(index) / (profile.samples.size() - 1) : 0.0);
        const double sampledMean = sampledSum / profile.samples.size();
        behaviorSpread = (sampledMaximum - sampledMinimum) /
                         std::max(std::abs(sampledMean), 1.0e-14);
        behaviorStep = profile.maximumSampleRelativeStep;
        const double averageVariation = signedSteps.empty() ? 0.0
            : profile.sampledTotalVariation / signedSteps.size();
        const double activeThreshold = averageVariation * 0.20;
        int activeSteps = 0, positiveSteps = 0, negativeSteps = 0;
        double dominantVariation = 0.0;
        bool crossFaceDiscontinuity = false;
        for (std::size_t index = 0; index < signedSteps.size(); ++index) {
          const double magnitude = std::abs(signedSteps[index]);
          dominantVariation = std::max(dominantVariation, magnitude);
          if (magnitude > activeThreshold) {
            ++activeSteps;
            if (signedSteps[index] > 0.0) ++positiveSteps;
            else ++negativeSteps;
          }
          const double first = profile.samples[index].evidence.radius;
          const double second = profile.samples[index + 1].evidence.radius;
          const double scale = std::max(0.5 * (std::abs(first) + std::abs(second)),
                                        1.0e-14);
          if (profile.samples[index].faceId != profile.samples[index + 1].faceId &&
              magnitude / scale >= options.discontinuousRadiusRelativeStep)
            crossFaceDiscontinuity = true;
        }
        profile.dominantStepVariationFraction = profile.sampledTotalVariation > 0.0
            ? dominantVariation / profile.sampledTotalVariation : 0.0;
        profile.activeStepFraction = signedSteps.empty() ? 0.0
            : static_cast<double>(activeSteps) / signedSteps.size();
        profile.monotonicStepFraction = activeSteps > 0
            ? static_cast<double>(std::max(positiveSteps, negativeSteps)) / activeSteps : 0.0;
        profile.startRadius = profile.samples.front().evidence.radius;
        profile.endRadius = profile.samples.back().evidence.radius;
        if (behaviorSpread <= options.constantRadiusRelativeSpread)
          profile.behavior = RadiusBehavior::Constant;
        else if (crossFaceDiscontinuity ||
                 (behaviorStep >= options.discontinuousRadiusRelativeStep &&
                  profile.dominantStepVariationFraction >=
                      options.minimumDiscontinuityVariationFraction))
          profile.behavior = RadiusBehavior::Discontinuous;
        else if (profile.activeStepFraction >= options.minimumSmoothActiveStepFraction &&
                 profile.monotonicStepFraction >=
                     options.minimumSmoothMonotonicStepFraction)
          profile.behavior = RadiusBehavior::SmoothVariable;
        else
          profile.behavior = RadiusBehavior::Segmented;
        // A non-constant law is promoted only when every contributing
        // freeform trace has independent cross-width support.  Keep the
        // samples for audit, but do not present an unstable law as a result.
        if (profile.behavior != RadiusBehavior::Constant &&
            (profile.unstableAcrossSeedsTraceFaceCount > 0 ||
             profile.insufficientSeedStabilityTraceFaceCount > 0))
          profile.behavior = RadiusBehavior::InsufficientEvidence;
      } else {
        const double nominalMean = std::accumulate(profile.nominalRadii.begin(),
                                                   profile.nominalRadii.end(), 0.0) /
                                   profile.nominalRadii.size();
        behaviorSpread = (profile.maximumRadius - profile.minimumRadius) /
            std::max(std::abs(nominalMean), 1.0e-14);
        if (profile.variableFaceCount == 0 &&
            behaviorSpread <= options.constantRadiusRelativeSpread)
          profile.behavior = RadiusBehavior::Constant;
        else
          profile.behavior = RadiusBehavior::InsufficientEvidence;
      }
    }
    result.radiusProfiles.push_back(std::move(profile));
  }

  const auto radiusProfileEnd = PerformanceClock::now();
  result.performance.radiusProfileMilliseconds =
      elapsedMilliseconds(topologyEnd, radiusProfileEnd);


  std::map<int, int> chainByFace;
  for (std::size_t chainIndex = 0; chainIndex < result.chains.size(); ++chainIndex)
    for (int faceId : result.chains[chainIndex].faceIds)
      chainByFace[faceId] = static_cast<int>(chainIndex);
  std::vector<std::vector<int>> pathsByChain(result.chains.size());
  for (std::size_t pathIndex = 0; pathIndex < result.paths.size(); ++pathIndex) {
    if (result.paths[pathIndex].faceIds.empty()) continue;
    const auto owner = chainByFace.find(result.paths[pathIndex].faceIds.front());
    if (owner != chainByFace.end())
      pathsByChain[owner->second].push_back(static_cast<int>(pathIndex + 1));
  }
  for (std::size_t chainIndex = 0; chainIndex < result.chains.size(); ++chainIndex) {
    const Chain& chain = result.chains[chainIndex];
    Feature feature;
    feature.chainId = static_cast<int>(chainIndex + 1);
    feature.verdict = chain.verdict;
    feature.faceIds = chain.faceIds;
    feature.pathIds = pathsByChain[chainIndex];
    feature.radiusProfileIds = feature.pathIds;
    feature.externalSupportFaceIds = chain.supportFaceIds;
    feature.minimumRadius = chain.minimumRadius;
    feature.maximumRadius = chain.maximumRadius;
    feature.baseGeometryConfidence = chain.confidence;
    for (int faceId : chain.faceIds) {
      const auto& evidence = result.faces[acceptedByFaceId[faceId]];
      switch (evidence.topologyRole) {
        case TopologyRole::IsolatedPatch: feature.isolatedFaceIds.push_back(faceId); break;
        case TopologyRole::TerminalPatch: feature.terminalFaceIds.push_back(faceId); break;
        case TopologyRole::BandPatch: feature.bandFaceIds.push_back(faceId); break;
        case TopologyRole::JunctionPatch: feature.junctionFaceIds.push_back(faceId); break;
        case TopologyRole::Unknown: break;
      }
      if (evidence.roleConsistency == RoleConsistency::NeedsReview)
        feature.roleReviewFaceIds.push_back(faceId);
      else if (evidence.roleConsistency == RoleConsistency::Unknown)
        feature.roleUnknownFaceIds.push_back(faceId);
    }
    if (!feature.junctionFaceIds.empty())
      feature.kind = FeatureKind::CompositeJunctionNetwork;
    else if (!feature.isolatedFaceIds.empty() && feature.faceIds.size() == 1)
      feature.kind = FeatureKind::IsolatedPatch;
    else {
      bool graphClosed = !feature.faceIds.empty();
      for (int faceId : feature.faceIds)
        graphClosed = graphClosed && graph[faceId].size() == 2;
      feature.kind = graphClosed ? FeatureKind::ClosedLoop : FeatureKind::SimpleChain;
    }
    feature.geometryRoleValidated = feature.roleReviewFaceIds.empty() &&
                                    feature.roleUnknownFaceIds.empty();
    feature.topologyEvidenceState = !feature.roleReviewFaceIds.empty()
        ? EvidenceState::Conflict
        : (chain.verdict != ChainVerdict::Accepted ||
           !feature.roleUnknownFaceIds.empty()
              ? EvidenceState::InsufficientEvidence
              : EvidenceState::Validated);
    feature.radiusEvidenceState = EvidenceState::Validated;
    int acceptedOrientationTransitions = 0;
    for (int profileId : feature.radiusProfileIds) {
      const RadiusProfile& profile = result.radiusProfiles[profileId - 1];
      const RadiusBehavior behavior = profile.behavior;
      if (radiusBehaviorSeverity(behavior) >
          radiusBehaviorSeverity(feature.radiusBehavior))
        feature.radiusBehavior = behavior;
      if (profile.unstableAcrossSeedsTraceFaceCount > 0 ||
          profile.rejectedLowAlignmentTraceFaceCount > 0) {
        ++feature.radiusUnstableProfileCount;
        feature.radiusEvidenceState = EvidenceState::Conflict;
      } else if (profile.insufficientSeedStabilityTraceFaceCount > 0 ||
                 behavior == RadiusBehavior::InsufficientEvidence) {
        ++feature.radiusInsufficientProfileCount;
        if (feature.radiusEvidenceState != EvidenceState::Conflict)
          feature.radiusEvidenceState = EvidenceState::InsufficientEvidence;
      }
      acceptedOrientationTransitions += profile.acceptedOrientationTransitionCount;
      feature.orientationConflictTransitionCount += static_cast<int>(
          profile.orientationTransitions.size()) -
          profile.acceptedOrientationTransitionCount -
          profile.unobservedOrientationTransitionCount;
      feature.orientationUnobservedTransitionCount +=
          profile.unobservedOrientationTransitionCount;
      feature.orientationAnalyticBridgedTransitionCount +=
          profile.analyticBridgedOrientationTransitionCount;
    }
    const std::set<int> featureFaces(feature.faceIds.begin(), feature.faceIds.end());
    double alignmentSum = 0.0, coverageSum = 0.0;
    for (std::size_t linkIndex = 0; linkIndex < result.links.size(); ++linkIndex) {
      const Link& link = result.links[linkIndex];
      if (!link.usedInGraph || !featureFaces.count(link.firstFaceId) ||
          !featureFaces.count(link.secondFaceId)) continue;
      if (link.spineDirectionRequestedSamples > 0 && !link.spineDirectionEvaluated) {
        feature.directionInsufficientCoverageLinkIds.push_back(static_cast<int>(linkIndex + 1));
        continue;
      }
      if (!link.spineDirectionEvaluated) continue;
      ++feature.directionEvaluatedLinkCount;
      alignmentSum += link.spineDirectionAlignment;
      coverageSum += link.spineDirectionSampleCoverage;
      if (feature.minimumSpineDirectionAlignment < 0.0)
        feature.minimumSpineDirectionAlignment = link.minimumSpineDirectionAlignment;
      else
        feature.minimumSpineDirectionAlignment = std::min(
            feature.minimumSpineDirectionAlignment, link.minimumSpineDirectionAlignment);
      if (!link.spineDirectionCompatible)
        feature.directionConflictLinkIds.push_back(static_cast<int>(linkIndex + 1));
    }
    if (feature.directionEvaluatedLinkCount > 0) {
      feature.meanSpineDirectionAlignment = alignmentSum / feature.directionEvaluatedLinkCount;
      feature.meanSpineDirectionSampleCoverage = coverageSum /
                                                 feature.directionEvaluatedLinkCount;
    }
    feature.spineDirectionValidated = feature.directionEvaluatedLinkCount > 0 &&
        feature.directionConflictLinkIds.empty() &&
        feature.directionInsufficientCoverageLinkIds.empty();
    if (!feature.directionConflictLinkIds.empty() ||
        feature.orientationConflictTransitionCount > 0) {
      feature.directionEvidenceState = EvidenceState::Conflict;
    } else if (!feature.directionInsufficientCoverageLinkIds.empty() ||
               feature.orientationUnobservedTransitionCount > 0) {
      feature.directionEvidenceState = EvidenceState::InsufficientEvidence;
    } else if (feature.directionEvaluatedLinkCount > 0 ||
               acceptedOrientationTransitions > 0 ||
               feature.faceIds.size() == 1) {
      feature.directionEvidenceState = EvidenceState::Validated;
    } else {
      feature.directionEvidenceState = EvidenceState::InsufficientEvidence;
    }
    feature.aggregateEvidenceState = feature.topologyEvidenceState;
    if (evidenceStateSeverity(feature.radiusEvidenceState) >
        evidenceStateSeverity(feature.aggregateEvidenceState))
      feature.aggregateEvidenceState = feature.radiusEvidenceState;
    if (evidenceStateSeverity(feature.directionEvidenceState) >
        evidenceStateSeverity(feature.aggregateEvidenceState))
      feature.aggregateEvidenceState = feature.directionEvidenceState;
    feature.topologyEvidenceScore = evidenceStateScore(
        feature.topologyEvidenceState, options);
    feature.radiusEvidenceScore = evidenceStateScore(
        feature.radiusEvidenceState, options);
    feature.directionEvidenceScore = evidenceStateScore(
        feature.directionEvidenceState, options);
    const double evidenceWeightSum = options.featureTopologyEvidenceWeight +
                                     options.featureRadiusEvidenceWeight +
                                     options.featureDirectionEvidenceWeight;
    feature.aggregateEvidenceScore = (
        options.featureTopologyEvidenceWeight * feature.topologyEvidenceScore +
        options.featureRadiusEvidenceWeight * feature.radiusEvidenceScore +
        options.featureDirectionEvidenceWeight * feature.directionEvidenceScore) /
        evidenceWeightSum;
    feature.confidence = feature.baseGeometryConfidence *
                         feature.aggregateEvidenceScore;
    result.features.push_back(std::move(feature));
  }
  const auto recognitionEnd = PerformanceClock::now();
  result.performance.featureAggregationMilliseconds =
      elapsedMilliseconds(radiusProfileEnd, recognitionEnd);
  result.performance.totalMilliseconds =
      elapsedMilliseconds(recognitionStart, recognitionEnd);
  result.performance.budgetExceeded = result.performance.budgetEnabled &&
      result.performance.totalMilliseconds > result.performance.budgetMilliseconds;
  return result;
}

SewingComparison compareWithSewing(const TopoDS_Shape& shape,
                                   const Options& recognitionOptions,
                                   const SewingOptions& sewingOptions) {
  if (shape.IsNull()) throw std::invalid_argument("compareWithSewing: null shape");
  if (!(sewingOptions.toleranceFactor > 0.0) ||
      !(sewingOptions.maximumToleranceToModelDiagonal > 0.0))
    throw std::invalid_argument("compareWithSewing: sewing tolerances must be positive");

  SewingComparison comparison;
  comparison.original = recognize(shape, recognitionOptions);
  const double requestedTolerance =
      comparison.original.diagnostics.effectiveLinearTolerance *
      sewingOptions.toleranceFactor;
  const double maximumTolerance = comparison.original.diagnostics.modelDiagonal > 0.0
      ? comparison.original.diagnostics.modelDiagonal *
            sewingOptions.maximumToleranceToModelDiagonal
      : requestedTolerance;
  comparison.sewingTolerance = std::min(requestedTolerance, maximumTolerance);

  TopTools_IndexedMapOfShape originalFaceMap;
  TopExp::MapShapes(shape, TopAbs_FACE, originalFaceMap);
  const int faceCount = originalFaceMap.Extent();
  std::vector<int> parents(static_cast<std::size_t>(faceCount + 1));
  std::iota(parents.begin(), parents.end(), 0);
  const auto findRoot = [&](int value) {
    while (parents[value] != value) value = parents[value];
    return value;
  };
  const auto unite = [&](int first, int second) {
    const int firstRoot = findRoot(first), secondRoot = findRoot(second);
    if (firstRoot != secondRoot) parents[secondRoot] = firstRoot;
  };
  if (sewingOptions.partitionByFaceProximity) {
    std::vector<int> fixedPartition(static_cast<std::size_t>(faceCount + 1), 0);
    int fixedPartitionCount = 0;
    TopTools_IndexedMapOfShape solids, shells;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    TopExp::MapShapes(shape, TopAbs_SHELL, shells);
    for (int solidIndex = 1; solidIndex <= solids.Extent(); ++solidIndex) {
      ++fixedPartitionCount;
      for (TopExp_Explorer explorer(solids.FindKey(solidIndex), TopAbs_FACE);
           explorer.More(); explorer.Next()) {
        const int faceId = originalFaceMap.FindIndex(explorer.Current());
        if (faceId > 0) fixedPartition[faceId] = fixedPartitionCount;
      }
    }
    for (int shellIndex = 1; shellIndex <= shells.Extent(); ++shellIndex) {
      std::vector<int> unassigned;
      for (TopExp_Explorer explorer(shells.FindKey(shellIndex), TopAbs_FACE);
           explorer.More(); explorer.Next()) {
        const int faceId = originalFaceMap.FindIndex(explorer.Current());
        if (faceId > 0 && fixedPartition[faceId] == 0) unassigned.push_back(faceId);
      }
      if (!unassigned.empty()) {
        ++fixedPartitionCount;
        for (int faceId : unassigned) fixedPartition[faceId] = fixedPartitionCount;
      }
    }
    for (int first = 1; first <= faceCount; ++first)
      for (int second = first + 1; second <= faceCount; ++second)
        if (fixedPartition[first] != 0 &&
            fixedPartition[first] == fixedPartition[second])
          unite(first, second);

    std::vector<Bnd_Box> boxes(static_cast<std::size_t>(faceCount + 1));
    for (int faceId = 1; faceId <= faceCount; ++faceId) {
      BRepBndLib::Add(TopoDS::Face(originalFaceMap.FindKey(faceId)), boxes[faceId], Standard_False);
      boxes[faceId].Enlarge(comparison.sewingTolerance);
    }
    for (int first = 1; first <= faceCount; ++first)
      for (int second = first + 1; second <= faceCount; ++second)
        if (fixedPartition[first] == 0 && fixedPartition[second] == 0 &&
            !boxes[first].IsOut(boxes[second])) unite(first, second);
  } else {
    for (int faceId = 2; faceId <= faceCount; ++faceId) unite(1, faceId);
  }
  std::map<int, std::vector<int>> partitions;
  for (int faceId = 1; faceId <= faceCount; ++faceId)
    partitions[findRoot(faceId)].push_back(faceId);
  if (partitions.empty()) partitions[0] = {};
  comparison.partitionCount = static_cast<int>(partitions.size());

  BRep_Builder compoundBuilder;
  TopoDS_Compound sewnCompound;
  compoundBuilder.MakeCompound(sewnCompound);
  std::vector<std::unique_ptr<BRepBuilderAPI_Sewing>> sewingEngines;
  std::vector<int> engineByFace(static_cast<std::size_t>(faceCount + 1), -1);
  for (const auto& partition : partitions) {
    comparison.maximumPartitionFaceCount = std::max(
        comparison.maximumPartitionFaceCount, static_cast<int>(partition.second.size()));
    auto sewing = std::make_unique<BRepBuilderAPI_Sewing>(
        comparison.sewingTolerance, Standard_True,
        sewingOptions.analyzeDegeneratedShapes ? Standard_True : Standard_False,
        sewingOptions.cutFreeEdges ? Standard_True : Standard_False,
        Standard_False);
    if (partition.second.empty()) sewing->Add(shape);
    for (int faceId : partition.second) {
      sewing->Add(originalFaceMap.FindKey(faceId));
      engineByFace[faceId] = static_cast<int>(sewingEngines.size());
    }
    sewing->Perform();
    const TopoDS_Shape partitionShape = sewing->SewedShape();
    if (!partitionShape.IsNull()) compoundBuilder.Add(sewnCompound, partitionShape);
    comparison.freeEdgeCount += sewing->NbFreeEdges();
    comparison.contiguousEdgeCount += sewing->NbContigousEdges();
    comparison.multipleEdgeCount += sewing->NbMultipleEdges();
    comparison.deletedFaceCount += sewing->NbDeletedFaces();
    for (int edgeIndex = 1; edgeIndex <= sewing->NbFreeEdges(); ++edgeIndex) {
      GProp_GProps properties;
      BRepGProp::LinearProperties(sewing->FreeEdge(edgeIndex), properties);
      comparison.totalFreeEdgeLength += properties.Mass();
      comparison.maximumFreeEdgeLength = std::max(
          comparison.maximumFreeEdgeLength, properties.Mass());
    }
    sewingEngines.push_back(std::move(sewing));
  }
  comparison.sewnShape = sewnCompound;
  comparison.sewn = recognize(comparison.sewnShape, recognitionOptions);

  const auto countShells = [](const TopoDS_Shape& candidate, int& total, int& closed) {
    TopTools_IndexedMapOfShape shells;
    TopExp::MapShapes(candidate, TopAbs_SHELL, shells);
    total = shells.Extent();
    closed = 0;
    for (int index = 1; index <= shells.Extent(); ++index)
      if (BRep_Tool::IsClosed(shells.FindKey(index))) ++closed;
  };
  countShells(shape, comparison.originalShellCount, comparison.originalClosedShellCount);
  countShells(comparison.sewnShape, comparison.sewnShellCount,
              comparison.sewnClosedShellCount);

  TopTools_IndexedMapOfShape sewnFaceMap;
  TopExp::MapShapes(comparison.sewnShape, TopAbs_FACE, sewnFaceMap);
  std::map<int, std::string> sewnPersistentIds;
  std::map<std::string, std::vector<int>> sewnIdsByPersistentId;
  for (const auto& evidence : comparison.sewn.faces) {
    sewnPersistentIds[evidence.faceId] = evidence.persistentId;
    sewnIdsByPersistentId[evidence.persistentId].push_back(evidence.faceId);
  }
  for (const auto& originalEvidence : comparison.original.faces) {
    SewingFaceMapping mapping;
    mapping.originalFaceId = originalEvidence.faceId;
    mapping.originalPersistentId = originalEvidence.persistentId;
    TopoDS_Shape mappedShape = originalEvidence.face;
    try {
      const int engineIndex = originalEvidence.faceId <= faceCount
          ? engineByFace[originalEvidence.faceId] : -1;
      if (engineIndex >= 0) {
        const auto& sewing = *sewingEngines[engineIndex];
        mapping.modified = sewing.IsModifiedSubShape(originalEvidence.face);
        if (mapping.modified)
          mappedShape = sewing.ModifiedSubShape(originalEvidence.face);
      }
    } catch (const Standard_Failure&) {}
    std::set<int> mappedIds;
    if (!mappedShape.IsNull()) {
      if (mappedShape.ShapeType() == TopAbs_FACE) {
        const int id = sewnFaceMap.FindIndex(mappedShape);
        if (id > 0) mappedIds.insert(id);
      } else {
        for (TopExp_Explorer explorer(mappedShape, TopAbs_FACE); explorer.More(); explorer.Next()) {
          const int id = sewnFaceMap.FindIndex(explorer.Current());
          if (id > 0) mappedIds.insert(id);
        }
      }
    }
    if (mappedIds.empty()) {
      const auto fallback = sewnIdsByPersistentId.find(originalEvidence.persistentId);
      if (fallback != sewnIdsByPersistentId.end())
        mappedIds.insert(fallback->second.begin(), fallback->second.end());
    }
    mapping.sewnFaceIds.assign(mappedIds.begin(), mappedIds.end());
    for (int id : mapping.sewnFaceIds) {
      const auto persistent = sewnPersistentIds.find(id);
      if (persistent != sewnPersistentIds.end())
        mapping.sewnPersistentIds.push_back(persistent->second);
    }
    mapping.mapped = !mapping.sewnFaceIds.empty();
    if (mapping.mapped) ++comparison.mappedFaceCount;
    else ++comparison.unmappedFaceCount;
    if (mapping.sewnFaceIds.size() > 1) ++comparison.splitFaceMappingCount;
    if (mapping.modified) ++comparison.modifiedFaceCount;
    comparison.faceMappings.push_back(std::move(mapping));
  }
  comparison.topologySafetyGatePassed = comparison.sewn.diagnostics.valid &&
      comparison.deletedFaceCount == 0 && comparison.multipleEdgeCount == 0 &&
      comparison.unmappedFaceCount == 0 && comparison.splitFaceMappingCount == 0 &&
      comparison.original.diagnostics.faceCount == comparison.sewn.diagnostics.faceCount;
  return comparison;
}

} // namespace fillet::improved
