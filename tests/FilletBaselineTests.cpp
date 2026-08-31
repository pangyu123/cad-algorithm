#include "fillet/FilletBaseline.hpp"
#include "fillet/ImprovedFilletRecognizer.hpp"
#include "fillet/TruthEvaluation.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp.hxx>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace {
TopoDS_Shape roundedBox(int edgeCount) {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 80.0, 60.0).Shape();
  BRepFilletAPI_MakeFillet builder(box);
  int added = 0;
  for (TopExp_Explorer ex(box, TopAbs_EDGE); ex.More() && added < edgeCount; ex.Next(), ++added)
    builder.Add(5.0, TopoDS::Edge(ex.Current()));
  builder.Build();
  if (!builder.IsDone()) throw std::runtime_error("test fillet construction failed");
  return builder.Shape();
}

TopoDS_Shape variableRoundedBox() {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 80.0, 60.0).Shape();
  BRepFilletAPI_MakeFillet builder(box);
  TopExp_Explorer edge(box, TopAbs_EDGE);
  if (!edge.More()) throw std::runtime_error("test box has no edge");
  builder.Add(3.0, 8.0, TopoDS::Edge(edge.Current()));
  builder.Build();
  if (!builder.IsDone()) throw std::runtime_error("variable fillet construction failed");
  return builder.Shape();
}

TopoDS_Shape closedBSplineFilletRing(bool keepOneAnalyticQuarter = false,
                                     double angularGap = 0.0,
                                     int translatedQuarter = -1,
                                     double translation = 0.0) {
  constexpr double pi = 3.14159265358979323846;
  constexpr double majorRadius = 30.0;
  constexpr double filletRadius = 5.0;
  const gp_Ax3 axes(gp::Origin(), gp::DZ(), gp::DX());
  const Handle(Geom_ToroidalSurface) torus =
      new Geom_ToroidalSurface(axes, majorRadius, filletRadius);
  const Handle(Geom_BSplineSurface) spline =
      GeomConvert::SurfaceToBSplineSurface(torus);
  BRepBuilderAPI_Sewing sewing(1.0e-6, Standard_True, Standard_True,
                               Standard_True, Standard_False);
  for (int quarter = 0; quarter < 4; ++quarter) {
    const double firstU = quarter * 0.5 * pi;
    const double lastU = (quarter + 1) * 0.5 * pi -
                         (quarter == 0 ? angularGap : 0.0);
    TopoDS_Shape patch = keepOneAnalyticQuarter && quarter == 1
        ? BRepBuilderAPI_MakeFace(torus, firstU, lastU, 0.0, pi, 1.0e-7).Face()
        : BRepBuilderAPI_MakeFace(spline, firstU, lastU, 0.0, pi, 1.0e-7).Face();
    if (quarter == translatedQuarter) {
      gp_Trsf transform;
      transform.SetTranslation(gp_Vec(translation, 0.0, 0.0));
      patch = BRepBuilderAPI_Transform(patch, transform, Standard_True).Shape();
    }
    sewing.Add(patch);
  }
  const Handle(Geom_CylindricalSurface) cylinder =
      new Geom_CylindricalSurface(axes, majorRadius + filletRadius);
  sewing.Add(BRepBuilderAPI_MakeFace(cylinder, 0.0, 2.0 * pi, -10.0, 0.0,
                                    1.0e-7).Face());
  const Handle(Geom_CylindricalSurface) innerCylinder =
      new Geom_CylindricalSurface(axes, majorRadius - filletRadius);
  sewing.Add(BRepBuilderAPI_MakeFace(innerCylinder, 0.0, 2.0 * pi, 0.0, 10.0,
                                    1.0e-7).Face());
  sewing.Perform();
  if (sewing.SewedShape().IsNull())
    throw std::runtime_error("closed BSpline fillet ring sewing failed");
  return sewing.SewedShape();
}

