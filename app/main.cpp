#include "fillet/FilletBaseline.hpp"
#include "fillet/ImprovedFilletRecognizer.hpp"
#include "fillet/RecognitionReport.hpp"
#include "fillet/TruthEvaluation.hpp"

#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <IGESControl_Reader.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Reader.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

bool readShape(const std::filesystem::path& path, TopoDS_Shape& shape) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (extension == ".step" || extension == ".stp") {
    STEPControl_Reader reader;
    if (reader.ReadFile(path.string().c_str()) != IFSelect_RetDone || reader.TransferRoots() == 0)
      return false;
    shape = reader.OneShape();
    return !shape.IsNull();
  }
  if (extension == ".iges" || extension == ".igs") {
    IGESControl_Reader reader;
    if (reader.ReadFile(path.string().c_str()) != IFSelect_RetDone || reader.TransferRoots() == 0)
      return false;
    shape = reader.OneShape();
    return !shape.IsNull();
  }
  if (extension == ".brep") {
    BRep_Builder builder;
    return BRepTools::Read(shape, path.string().c_str(), builder) && !shape.IsNull();
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: fillet-search [--improved|--baseline|--compare] "
                 "[--compare-sewing] [--sewing-factor number] "
                 "[--no-canonical-recovery] "
                 "[--radius-min number] [--radius-max number] "
                 "[--radius-mode nominal|entire|intersects|any] "
                 "[--min-edge-to-diagonal number] "
                 "[--min-face-area-to-diagonal-squared number] "
                 "[--orientation-direction-weight number] "
                 "[--truth-file labels.csv] [--truth-iou number] "
                 "[--performance-budget-ms number] [--fail-on-performance-budget] "
                 "[--report-dir directory] model.step ...\n";
    return EXIT_FAILURE;
  }

  enum class Mode { Improved, Baseline, Compare };
  Mode mode = Mode::Improved;
  std::filesystem::path reportDirectory;
  std::filesystem::path truthFile;
  double truthIou = 0.50;
  bool compareSewing = false;
  bool failOnPerformanceBudget = false;
  double sewingFactor = 1.0;
  fillet::improved::Options recognitionOptions;
  std::vector<std::filesystem::path> models;
  for (int argument = 1; argument < argc; ++argument) {
    const std::string value = argv[argument];
    if (value == "--baseline") mode = Mode::Baseline;
    else if (value == "--compare") mode = Mode::Compare;
    else if (value == "--improved") mode = Mode::Improved;
    else if (value == "--compare-sewing") compareSewing = true;
    else if (value == "--fail-on-performance-budget")
      failOnPerformanceBudget = true;
    else if (value == "--no-canonical-recovery")
      recognitionOptions.enableCanonicalRecovery = false;
    else if (value == "--radius-min" || value == "--radius-max") {
      if (++argument >= argc) {
        std::cerr << value << " requires a non-negative number\n";
        return EXIT_FAILURE;
      }
      double parsed = -1.0;
      try { parsed = std::stod(argv[argument]); }
      catch (const std::exception&) {}
      if (!(parsed >= 0.0) || !std::isfinite(parsed)) {
        std::cerr << value << " requires a non-negative finite number\n";
        return EXIT_FAILURE;
      }
      if (value == "--radius-min") recognitionOptions.minimumRadius = parsed;
      else recognitionOptions.maximumRadius = parsed;
    }
    else if (value == "--radius-mode") {
      if (++argument >= argc) {
        std::cerr << "--radius-mode requires nominal, entire, intersects, or any\n";
        return EXIT_FAILURE;
      }
      const std::string radiusMode = argv[argument];
      if (radiusMode == "nominal")
        recognitionOptions.radiusFilterMode =
            fillet::improved::RadiusFilterMode::NominalWithinRange;
      else if (radiusMode == "entire")
        recognitionOptions.radiusFilterMode =
            fillet::improved::RadiusFilterMode::EntireProfileWithinRange;
      else if (radiusMode == "intersects")
        recognitionOptions.radiusFilterMode =
            fillet::improved::RadiusFilterMode::ProfileIntersectsRange;
      else if (radiusMode == "any")
        recognitionOptions.radiusFilterMode = fillet::improved::RadiusFilterMode::AnyRadius;
      else {
        std::cerr << "--radius-mode requires nominal, entire, intersects, or any\n";
        return EXIT_FAILURE;
      }
    }
    else if (value == "--min-edge-to-diagonal" ||
             value == "--min-face-area-to-diagonal-squared" ||
             value == "--orientation-direction-weight") {
      if (++argument >= argc) {
        std::cerr << value << " requires a non-negative number\n";
        return EXIT_FAILURE;
      }
      double parsed = -1.0;
      try { parsed = std::stod(argv[argument]); }
      catch (const std::exception&) {}
      if (!(parsed >= 0.0) || !std::isfinite(parsed)) {
        std::cerr << value << " requires a non-negative finite number\n";
        return EXIT_FAILURE;
      }
      if (value == "--min-edge-to-diagonal")
        recognitionOptions.minimumResolvedEdgeLengthToModelDiagonal = parsed;
      else if (value == "--min-face-area-to-diagonal-squared")
        recognitionOptions.minimumResolvedFaceAreaToModelDiagonalSquared = parsed;
      else
        recognitionOptions.radiusProfileDirectionCostWeight = parsed;
    }
    else if (value == "--sewing-factor") {
      if (++argument >= argc) {
        std::cerr << "--sewing-factor requires a positive number\n";
        return EXIT_FAILURE;
      }
      try { sewingFactor = std::stod(argv[argument]); }
      catch (const std::exception&) { sewingFactor = 0.0; }
      if (!(sewingFactor > 0.0)) {
        std::cerr << "--sewing-factor requires a positive number\n";
        return EXIT_FAILURE;
      }
    }
    else if (value == "--performance-budget-ms") {
      if (++argument >= argc) {
        std::cerr << "--performance-budget-ms requires a positive number\n";
        return EXIT_FAILURE;
      }
      try { recognitionOptions.maximumRecognitionMilliseconds = std::stod(argv[argument]); }
      catch (const std::exception&) { recognitionOptions.maximumRecognitionMilliseconds = 0.0; }
      if (!(recognitionOptions.maximumRecognitionMilliseconds > 0.0) ||
          !std::isfinite(recognitionOptions.maximumRecognitionMilliseconds)) {
        std::cerr << "--performance-budget-ms requires a positive finite number\n";
        return EXIT_FAILURE;
      }
    }
    else if (value == "--report-dir") {
      if (++argument >= argc) {
        std::cerr << "--report-dir requires a directory\n";
        return EXIT_FAILURE;
      }
      reportDirectory = argv[argument];
    } else if (value == "--truth-file") {
      if (++argument >= argc) {
        std::cerr << "--truth-file requires a CSV file\n";
        return EXIT_FAILURE;
      }
      truthFile = argv[argument];
    } else if (value == "--truth-iou") {
      if (++argument >= argc) {
        std::cerr << "--truth-iou requires a number in (0, 1]\n";
        return EXIT_FAILURE;
      }
      try { truthIou = std::stod(argv[argument]); }
      catch (const std::exception&) { truthIou = 0.0; }
      if (!(truthIou > 0.0 && truthIou <= 1.0) || !std::isfinite(truthIou)) {
        std::cerr << "--truth-iou requires a finite number in (0, 1]\n";
        return EXIT_FAILURE;
      }
    } else if (!value.empty() && value[0] == '-') {
      std::cerr << "unknown option: " << value << '\n';
      return EXIT_FAILURE;
    } else models.emplace_back(value);
  }
  if (models.empty()) return EXIT_FAILURE;
  if (recognitionOptions.radiusFilterMode != fillet::improved::RadiusFilterMode::AnyRadius &&
      recognitionOptions.maximumRadius < recognitionOptions.minimumRadius) {
    std::cerr << "--radius-max must be greater than or equal to --radius-min\n";
    return EXIT_FAILURE;
  }
  if (!reportDirectory.empty() && mode == Mode::Baseline) {
    std::cerr << "--report-dir requires --improved or --compare\n";
    return EXIT_FAILURE;
  }
  if (compareSewing && mode == Mode::Baseline) {
    std::cerr << "--compare-sewing requires --improved or --compare\n";
    return EXIT_FAILURE;
  }
  if (!truthFile.empty() && (models.size() != 1 || mode == Mode::Baseline || compareSewing)) {
    std::cerr << "--truth-file requires exactly one model, improved/compare mode, and no sewing comparison\n";
    return EXIT_FAILURE;
  }
  if (failOnPerformanceBudget &&
      !(recognitionOptions.maximumRecognitionMilliseconds > 0.0)) {
    std::cerr << "--fail-on-performance-budget requires --performance-budget-ms\n";
    return EXIT_FAILURE;
  }
  std::optional<std::vector<fillet::evaluation::TruthRecord>> truthRecords;
  if (!truthFile.empty()) {
    try { truthRecords = fillet::evaluation::read(truthFile); }
    catch (const std::exception& error) {
      std::cerr << "failed to read truth file: " << error.what() << '\n';
      return EXIT_FAILURE;
    }
  }

  bool allRead = true;
  for (const std::filesystem::path& path : models) {
    TopoDS_Shape shape;
    if (!readShape(path, shape)) {
      std::cerr << "failed to read CAD file: " << path.string() << '\n';
      allRead = false;
      continue;
    }
    std::cout << "model: " << path.string() << '\n';
    if (mode == Mode::Baseline || mode == Mode::Compare) {
      const auto chains = fillet::baseline::search(
          shape, 1.0e-6, std::numeric_limits<double>::max());
      std::size_t acceptedFaces = 0;
      for (const auto& chain : chains) acceptedFaces += chain.faces.size();
      std::cout << "baseline: faces=" << acceptedFaces << ", chains=" << chains.size() << '\n';
    }
    if (mode == Mode::Improved || mode == Mode::Compare) {
      std::optional<fillet::improved::SewingComparison> sewingComparison;
      if (compareSewing) {
        fillet::improved::SewingOptions sewingOptions;
        sewingOptions.toleranceFactor = sewingFactor;
        sewingComparison = fillet::improved::compareWithSewing(
            shape, recognitionOptions, sewingOptions);
      }
      const fillet::improved::Result standalone = compareSewing
          ? fillet::improved::Result{} : fillet::improved::recognize(shape, recognitionOptions);
      const auto& improved = compareSewing ? sewingComparison->original : standalone;
      std::size_t acceptedFaces = 0, recoveredFaces = 0, curvatureFaces = 0;
      std::size_t rejectedSupports = 0, rejectedPrimary = 0, rejectedGeometry = 0;
      std::size_t rejectedUnresolved = 0;
      std::size_t inferredSupports = 0;
      for (const auto& face : improved.faces) {
        if (face.verdict == fillet::improved::Verdict::Accepted) ++acceptedFaces;
        if (face.geometrySource == fillet::improved::GeometrySource::RecoveredCylinder ||
            face.geometrySource == fillet::improved::GeometrySource::RecoveredSphere) ++recoveredFaces;
        if (face.geometrySource == fillet::improved::GeometrySource::CurvatureField) ++curvatureFaces;
        if (face.verdict == fillet::improved::Verdict::RejectedNoSupports) ++rejectedSupports;
        if (face.verdict == fillet::improved::Verdict::RejectedLikelyPrimarySurface) ++rejectedPrimary;
        if (face.verdict == fillet::improved::Verdict::RejectedGeometry) ++rejectedGeometry;
        if (face.verdict == fillet::improved::Verdict::RejectedNumericallyUnresolved)
          ++rejectedUnresolved;
        inferredSupports += face.inferredSupportFaceIds.size();
      }
      std::cout << "improved: faces=" << acceptedFaces << ", chains=" << improved.chains.size()
                << ", recovered=" << recoveredFaces
                << ", curvature=" << curvatureFaces
                << ", paths=" << improved.paths.size()
                << ", inferred-supports=" << inferredSupports
                << ", valid=" << std::boolalpha << improved.diagnostics.valid
                << ", short-edges=" << improved.diagnostics.shortEdgeCount
                << ", sliver-faces=" << improved.diagnostics.sliverFaceCount
                << ", tolerance=" << improved.diagnostics.effectiveLinearTolerance << '\n';
      std::cout << "  rejected: geometry=" << rejectedGeometry
                << ", supports=" << rejectedSupports
                << ", primary-risk=" << rejectedPrimary
                << ", numerically-unresolved=" << rejectedUnresolved << '\n';
      std::cout << "  performance: total=" << improved.performance.totalMilliseconds
                << " ms, diagnostics=" << improved.performance.diagnosticsMilliseconds
                << ", faces=" << improved.performance.faceAnalysisMilliseconds
                << ", topology=" << improved.performance.topologyMilliseconds
                << ", profiles=" << improved.performance.radiusProfileMilliseconds
                << ", features=" << improved.performance.featureAggregationMilliseconds
                << ", link-index=" << improved.performance.linkPairIndexEntryCount
                << " pair(s)/" << improved.performance.linkPairLookupCount
                << " lookup(s)/" << improved.performance.linkCandidatesVisitedThroughIndex
                << " candidate(s)"
                << (improved.performance.budgetEnabled
                        ? (improved.performance.budgetExceeded
                              ? ", budget=EXCEEDED" : ", budget=passed")
                        : "") << '\n';
      if (failOnPerformanceBudget && improved.performance.budgetExceeded)
        allRead = false;
      for (std::size_t i = 0; i < improved.chains.size(); ++i)
        std::cout << "  chain " << i << ": " << improved.chains[i].faceIds.size()
                  << " face(s), radius " << improved.chains[i].minimumRadius << ".."
                  << improved.chains[i].maximumRadius << ", confidence "
                  << improved.chains[i].confidence << ", supports "
                  << improved.chains[i].supportFaceIds.size()
                  << (improved.chains[i].verdict == fillet::improved::ChainVerdict::Accepted
                          ? ", validated" : ", needs-review") << '\n';
      const auto reviewChains = std::count_if(improved.chains.begin(), improved.chains.end(),
          [](const fillet::improved::Chain& chain) {
            return chain.verdict != fillet::improved::ChainVerdict::Accepted;
          });
      std::cout << "  chain validation: accepted="
                << improved.chains.size() - reviewChains
                << ", needs-review=" << reviewChains << '\n';
      std::size_t closedPaths = 0, branchPaths = 0;
      for (const auto& pathResult : improved.paths) {
        if (pathResult.isClosed) ++closedPaths;
        if (pathResult.startsAtBranch || pathResult.endsAtBranch) ++branchPaths;
      }
      std::cout << "  paths: closed=" << closedPaths << ", touching-branch="
                << branchPaths << '\n';
      std::size_t constantProfiles = 0, smoothProfiles = 0, segmentedProfiles = 0,
                  discontinuousProfiles = 0, insufficientProfiles = 0;
      std::size_t tracedProfiles = 0, radiusSamples = 0;
      std::size_t streamlineFaces = 0, parameterFallbackFaces = 0;
      std::size_t rejectedLowAlignmentTraceFaces = 0;
      std::size_t stableAcrossSeedsTraceFaces = 0, unstableAcrossSeedsTraceFaces = 0;
      std::size_t insufficientSeedStabilityTraceFaces = 0;
      std::size_t globallyOrientedProfiles = 0, globallyReversedTraceFaces = 0;
      std::size_t acceptedOrientationTransitions = 0, gapConflictTransitions = 0;
      std::size_t directionConflictTransitions = 0, unobservedTransitions = 0;
      std::size_t analyticBridgedTransitions = 0, analyticBridgeConflicts = 0;
      std::size_t analyticBridgeOptimizedTransitions = 0;
      double globalOrientationCost = 0.0, greedyOrientationCost = 0.0;
      std::size_t traceSeedAttempts = 0, adaptiveStepReductions = 0;
      std::size_t totalAttemptedStepReductions = 0;
      for (const auto& face : improved.faces) {
        traceSeedAttempts += face.radiusTraceSeedAttempts;
        adaptiveStepReductions += face.radiusTraceAdaptiveStepReductions;
        totalAttemptedStepReductions += face.radiusTraceTotalAttemptedStepReductions;
      }
      for (const auto& profile : improved.radiusProfiles) {
        if (!profile.samples.empty()) ++tracedProfiles;
        radiusSamples += profile.samples.size();
        streamlineFaces += profile.streamlineFaceCount;
        parameterFallbackFaces += profile.parameterLineFallbackFaceCount;
        rejectedLowAlignmentTraceFaces += profile.rejectedLowAlignmentTraceFaceCount;
        stableAcrossSeedsTraceFaces += profile.stableAcrossSeedsTraceFaceCount;
        unstableAcrossSeedsTraceFaces += profile.unstableAcrossSeedsTraceFaceCount;
        insufficientSeedStabilityTraceFaces +=
            profile.insufficientSeedStabilityTraceFaceCount;
        if (profile.globalOrientationOptimized) ++globallyOrientedProfiles;
        globallyReversedTraceFaces += profile.orientationReversedFaceCount;
        globalOrientationCost += profile.orientationTransitionCost;
        greedyOrientationCost += profile.greedyOrientationTransitionCost;
        acceptedOrientationTransitions += profile.acceptedOrientationTransitionCount;
        gapConflictTransitions += profile.gapConflictOrientationTransitionCount;
        directionConflictTransitions +=
            profile.directionConflictOrientationTransitionCount;
        unobservedTransitions += profile.unobservedOrientationTransitionCount;
        analyticBridgedTransitions += profile.analyticBridgedOrientationTransitionCount;
        analyticBridgeConflicts +=
            profile.analyticBridgeConflictOrientationTransitionCount;
        analyticBridgeOptimizedTransitions +=
            profile.analyticBridgeOptimizedOrientationTransitionCount;
        switch (profile.behavior) {
          case fillet::improved::RadiusBehavior::Constant: ++constantProfiles; break;
          case fillet::improved::RadiusBehavior::SmoothVariable: ++smoothProfiles; break;
          case fillet::improved::RadiusBehavior::Segmented: ++segmentedProfiles; break;
          case fillet::improved::RadiusBehavior::Discontinuous:
            ++discontinuousProfiles; break;
          case fillet::improved::RadiusBehavior::InsufficientEvidence:
            ++insufficientProfiles; break;
        }
      }
      std::cout << "  radius profiles: total=" << improved.radiusProfiles.size()
                << ", constant=" << constantProfiles
                << ", smooth-variable-candidate=" << smoothProfiles
                << ", segmented=" << segmentedProfiles
                << ", discontinuous=" << discontinuousProfiles
                << ", insufficient=" << insufficientProfiles
                << ", traced=" << tracedProfiles
                << ", samples=" << radiusSamples
                << ", streamline-faces=" << streamlineFaces
                << ", parameter-fallback-faces=" << parameterFallbackFaces
                << ", rejected-low-alignment-traces="
                << rejectedLowAlignmentTraceFaces
                << ", stable-cross-seed-traces=" << stableAcrossSeedsTraceFaces
                << ", unstable-cross-seed-traces=" << unstableAcrossSeedsTraceFaces
                << ", insufficient-seed-stability-traces="
                << insufficientSeedStabilityTraceFaces
                << ", globally-oriented-profiles=" << globallyOrientedProfiles
                << ", globally-reversed-trace-faces=" << globallyReversedTraceFaces
                << ", orientation-cost=" << globalOrientationCost
                << ", greedy-orientation-cost=" << greedyOrientationCost
                << ", accepted-orientation-transitions="
                << acceptedOrientationTransitions
                << ", gap-conflict-transitions=" << gapConflictTransitions
                << ", direction-conflict-transitions=" << directionConflictTransitions
                << ", unobserved-transitions=" << unobservedTransitions
                << ", analytic-bridged-transitions=" << analyticBridgedTransitions
                << ", analytic-bridge-conflicts=" << analyticBridgeConflicts
                << ", analytic-bridge-optimized-transitions="
                << analyticBridgeOptimizedTransitions
                << ", seed-attempts=" << traceSeedAttempts
                << ", selected-step-reductions=" << adaptiveStepReductions
                << ", attempted-step-reductions=" << totalAttemptedStepReductions << '\n';
      std::size_t compositeFeatures = 0, isolatedFeatures = 0;
      std::size_t geometryRoleValidated = 0, geometryRoleReview = 0;
      std::size_t directionValidated = 0, directionReview = 0;
      std::size_t directionInsufficientCoverage = 0;
      std::size_t aggregateValidated = 0, aggregateConflicts = 0,
                  aggregateInsufficient = 0;
      double featureConfidenceSum = 0.0;
      for (const auto& feature : improved.features) {
        if (feature.kind == fillet::improved::FeatureKind::CompositeJunctionNetwork)
          ++compositeFeatures;
        if (feature.kind == fillet::improved::FeatureKind::IsolatedPatch)
          ++isolatedFeatures;
        if (feature.geometryRoleValidated) ++geometryRoleValidated;
        if (!feature.roleReviewFaceIds.empty()) ++geometryRoleReview;
        if (feature.spineDirectionValidated) ++directionValidated;
        if (!feature.directionConflictLinkIds.empty()) ++directionReview;
        if (!feature.directionInsufficientCoverageLinkIds.empty())
          ++directionInsufficientCoverage;
        featureConfidenceSum += feature.confidence;
        switch (feature.aggregateEvidenceState) {
          case fillet::improved::EvidenceState::Validated: ++aggregateValidated; break;
          case fillet::improved::EvidenceState::Conflict: ++aggregateConflicts; break;
          case fillet::improved::EvidenceState::InsufficientEvidence:
            ++aggregateInsufficient; break;
        }
      }
      std::cout << "  features: total=" << improved.features.size()
                << ", composite=" << compositeFeatures
                << ", isolated=" << isolatedFeatures
                << ", geometry-role-validated=" << geometryRoleValidated
                << ", geometry-role-review=" << geometryRoleReview
                << ", direction-validated=" << directionValidated
                << ", direction-review=" << directionReview
                << ", direction-low-coverage=" << directionInsufficientCoverage
                << ", aggregate-validated=" << aggregateValidated
                << ", aggregate-conflict=" << aggregateConflicts
                << ", aggregate-insufficient=" << aggregateInsufficient
                << ", mean-confidence=" << (improved.features.empty() ? 0.0
                    : featureConfidenceSum / improved.features.size()) << '\n';
      if (sewingComparison) {
        const auto sewnAcceptedFaces = std::count_if(
            sewingComparison->sewn.faces.begin(), sewingComparison->sewn.faces.end(),
            [](const fillet::improved::FaceEvidence& face) {
              return face.verdict == fillet::improved::Verdict::Accepted;
            });
        std::cout << "  sewing comparison: tolerance="
                  << sewingComparison->sewingTolerance
                  << ", free-edges=" << sewingComparison->freeEdgeCount
                  << ", contiguous-edges=" << sewingComparison->contiguousEdgeCount
                  << ", multiple-edges=" << sewingComparison->multipleEdgeCount
                  << ", deleted-faces=" << sewingComparison->deletedFaceCount
                  << ", mapped-faces=" << sewingComparison->mappedFaceCount << '/'
                  << sewingComparison->original.diagnostics.faceCount
                  << ", modified-faces=" << sewingComparison->modifiedFaceCount
                  << ", partitions=" << sewingComparison->partitionCount
                  << ", max-partition-faces=" << sewingComparison->maximumPartitionFaceCount
                  << ", free-edge-length=" << sewingComparison->totalFreeEdgeLength
                  << ", shells=" << sewingComparison->originalShellCount << "->"
                  << sewingComparison->sewnShellCount
                  << ", closed-shells=" << sewingComparison->originalClosedShellCount << "->"
                  << sewingComparison->sewnClosedShellCount
                  << ", topology-safe=" << sewingComparison->topologySafetyGatePassed << '\n';
        std::cout << "  sewn result: faces=" << sewnAcceptedFaces
                  << ", chains=" << sewingComparison->sewn.chains.size()
                  << ", paths=" << sewingComparison->sewn.paths.size()
                  << ", features=" << sewingComparison->sewn.features.size()
                  << ", valid=" << sewingComparison->sewn.diagnostics.valid << '\n';
      }
      if (!reportDirectory.empty()) {
        try {
          const auto written = sewingComparison
              ? fillet::report::writeSewingComparison(
                    *sewingComparison, reportDirectory, path.stem().string())
              : fillet::report::write(
                    improved, shape, reportDirectory, path.stem().string());
          std::cout << "  report: " << written.string() << '\n';
          if (truthRecords) {
            const auto evaluation = fillet::evaluation::evaluate(
                improved, *truthRecords, truthIou);
            fillet::evaluation::write(evaluation, written);
            std::cout << "  evaluation: coverage=" << evaluation.reviewCoverage
                      << ", face-F1=" << evaluation.faces.f1;
            if (evaluation.features.available)
              std::cout << ", chain-F1=" << evaluation.chains.f1
                        << ", feature-F1=" << evaluation.features.f1;
            else
              std::cout << ", object-metrics=unavailable-partial-truth";
            std::cout << '\n';
          }
        } catch (const std::exception& error) {
          std::cerr << "failed to write report for " << path.string() << ": "
                    << error.what() << '\n';
          allRead = false;
        }
      }
      else if (truthRecords) {
        const auto evaluation = fillet::evaluation::evaluate(
            improved, *truthRecords, truthIou);
        std::cout << "  evaluation: coverage=" << evaluation.reviewCoverage
                  << ", face-F1=" << evaluation.faces.f1;
        if (evaluation.features.available)
          std::cout << ", chain-F1=" << evaluation.chains.f1
                    << ", feature-F1=" << evaluation.features.f1;
        else
          std::cout << ", object-metrics=unavailable-partial-truth";
        std::cout << '\n';
      }
    }
  }
  return allRead ? EXIT_SUCCESS : EXIT_FAILURE;
}