TopoDS_Shape sliverFilletPatch() {
  constexpr double pi = 3.14159265358979323846;
  constexpr double span = 1.0e-8;
  const gp_Ax3 axes(gp::Origin(), gp::DZ(), gp::DX());
  const Handle(Geom_ToroidalSurface) torus =
      new Geom_ToroidalSurface(axes, 30.0, 5.0);
  const Handle(Geom_CylindricalSurface) outer =
      new Geom_CylindricalSurface(axes, 35.0);
  const Handle(Geom_CylindricalSurface) inner =
      new Geom_CylindricalSurface(axes, 25.0);
  BRepBuilderAPI_Sewing sewing(1.0e-9, Standard_True, Standard_True,
                               Standard_True, Standard_False);
  sewing.Add(BRepBuilderAPI_MakeFace(torus, 0.0, span, 0.0, pi, 1.0e-10).Face());
  sewing.Add(BRepBuilderAPI_MakeFace(outer, 0.0, span, -10.0, 0.0, 1.0e-10).Face());
  sewing.Add(BRepBuilderAPI_MakeFace(inner, 0.0, span, 0.0, 10.0, 1.0e-10).Face());
  sewing.Perform();
  if (sewing.SewedShape().IsNull())
    throw std::runtime_error("sliver fillet patch sewing failed");
  return sewing.SewedShape();
}

bool check(bool condition, const char* message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}
} // namespace

int main() {
  bool ok = true;
  const auto search = [](const TopoDS_Shape& shape) {
    return fillet::baseline::search(shape, 1.0e-6, std::numeric_limits<double>::max());
  };
  const auto plain = search(BRepPrimAPI_MakeBox(100.0, 80.0, 60.0).Shape());
  ok &= check(plain.empty(), "plain box must have no fillet chains");

  const auto cylinder = search(BRepPrimAPI_MakeCylinder(20.0, 60.0).Shape());
  ok &= check(cylinder.empty(), "ordinary cylinder must fail fillet support validation");

  const auto single = search(roundedBox(1));
  ok &= check(single.size() == 1, "single rounded edge must form one chain");

  const auto connected = search(roundedBox(3));
  ok &= check(!connected.empty(), "connected rounding must produce chains");

  const auto improvedPlain = fillet::improved::recognize(
      BRepPrimAPI_MakeBox(100.0, 80.0, 60.0).Shape());
  ok &= check(improvedPlain.chains.empty(), "improved recognizer must reject plain box");
  const TopoDS_Shape singleShape = roundedBox(1);
  const auto improvedSingle = fillet::improved::recognize(singleShape);
  const auto improvedSingleAgain = fillet::improved::recognize(singleShape);
  ok &= check(!improvedSingle.chains.empty(), "improved recognizer must find single fillet");
  const double measuredPhaseTotal =
      improvedSingle.performance.diagnosticsMilliseconds +
      improvedSingle.performance.faceAnalysisMilliseconds +
      improvedSingle.performance.topologyMilliseconds +
      improvedSingle.performance.radiusProfileMilliseconds +
      improvedSingle.performance.featureAggregationMilliseconds;
  ok &= check(improvedSingle.performance.totalMilliseconds > 0.0 &&
                  std::abs(measuredPhaseTotal -
                           improvedSingle.performance.totalMilliseconds) < 1.0e-6 &&
                  !improvedSingle.performance.budgetEnabled &&
                  !improvedSingle.performance.budgetExceeded,
              "recognition phases must form a complete disabled-budget timing record");
  fillet::improved::Options impossibleBudget;
  impossibleBudget.maximumRecognitionMilliseconds = 1.0e-9;
  const auto budgetExceeded = fillet::improved::recognize(singleShape, impossibleBudget);
  ok &= check(budgetExceeded.performance.budgetEnabled &&
                  budgetExceeded.performance.budgetExceeded,
              "an impossible recognition budget must be reported as exceeded");
  ok &= check(improvedSingle.chains.front().verdict ==
                  fillet::improved::ChainVerdict::Accepted,
              "generated single fillet chain must have two external supports");
  ok &= check(!improvedSingle.paths.empty(), "improved recognizer must decompose fillet paths");
  ok &= check(improvedSingle.radiusProfiles.size() == improvedSingle.paths.size(),
              "every path must expose one ordered radius profile");
  for (const auto& profile : improvedSingle.radiusProfiles)
    ok &= check(profile.behavior == fillet::improved::RadiusBehavior::Constant,
                "generated constant-radius fillet must have constant profiles");
  ok &= check(improvedSingle.features.size() == improvedSingle.chains.size(),
              "every connected chain must produce one semantic feature");
  fillet::improved::Options excludedRadius;
  excludedRadius.minimumRadius = 100.0;
  excludedRadius.maximumRadius = 200.0;
  const auto excludedSingle = fillet::improved::recognize(singleShape, excludedRadius);
  ok &= check(excludedSingle.chains.empty(),
              "nominal radius mode must reject an excluded constant radius");
  excludedRadius.radiusFilterMode = fillet::improved::RadiusFilterMode::AnyRadius;
  const auto unfilteredSingle = fillet::improved::recognize(singleShape, excludedRadius);
  ok &= check(!unfilteredSingle.chains.empty(),
              "any radius mode must bypass numeric radius filtering");
  fillet::improved::Options entireRadius;
  entireRadius.minimumRadius = 4.9;
  entireRadius.maximumRadius = 5.1;
  entireRadius.radiusFilterMode =
      fillet::improved::RadiusFilterMode::EntireProfileWithinRange;
  ok &= check(!fillet::improved::recognize(singleShape, entireRadius).chains.empty(),
              "entire profile mode must accept a contained constant radius");
  fillet::improved::Options intersectingRadius = entireRadius;
  intersectingRadius.minimumRadius = 5.0;
  intersectingRadius.maximumRadius = 6.0;
  intersectingRadius.radiusFilterMode =
      fillet::improved::RadiusFilterMode::ProfileIntersectsRange;
  ok &= check(!fillet::improved::recognize(singleShape, intersectingRadius).chains.empty(),
              "profile intersection mode must accept a touching constant radius");
  const TopoDS_Shape variableShape = variableRoundedBox();
  std::filesystem::create_directories(FILLET_TEST_OUTPUT_DIR);
  ok &= check(BRepTools::Write(variableShape,
                  (std::filesystem::path(FILLET_TEST_OUTPUT_DIR) /
                   "generated-variable-fillet.brep").string().c_str()),
              "generated variable-radius fixture must be persisted for inspection");
  const auto improvedVariable = fillet::improved::recognize(variableShape);
  bool foundRadiusTrace = false, foundStreamlineTrace = false,
       foundStableCrossSeedTrace = false, foundSmoothVariableBehavior = false;
  for (const auto& face : improvedVariable.faces) {
    if (face.verdict != fillet::improved::Verdict::Accepted || face.radiusSamples.empty())
      continue;
    foundRadiusTrace = true;
    if (face.radiusTraceMethod ==
            fillet::improved::RadiusTraceMethod::PrincipalDirectionStreamline &&
        face.radiusTraceCoverage >= 0.5 &&
        face.radiusTraceMinimumTangentAlignment >= 0.95 &&
        face.radiusTraceNormalizedLength >= 0.8 &&
        face.radiusTraceSeedAttempts >= 1)
      foundStreamlineTrace = true;
    if (face.radiusTraceMethod ==
            fillet::improved::RadiusTraceMethod::PrincipalDirectionStreamline &&
        face.radiusTraceValidSeedCount >= 3 &&
        face.radiusTraceStableAcrossSeeds &&
        face.radiusTraceMaximumCrossSeedRelativeDeviation <= 0.10)
      foundStableCrossSeedTrace = true;
    double previous = -1.0;
    for (const auto& sample : face.radiusSamples) {
      ok &= check(sample.radius > 0.0 && sample.normalizedSpineParameter >= previous &&
                      sample.normalizedSpineParameter >= 0.0 &&
                      sample.normalizedSpineParameter <= 1.0,
                  "face R(s) samples must be positive and monotonically parameterized");
      previous = sample.normalizedSpineParameter;
    }
  }
  for (const auto& profile : improvedVariable.radiusProfiles) {
    if (profile.behavior == fillet::improved::RadiusBehavior::SmoothVariable)
      foundSmoothVariableBehavior = true;
    ok &= check(profile.rejectedLowAlignmentTraceFaceCount == 0,
                "generated streamline must pass the path trace alignment gate");
    ok &= check(profile.activeStepFraction >= 0.0 && profile.activeStepFraction <= 1.0 &&
                    profile.monotonicStepFraction >= 0.0 &&
                    profile.monotonicStepFraction <= 1.0 &&
                    profile.dominantStepVariationFraction >= 0.0 &&
                    profile.dominantStepVariationFraction <= 1.0,
                "R(s) variation metrics must be normalized");
    ok &= check(profile.orientationReversedFaceCount >= 0 &&
                    profile.orientationReversedFaceCount <=
                        profile.orientationTraceFaceCount,
                "global trace orientation counts must be internally consistent");
    if (profile.globalOrientationOptimized)
      ok &= check(profile.orientationTransitionCost <=
                      profile.greedyOrientationTransitionCost + 1.0e-12,
                  "global trace orientation must not cost more than greedy orientation");
    double previous = -1.0;
    for (const auto& sample : profile.samples) {
      ok &= check(sample.pathParameter >= previous && sample.pathParameter >= 0.0 &&
                      sample.pathParameter <= 1.0,
                  "path R(s) samples must be monotonically parameterized");
      previous = sample.pathParameter;
    }
  }
  ok &= check(foundRadiusTrace,
              "generated variable-radius fillet must expose a sampled R(s) trace");
  ok &= check(foundStreamlineTrace,
              "generated variable-radius fillet must use the principal-direction streamline");
  ok &= check(foundStableCrossSeedTrace,
              "generated variable-radius fillet must recover a stable R(s) law across seeds");
  ok &= check(foundSmoothVariableBehavior,
              "generated OCCT 3-to-8 radius law must be classified as smooth-variable");
  const TopoDS_Shape closedRingShape = closedBSplineFilletRing();
  ok &= check(BRepTools::Write(closedRingShape,
                  (std::filesystem::path(FILLET_TEST_OUTPUT_DIR) /
                   "generated-closed-bspline-fillet-ring.brep").string().c_str()),
              "generated closed BSpline fillet ring must be persisted for inspection");
  fillet::improved::Options closedRingOptions;
  closedRingOptions.enableCanonicalRecovery = false;
  const auto closedRing = fillet::improved::recognize(closedRingShape, closedRingOptions);
  const int activeClosedRingLinks = static_cast<int>(std::count_if(
      closedRing.links.begin(), closedRing.links.end(),
      [](const fillet::improved::Link& link) { return link.usedInGraph; }));
  ok &= check(closedRing.chains.size() == 1 &&
                  closedRing.chains.front().verdict ==
                      fillet::improved::ChainVerdict::Accepted &&
                  activeClosedRingLinks == 4,
              "closed ring must retain exactly four adjacent links and two supports");
  ok &= check(closedRing.features.size() == 1 &&
                  closedRing.features.front().topologyEvidenceState ==
                      fillet::improved::EvidenceState::Validated &&
                  closedRing.features.front().radiusEvidenceState ==
                      fillet::improved::EvidenceState::Validated &&
                  closedRing.features.front().directionEvidenceState ==
                      fillet::improved::EvidenceState::Validated &&
                  closedRing.features.front().aggregateEvidenceState ==
                      fillet::improved::EvidenceState::Validated &&
                  std::abs(closedRing.features.front().confidence -
                           closedRing.features.front().baseGeometryConfidence) < 1.0e-12,
              "fully evidenced closed ring feature must retain validated confidence");
  bool foundClosedMultiFaceTracePath = false;
  for (std::size_t pathIndex = 0; pathIndex < closedRing.paths.size(); ++pathIndex) {
    const auto& path = closedRing.paths[pathIndex];
    if (!path.isClosed || path.faceIds.size() < 3) continue;
    const auto& profile = closedRing.radiusProfiles[pathIndex];
    const int closureTransitions = static_cast<int>(std::count_if(
        profile.orientationTransitions.begin(), profile.orientationTransitions.end(),
        [](const fillet::improved::RadiusProfile::OrientationTransitionEvidence& transition) {
          return transition.closure;
        }));
    if (profile.orientationTraceFaceCount >= 3 &&
        profile.globalOrientationOptimized &&
        profile.orientationTransitionCost <=
            profile.greedyOrientationTransitionCost + 1.0e-12 &&
        profile.orientationClosureGap >= 0.0 &&
        profile.orientationTransitions.size() == path.faceIds.size() &&
        closureTransitions == 1 && profile.orientationTransitionsValidated)
      foundClosedMultiFaceTracePath = true;
  }
  ok &= check(foundClosedMultiFaceTracePath,
              "closed BSpline fillet ring must exercise cyclic global trace orientation");
  std::vector<fillet::evaluation::TruthRecord> closedRingTruth;
  for (const auto& face : closedRing.faces) {
    fillet::evaluation::TruthRecord truth;
    truth.persistentId = face.persistentId;
    const bool isFillet = BRepAdaptor_Surface(face.face, Standard_True).GetType() ==
                          GeomAbs_BSplineSurface;
    truth.label = isFillet ? fillet::evaluation::TruthLabel::Fillet
                           : fillet::evaluation::TruthLabel::NotFillet;
    if (isFillet) {
      truth.expectedChain = "ring-chain";
      truth.expectedFeature = "ring-feature";
      truth.expectedFeatureKind = "closed-loop";
      truth.expectedRadiusBehavior = "constant";
    }
    closedRingTruth.push_back(std::move(truth));
  }
  const auto closedRingEvaluation =
      fillet::evaluation::evaluate(closedRing, closedRingTruth, 0.50);
  ok &= check(closedRingEvaluation.reviewCoverage == 1.0 &&
                  closedRingEvaluation.faces.truePositive == 4 &&
                  closedRingEvaluation.faces.trueNegative == 2 &&
                  closedRingEvaluation.faces.f1 == 1.0 &&
                  closedRingEvaluation.chains.truePositive == 1 &&
                  closedRingEvaluation.features.truePositive == 1 &&
                  closedRingEvaluation.correctFeatureKindCount == 1 &&
                  closedRingEvaluation.correctRadiusBehaviorCount == 1,
              "independent closed-ring truth must score perfectly at all three levels");
  auto oneFalsePositiveTruth = closedRingTruth;
  const auto firstPositive = std::find_if(
      oneFalsePositiveTruth.begin(), oneFalsePositiveTruth.end(),
      [](const fillet::evaluation::TruthRecord& truth) {
        return truth.label == fillet::evaluation::TruthLabel::Fillet;
      });
  firstPositive->label = fillet::evaluation::TruthLabel::NotFillet;
  firstPositive->expectedChain.clear();
  firstPositive->expectedFeature.clear();
  const auto oneFalsePositiveEvaluation =
      fillet::evaluation::evaluate(closedRing, oneFalsePositiveTruth, 0.50);
  ok &= check(oneFalsePositiveEvaluation.faces.falsePositive == 1 &&
                  std::abs(oneFalsePositiveEvaluation.faces.precision - 0.75) < 1.0e-12,
              "truth metrics must expose a deliberately injected false positive");
  const std::filesystem::path evaluationOutput =
      std::filesystem::path(FILLET_TEST_OUTPUT_DIR) / "truth-evaluation";
  fillet::evaluation::write(closedRingEvaluation, evaluationOutput);
  ok &= check(std::filesystem::exists(evaluationOutput / "evaluation-summary.csv") &&
                  std::filesystem::exists(evaluationOutput / "evaluation-matches.csv"),
              "truth evaluation must persist summary and object matches");
  auto partialClosedRingTruth = closedRingTruth;
  partialClosedRingTruth.front().label = fillet::evaluation::TruthLabel::Unknown;
  const auto partialEvaluation =
      fillet::evaluation::evaluate(closedRing, partialClosedRingTruth, 0.50);
  ok &= check(partialEvaluation.reviewCoverage < 1.0 &&
                  !partialEvaluation.chains.available &&
                  !partialEvaluation.features.available,
              "partial truth must not publish misleading object-level metrics");
  const TopoDS_Shape mixedRingShape = closedBSplineFilletRing(true);
  ok &= check(BRepTools::Write(mixedRingShape,
                  (std::filesystem::path(FILLET_TEST_OUTPUT_DIR) /
                   "generated-mixed-analytic-bspline-fillet-ring.brep").string().c_str()),
              "mixed analytic/BSpline fillet ring must be persisted for inspection");
  const auto mixedRing = fillet::improved::recognize(mixedRingShape, closedRingOptions);
  int mixedBridgedTransitions = 0, mixedUnobservedTransitions = 0,
      mixedOptimizedBridges = 0;
  bool mixedBridgeCostAuditable = false;
  for (const auto& profile : mixedRing.radiusProfiles) {
    mixedBridgedTransitions += profile.analyticBridgedOrientationTransitionCount;
    mixedUnobservedTransitions += profile.unobservedOrientationTransitionCount;
      mixedOptimizedBridges +=
        profile.analyticBridgeOptimizedOrientationTransitionCount;
    for (const auto& transition : profile.orientationTransitions)
      if (transition.analyticBridgeIncludedInOptimization &&
          transition.analyticBridgeSignedAlignment > 0.95 &&
          std::abs(transition.cost -
                   transition.analyticBridgeOrientationCost) < 1.0e-12)
        mixedBridgeCostAuditable = true;
  }
  ok &= check(mixedRing.chains.size() == 1 && mixedRing.paths.size() == 1 &&
                  mixedRing.paths.front().isClosed && mixedBridgedTransitions == 1 &&
                  mixedOptimizedBridges == 1 && mixedBridgeCostAuditable &&
                  mixedUnobservedTransitions == 0 &&
                  mixedRing.features.size() == 1 &&
                  mixedRing.features.front().directionEvidenceState ==
                      fillet::improved::EvidenceState::Validated,
              "analytic quarter must bridge the missing freeform trace in a closed ring");
  ok &= check(mixedRing.performance.linkPairIndexEntryCount == 4 &&
                  mixedRing.performance.linkPairLookupCount > 0 &&
                  mixedRing.performance.linkCandidatesVisitedThroughIndex > 0,
              "analytic bridge optimization must use the face-pair link index");
  const TopoDS_Shape gappedRingShape = closedBSplineFilletRing(false, 1.0e-3);
  ok &= check(BRepTools::Write(gappedRingShape,
                  (std::filesystem::path(FILLET_TEST_OUTPUT_DIR) /
                   "generated-trim-gap-bspline-fillet-ring.brep").string().c_str()),
              "trim-gap negative fixture must be persisted for inspection");
  const auto gappedRing = fillet::improved::recognize(gappedRingShape, closedRingOptions);
  ok &= check(std::none_of(gappedRing.paths.begin(), gappedRing.paths.end(),
                           [](const fillet::improved::Path& path) {
                             return path.isClosed && path.faceIds.size() >= 4;
                           }),
              "a physical trim gap must not be healed into a closed fillet path");
  const TopoDS_Shape offsetRingShape =
      closedBSplineFilletRing(false, 0.0, 1, 0.25);
  ok &= check(BRepTools::Write(offsetRingShape,
                  (std::filesystem::path(FILLET_TEST_OUTPUT_DIR) /
                   "generated-offset-bspline-fillet-ring.brep").string().c_str()),
              "offset negative fixture must be persisted for inspection");
  const auto offsetRing = fillet::improved::recognize(offsetRingShape, closedRingOptions);
  ok &= check(std::none_of(offsetRing.paths.begin(), offsetRing.paths.end(),
                           [](const fillet::improved::Path& path) {
                             return path.isClosed && path.faceIds.size() >= 4;
                           }),
              "an offset quarter must not remain a validated closed fillet path");
  fillet::improved::Options shortEdgeOptions = closedRingOptions;
  shortEdgeOptions.minimumResolvedEdgeLengthToModelDiagonal = 0.30;
  const auto shortEdgeRejectedRing =
      fillet::improved::recognize(closedRingShape, shortEdgeOptions);
  const int rejectedShortLinks = static_cast<int>(std::count_if(
      shortEdgeRejectedRing.links.begin(), shortEdgeRejectedRing.links.end(),
      [](const fillet::improved::Link& link) { return link.rejectedShortSharedEdge; }));
  ok &= check(rejectedShortLinks >= 4 &&
                  std::none_of(shortEdgeRejectedRing.chains.begin(),
                               shortEdgeRejectedRing.chains.end(),
                               [](const fillet::improved::Chain& chain) {
                                 return chain.faceIds.size() > 1;
                               }),
              "short shared edges must not be reintroduced by composite linking");
  const TopoDS_Shape sliverShape = sliverFilletPatch();
  ok &= check(BRepTools::Write(sliverShape,
                  (std::filesystem::path(FILLET_TEST_OUTPUT_DIR) /
                   "generated-sliver-fillet-patch.brep").string().c_str()),
              "sliver negative fixture must be persisted for inspection");
  fillet::improved::Options sliverOptions;
  sliverOptions.minimumResolvedFaceAreaToModelDiagonalSquared = 1.0e-6;
  const auto sliver = fillet::improved::recognize(sliverShape, sliverOptions);
  ok &= check(sliver.diagnostics.sliverFaceCount > 0 && sliver.chains.empty(),
              "scale-normalized sliver faces must be diagnosed and excluded");
  fillet::improved::Options strictGapOptions = closedRingOptions;
  strictGapOptions.maximumRadiusProfileTransitionGapToModelDiagonal = 0.0;
  strictGapOptions.minimumRadiusProfileTransitionAlignment = -1.0;
  strictGapOptions.featureTopologyEvidenceWeight = 0.0;
  strictGapOptions.featureRadiusEvidenceWeight = 0.0;
  strictGapOptions.featureDirectionEvidenceWeight = 1.0;
  const auto strictGapRing = fillet::improved::recognize(closedRingShape, strictGapOptions);
  const int forcedGapConflicts = std::accumulate(
      strictGapRing.radiusProfiles.begin(), strictGapRing.radiusProfiles.end(), 0,
      [](int count, const fillet::improved::RadiusProfile& profile) {
        return count + profile.gapConflictOrientationTransitionCount;
      });
  ok &= check(forcedGapConflicts == 4,
              "zero transition gap threshold must locate all four ring transitions");
  ok &= check(strictGapRing.features.size() == 1 &&
                  strictGapRing.features.front().directionEvidenceState ==
                      fillet::improved::EvidenceState::Conflict &&
                  strictGapRing.features.front().aggregateEvidenceState ==
                      fillet::improved::EvidenceState::Conflict &&
                  std::abs(strictGapRing.features.front().confidence -
                           strictGapRing.features.front().baseGeometryConfidence *
                               strictGapOptions.conflictEvidenceScore) < 1.0e-12,
              "transition conflicts must reduce aggregate feature confidence");
  fillet::improved::Options strictDirectionOptions = closedRingOptions;
  strictDirectionOptions.maximumRadiusProfileTransitionGapToModelDiagonal = 1.0;
  strictDirectionOptions.minimumRadiusProfileTransitionAlignment = 0.999;
  const auto strictDirectionRing =
      fillet::improved::recognize(closedRingShape, strictDirectionOptions);
  const int forcedDirectionConflicts = std::accumulate(
      strictDirectionRing.radiusProfiles.begin(), strictDirectionRing.radiusProfiles.end(), 0,
      [](int count, const fillet::improved::RadiusProfile& profile) {
        return count + profile.directionConflictOrientationTransitionCount;
      });
  ok &= check(forcedDirectionConflicts > 0,
              "strict direction threshold must expose locatable ring transitions");
  const TopoDS_Shape branchedShape = roundedBox(12);
  ok &= check(BRepTools::Write(branchedShape,
                  (std::filesystem::path(FILLET_TEST_OUTPUT_DIR) /
                   "generated-branched-fillet-network.brep").string().c_str()),
              "generated branched fillet network must be persisted for inspection");
  const auto branched = fillet::improved::recognize(branchedShape);
  const bool foundBranchedChain = std::any_of(
      branched.chains.begin(), branched.chains.end(),
      [](const fillet::improved::Chain& chain) { return chain.isBranched; });
  const int branchPaths = static_cast<int>(std::count_if(
      branched.paths.begin(), branched.paths.end(),
      [](const fillet::improved::Path& path) {
        return path.startsAtBranch || path.endsAtBranch;
      }));
  ok &= check(foundBranchedChain && branchPaths >= 3,
              "fully rounded box must exercise a multi-branch fillet network");
  for (const auto& path : branched.paths)
    ok &= check(!(path.endsAtBranch && !path.startsAtBranch),
                "generated branch paths must be directed outward from junctions");
  std::map<int, std::string> variablePersistentIds;
  for (const auto& face : improvedVariable.faces)
    variablePersistentIds[face.faceId] = face.persistentId;
  for (const auto& path : improvedVariable.paths) {
    ok &= check(!(path.endsAtBranch && !path.startsAtBranch),
                "single-junction paths must be canonically directed away from the branch");
    if (path.isClosed && path.faceIds.size() > 2) {
      const auto minimum = std::min_element(path.faceIds.begin(), path.faceIds.end(),
          [&](int first, int second) {
            return variablePersistentIds[first] < variablePersistentIds[second];
          });
      ok &= check(minimum == path.faceIds.begin() &&
                      variablePersistentIds[path.faceIds[1]] <
                          variablePersistentIds[path.faceIds.back()],
                  "closed paths must have a deterministic root and direction");
    }
  }
  gp_Trsf scaleTransform;
  scaleTransform.SetScale(gp_Pnt(0.0, 0.0, 0.0), 1.0e-4);
  const TopoDS_Shape scaledVariableShape =
      BRepBuilderAPI_Transform(variableShape, scaleTransform, Standard_True).Shape();
  fillet::improved::Options scaledOptions;
  scaledOptions.minimumRadius = 1.0e-10;
  scaledOptions.absoluteTolerance = 1.0e-10;
  const auto scaledVariable = fillet::improved::recognize(scaledVariableShape, scaledOptions);
  bool scaledStreamlineFound = false;
  for (const auto& face : scaledVariable.faces)
    if (face.verdict == fillet::improved::Verdict::Accepted &&
        face.radiusTraceMethod ==
            fillet::improved::RadiusTraceMethod::PrincipalDirectionStreamline &&
        face.radiusTraceMinimumTangentAlignment >= 0.90)
      scaledStreamlineFound = true;
  ok &= check(scaledStreamlineFound,
              "uniformly scaled variable fillet must retain a valid streamline trace");
  for (const auto& link : improvedSingle.links)
    ok &= check(link.firstFaceId != link.secondFaceId,
                "seam ownership must never create a self-link");
  for (const auto& link : improvedSingle.links)
    if (link.spineDirectionEvaluated)
      ok &= check(link.spineDirectionAlignment >= 0.0 &&
                      link.spineDirectionAlignment <= 1.0,
                  "evaluated spine direction alignment must be normalized");
  for (const auto& link : improvedSingle.links)
    if (link.spineDirectionEvaluated)
      ok &= check(link.directionEvidenceSource !=
                      fillet::improved::DirectionEvidenceSource::None,
                  "evaluated direction must identify its evidence source");
  for (const auto& link : improvedSingle.links)
    if (link.spineDirectionEvaluated)
      ok &= check(link.spineDirectionValidSamples > 0 &&
                      link.spineDirectionSampleCoverage >= 0.0 &&
                      link.spineDirectionSampleCoverage <= 1.0 &&
                      link.minimumSpineDirectionAlignment >= 0.0 &&
                      link.minimumSpineDirectionAlignment <= link.spineDirectionAlignment,
                  "multi-sample direction statistics must be internally consistent");
  for (const auto& link : improvedSingle.links)
    ok &= check(link.directionSamples.size() ==
                    static_cast<std::size_t>(link.spineDirectionValidSamples),
                "every valid direction sample must retain locatable evidence");
  for (const auto& link : improvedSingle.links)
    if (link.usedInGraph)
      ok &= check(link.radiusCompatible && (link.tangent || link.sharedSupportPair),
                  "active graph link must retain its qualifying evidence");
  ok &= check(improvedSingle.diagnostics.valid, "generated fillet shape must be valid");
  std::set<std::string> persistentIds;
  for (const auto& face : improvedSingle.faces) {
    ok &= check(!face.persistentId.empty(), "every evidence face must have a persistent ID");
    persistentIds.insert(face.persistentId);
  }
  ok &= check(persistentIds.size() == improvedSingle.faces.size(),
              "persistent IDs must be unique inside one model");
  bool repeatable = improvedSingle.faces.size() == improvedSingleAgain.faces.size();
  for (std::size_t index = 0; repeatable && index < improvedSingle.faces.size(); ++index)
    repeatable = improvedSingle.faces[index].persistentId ==
                 improvedSingleAgain.faces[index].persistentId;
  ok &= check(repeatable, "persistent IDs must repeat for the same imported shape");
  for (const auto& face : improvedSingle.faces)
    if (face.verdict == fillet::improved::Verdict::Accepted)
      ok &= check(face.topologyRole != fillet::improved::TopologyRole::Unknown,
                  "accepted face must have a topology role");
  for (const auto& face : improvedSingle.faces)
    if (face.verdict == fillet::improved::Verdict::Accepted)
      ok &= check(!face.roleReason.empty(),
                  "accepted face must explain geometry/topology role consistency");
  for (const auto& face : improvedSingle.faces)
    if (face.verdict == fillet::improved::Verdict::Accepted)
      for (int supportId : face.externalSupportFaceIds)
        for (const auto& candidate : improvedSingle.faces)
          ok &= check(candidate.faceId != supportId ||
                          candidate.verdict != fillet::improved::Verdict::Accepted,
                      "external support must not be an accepted fillet candidate");
  fillet::improved::SewingOptions sewingOptions;
  sewingOptions.toleranceFactor = 1.0;
  const auto sewingComparison = fillet::improved::compareWithSewing(
      singleShape, fillet::improved::Options{}, sewingOptions);
  ok &= check(!sewingComparison.sewnShape.IsNull(),
              "controlled sewing comparison must produce a non-null copy");
  ok &= check(sewingComparison.faceMappings.size() == improvedSingle.faces.size(),
              "sewing comparison must report every original face mapping");
  ok &= check(sewingComparison.original.diagnostics.faceCount ==
                  improvedSingle.diagnostics.faceCount,
              "sewing comparison must preserve an independent original result");
  ok &= check(sewingComparison.partitionCount >= 1 &&
                  sewingComparison.unmappedFaceCount == 0,
              "controlled sewing must partition and map every generated face");
  ok &= check(sewingComparison.topologySafetyGatePassed,
              "generated valid fillet must pass the topology-only sewing gate");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
