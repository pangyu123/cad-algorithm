#pragma once

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace fillet::improved {

enum class GeometrySource { AnalyticCylinder, AnalyticTorus, AnalyticSphere,
                            RecoveredCylinder, RecoveredSphere, CurvatureField };
enum class Convexity { Unknown, Convex, Concave };
enum class ChainVerdict { Accepted, NeedsReviewInsufficientExternalSupports };
enum class TopologyRole { Unknown, IsolatedPatch, TerminalPatch, BandPatch,
                          JunctionPatch };
enum class RoleConsistency { Unknown, Consistent, NeedsReview };
enum class DirectionEvidenceSource { None, SharedEdge, ProjectedBoundary };
enum class RadiusFilterMode { NominalWithinRange, EntireProfileWithinRange,
                              ProfileIntersectsRange, AnyRadius };
enum class RadiusBehavior { InsufficientEvidence, Constant, SmoothVariable,
                            Segmented, Discontinuous };
enum class RadiusTraceMethod { None, PrincipalDirectionStreamline,
                               ParameterLineFallback };
enum class OrientationTransitionIssue { None, UnobservedIntermediateFaces,
                                        GapTooLarge, DirectionMismatch,
                                        GapAndDirectionMismatch };
enum class EvidenceState { Validated, Conflict, InsufficientEvidence };
enum class FeatureKind { IsolatedPatch, SimpleChain, ClosedLoop,
                         CompositeJunctionNetwork };
enum class Verdict { Accepted, RejectedNoSupports, RejectedRadius,
                     RejectedGeometry, RejectedLikelyPrimarySurface,
                     RejectedNumericallyUnresolved, EvaluationFailed };

struct Options {
  double minimumRadius = 1.0e-6;
  double maximumRadius = 1.0e100;
  RadiusFilterMode radiusFilterMode = RadiusFilterMode::NominalWithinRange;
  double absoluteTolerance = 1.0e-6;
  double relativeTolerance = 1.0e-6;
  double canonicalToleranceFactor = 5.0;
  double tangentAngleToleranceRadians = 5.0 * 3.14159265358979323846 / 180.0;
  double minimumSpineDirectionAlignment = 0.9659258262890683; // cos(15 degrees)
  int spineDirectionSampleCount = 5;
  double minimumSpineDirectionSampleCoverage = 0.60;
  double maximumUmbilicCurvatureRatioForDirection = 0.90;
  int maximumProjectedDirectionAttempts = 100;
  int directionConflictRefinementSampleCount = 21;
  int maximumProjectedDirectionRefinementAttempts = 400;
  double radiusRelativeTolerance = 0.05;
  double constantRadiusRelativeSpread = 0.05;
  double maximumSmoothRadiusRelativeStep = 0.15;
  double discontinuousRadiusRelativeStep = 0.30;
  double minimumSmoothActiveStepFraction = 0.75;
  double minimumSmoothMonotonicStepFraction = 0.75;
  double minimumDiscontinuityVariationFraction = 0.80;
  int minimumSupportFaces = 2;
  bool enableCanonicalRecovery = true;
  bool enableCurvatureField = true;
  int curvatureGridSize = 5;
  int minimumCurvatureSamples = 6;
  int minimumRadiusTraceSamples = 3;
  int radiusTraceSampleCount = 11;
  double radiusTraceNormalizedStep = 0.09;
  double minimumRadiusTraceCoverage = 0.50;
  double minimumRadiusTraceTangentAlignment = 0.90;
  int radiusTraceSeedCount = 5;
  int radiusTraceStabilitySeedCount = 3;
  int radiusTraceMaximumStepReductions = 3;
  double radiusTraceStepReductionFactor = 0.50;
  double maximumRadiusTraceCrossSeedRelativeDeviation = 0.10;
  double maximumRadiusProfileTransitionGapToModelDiagonal = 0.15;
  double minimumRadiusProfileTransitionAlignment = 0.0;
  double radiusProfileDirectionCostWeight = 0.10;
  double featureTopologyEvidenceWeight = 0.40;
  double featureRadiusEvidenceWeight = 0.30;
  double featureDirectionEvidenceWeight = 0.30;
  double validatedEvidenceScore = 1.0;
  double insufficientEvidenceScore = 0.65;
  double conflictEvidenceScore = 0.35;
  double maximumRadiusCoefficientOfVariation = 0.35;
  double maximumSecondaryCurvatureRatio = 0.65;
  bool enableDoubleCurvedCornerCandidates = true;
  double minimumDoubleCurvatureRatio = 0.65;
  double minimumSameSignCurvatureFraction = 0.80;
  double primaryCylinderAngularCoverage = 0.75;
  double primaryRadiusToDiagonal = 0.35;
  double primaryAreaRatio = 0.10;
  bool enableGeometricSupportRecovery = true;
  double geometricSupportGapFactor = 10.0;
  int maximumGeometricSupportTestsPerFace = 100;
  int maximumGeometricSupportTestsPerModel = 5000;
  double minimumResolvedEdgeLengthToModelDiagonal = 1.0e-8;
  double minimumResolvedFaceAreaToModelDiagonalSquared = 1.0e-12;
  double maximumRecognitionMilliseconds = 0.0;
  bool keepRejectedEvidence = true;
};

struct Diagnostics {
  bool valid = false;
  int faceCount = 0;
  int edgeCount = 0;
  int degeneratedEdgeCount = 0;
  int shortEdgeCount = 0;
  int sliverFaceCount = 0;
  int nonManifoldEdgeCount = 0;
  double modelDiagonal = 0.0;
  double minimumTolerance = 0.0;
  double averageTolerance = 0.0;
  double maximumTolerance = 0.0;
  double effectiveLinearTolerance = 0.0;
};

struct RadiusSampleEvidence {
  double u = 0.0;
  double v = 0.0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double radius = 0.0;
  double secondaryCurvatureRatio = 0.0;
  double spineDirectionX = 0.0;
  double spineDirectionY = 0.0;
  double spineDirectionZ = 0.0;
  double normalizedSpineParameter = 0.0;
};

struct FaceEvidence {
  int faceId = 0;
  std::string persistentId;
  TopoDS_Face face;
  GeometrySource geometrySource = GeometrySource::AnalyticCylinder;
  Verdict verdict = Verdict::RejectedGeometry;
  double radius = 0.0;
  double minimumRadius = 0.0;
  double maximumRadius = 0.0;
  double canonicalGap = 0.0;
  double angularCoverage = 0.0;
  double surfaceArea = 0.0;
  double areaRatio = 0.0;
  double areaToModelDiagonalSquared = 0.0;
  bool numericallyResolved = true;
  double radiusToModelDiagonal = 0.0;
  double confidence = 0.0;
  double secondaryCurvatureRatio = 0.0;
  double sameSignCurvatureFraction = 0.0;
  bool variableRadius = false;
  bool doubleCurved = false;
  bool likelyPrimarySurface = false;
  Convexity convexity = Convexity::Unknown;
  TopologyRole topologyRole = TopologyRole::Unknown;
  RoleConsistency roleConsistency = RoleConsistency::Unknown;
  std::vector<int> supportFaceIds;
  // Tangent neighbors that are not accepted fillet candidates.  Composite
  // links may only use these structural support faces.
  std::vector<int> externalSupportFaceIds;
  std::vector<int> inferredSupportFaceIds;
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
  int tangentBoundaryCount = 0;
  int nonTangentBoundaryCount = 0;
  std::string reason;
  std::string roleReason;
};

struct DirectionSampleEvidence {
  double firstX = 0.0;
  double firstY = 0.0;
  double firstZ = 0.0;
  double secondX = 0.0;
  double secondY = 0.0;
  double secondZ = 0.0;
  double firstU = 0.0;
  double firstV = 0.0;
  double secondU = 0.0;
  double secondV = 0.0;
  double gap = 0.0;
  double alignment = 0.0;
  double firstSecondaryCurvatureRatio = 0.0;
  double secondSecondaryCurvatureRatio = 0.0;
  double firstDirectionX = 0.0;
  double firstDirectionY = 0.0;
  double firstDirectionZ = 0.0;
  double secondDirectionX = 0.0;
  double secondDirectionY = 0.0;
  double secondDirectionZ = 0.0;
};

struct Link {
  int firstFaceId = 0;
  int secondFaceId = 0;
  bool sharedEdge = false;
  double sharedEdgeLength = 0.0;
  double sharedEdgeLengthToModelDiagonal = 0.0;
  bool rejectedShortSharedEdge = false;
  bool tangent = false;
  bool radiusCompatible = false;
  bool sharedSupportPair = false;
  bool usedInGraph = false;
  bool spineDirectionEvaluated = false;
  bool spineDirectionCompatible = false;
  DirectionEvidenceSource directionEvidenceSource = DirectionEvidenceSource::None;
  double spineDirectionAlignment = -1.0;
  int spineDirectionRequestedSamples = 0;
  int spineDirectionValidSamples = 0;
  double spineDirectionSampleCoverage = 0.0;
  double minimumSpineDirectionAlignment = -1.0;
  double spineDirectionAlignmentStandardDeviation = 0.0;
  std::vector<double> spineDirectionAlignmentSamples;
  std::vector<double> spineDirectionProjectionGapSamples;
  std::vector<DirectionSampleEvidence> directionSamples;
  bool directionRefinementAttempted = false;
  bool directionRefinementAccepted = false;
  int initialDirectionValidSamples = 0;
  double initialMinimumSpineDirectionAlignment = -1.0;
};

struct Chain {
  std::vector<int> faceIds;
  std::vector<int> supportFaceIds;
  double minimumRadius = 0.0;
  double maximumRadius = 0.0;
  double meanRadius = 0.0;
  double confidence = 0.0;
  ChainVerdict verdict = ChainVerdict::NeedsReviewInsufficientExternalSupports;
  bool isClosed = false;
  bool isBranched = false;
};

struct Path {
  std::vector<int> faceIds;
  int startNodeFaceId = 0;
  int endNodeFaceId = 0;
  bool isClosed = false;
  bool startsAtBranch = false;
  bool endsAtBranch = false;
};

struct Feature {
  int chainId = 0;
  FeatureKind kind = FeatureKind::IsolatedPatch;
  ChainVerdict verdict = ChainVerdict::NeedsReviewInsufficientExternalSupports;
  std::vector<int> faceIds;
  std::vector<int> bandFaceIds;
  std::vector<int> terminalFaceIds;
  std::vector<int> junctionFaceIds;
  std::vector<int> isolatedFaceIds;
  std::vector<int> pathIds;
  std::vector<int> radiusProfileIds;
  std::vector<int> externalSupportFaceIds;
  std::vector<int> roleReviewFaceIds;
  std::vector<int> roleUnknownFaceIds;
  std::vector<int> directionConflictLinkIds;
  std::vector<int> directionInsufficientCoverageLinkIds;
  double minimumRadius = 0.0;
  double maximumRadius = 0.0;
  double baseGeometryConfidence = 0.0;
  double topologyEvidenceScore = 0.0;
  double radiusEvidenceScore = 0.0;
  double directionEvidenceScore = 0.0;
  double aggregateEvidenceScore = 0.0;
  double confidence = 0.0;
  EvidenceState topologyEvidenceState = EvidenceState::InsufficientEvidence;
  EvidenceState radiusEvidenceState = EvidenceState::InsufficientEvidence;
  EvidenceState directionEvidenceState = EvidenceState::InsufficientEvidence;
  EvidenceState aggregateEvidenceState = EvidenceState::InsufficientEvidence;
  int radiusUnstableProfileCount = 0;
  int radiusInsufficientProfileCount = 0;
  int orientationConflictTransitionCount = 0;
  int orientationUnobservedTransitionCount = 0;
  int orientationAnalyticBridgedTransitionCount = 0;
  bool geometryRoleValidated = false;
  int directionEvaluatedLinkCount = 0;
  double meanSpineDirectionAlignment = 0.0;
  double minimumSpineDirectionAlignment = -1.0;
  double meanSpineDirectionSampleCoverage = 0.0;
  bool spineDirectionValidated = false;
  RadiusBehavior radiusBehavior = RadiusBehavior::InsufficientEvidence;
};

struct RadiusProfile {
  int pathId = 0;
  std::vector<int> faceIds;
  std::vector<double> nominalRadii;
  std::vector<double> minimumRadii;
  std::vector<double> maximumRadii;
  double minimumRadius = 0.0;
  double maximumRadius = 0.0;
  double startRadius = 0.0;
  double endRadius = 0.0;
  double maximumRelativeStep = 0.0;
  double maximumSampleRelativeStep = 0.0;
  double sampledTotalVariation = 0.0;
  double dominantStepVariationFraction = 0.0;
  double activeStepFraction = 0.0;
  double monotonicStepFraction = 0.0;
  int variableFaceCount = 0;
  int streamlineFaceCount = 0;
  int parameterLineFallbackFaceCount = 0;
  int rejectedLowAlignmentTraceFaceCount = 0;
  int stableAcrossSeedsTraceFaceCount = 0;
  int unstableAcrossSeedsTraceFaceCount = 0;
  int insufficientSeedStabilityTraceFaceCount = 0;
  bool globalOrientationOptimized = false;
  int orientationTraceFaceCount = 0;
  int orientationReversedFaceCount = 0;
  double orientationTransitionCost = 0.0;
  double greedyOrientationTransitionCost = 0.0;
  double orientationClosureGap = 0.0;
  struct OrientationTransitionEvidence {
    int firstFaceId = 0;
    int secondFaceId = 0;
    bool closure = false;
    int skippedFaceCount = 0;
    double normalizedGap = 0.0;
    double directionAlignment = 0.0;
    double cost = 0.0;
    bool analyticBridgeAttempted = false;
    int analyticBridgeHopCount = 0;
    int analyticBridgeEvaluatedHopCount = 0;
    double minimumAnalyticBridgeAlignment = -1.0;
    bool analyticBridgeValidated = false;
    bool analyticBridgeIncludedInOptimization = false;
    double analyticBridgeSignedAlignment = -1.0;
    double analyticBridgeOrientationCost = 0.0;
    bool accepted = false;
    OrientationTransitionIssue issue = OrientationTransitionIssue::None;
  };
  std::vector<OrientationTransitionEvidence> orientationTransitions;
  int acceptedOrientationTransitionCount = 0;
  int gapConflictOrientationTransitionCount = 0;
  int directionConflictOrientationTransitionCount = 0;
  int unobservedOrientationTransitionCount = 0;
  int analyticBridgedOrientationTransitionCount = 0;
  int analyticBridgeConflictOrientationTransitionCount = 0;
  int analyticBridgeOptimizedOrientationTransitionCount = 0;
  bool orientationTransitionsValidated = false;
  struct Sample {
    int faceId = 0;
    double pathParameter = 0.0;
    RadiusSampleEvidence evidence;
  };
  std::vector<Sample> samples;
  RadiusBehavior behavior = RadiusBehavior::InsufficientEvidence;
};

struct Result {
  struct PerformanceDiagnostics {
    double diagnosticsMilliseconds = 0.0;
    double faceAnalysisMilliseconds = 0.0;
    double topologyMilliseconds = 0.0;
    double radiusProfileMilliseconds = 0.0;
    double featureAggregationMilliseconds = 0.0;
    double totalMilliseconds = 0.0;
    double budgetMilliseconds = 0.0;
    bool budgetEnabled = false;
    bool budgetExceeded = false;
    int geometricSupportTestCount = 0;
    int linkPairIndexEntryCount = 0;
    int linkPairLookupCount = 0;
    int linkCandidatesVisitedThroughIndex = 0;
  } performance;
  Diagnostics diagnostics;
  std::vector<FaceEvidence> faces;
  std::vector<Link> links;
  std::vector<Chain> chains;
  std::vector<Path> paths;
  std::vector<Feature> features;
  std::vector<RadiusProfile> radiusProfiles;
};

struct SewingOptions {
  double toleranceFactor = 1.0;
  double maximumToleranceToModelDiagonal = 1.0e-5;
  bool analyzeDegeneratedShapes = true;
  bool cutFreeEdges = false;
  bool partitionByFaceProximity = true;
};

struct SewingFaceMapping {
  int originalFaceId = 0;
  std::string originalPersistentId;
  std::vector<int> sewnFaceIds;
  std::vector<std::string> sewnPersistentIds;
  bool modified = false;
  bool mapped = false;
};

struct SewingComparison {
  Result original;
  Result sewn;
  TopoDS_Shape sewnShape;
  double sewingTolerance = 0.0;
  int freeEdgeCount = 0;
  int contiguousEdgeCount = 0;
  int multipleEdgeCount = 0;
  int deletedFaceCount = 0;
  int mappedFaceCount = 0;
  int modifiedFaceCount = 0;
  int partitionCount = 0;
  int maximumPartitionFaceCount = 0;
  int unmappedFaceCount = 0;
  int splitFaceMappingCount = 0;
  double totalFreeEdgeLength = 0.0;
  double maximumFreeEdgeLength = 0.0;
  int originalShellCount = 0;
  int sewnShellCount = 0;
  int originalClosedShellCount = 0;
  int sewnClosedShellCount = 0;
  bool topologySafetyGatePassed = false;
  std::vector<SewingFaceMapping> faceMappings;
};

Result recognize(const TopoDS_Shape& shape, const Options& options = {});
SewingComparison compareWithSewing(const TopoDS_Shape& shape,
                                   const Options& recognitionOptions = {},
                                   const SewingOptions& sewingOptions = {});

} // namespace fillet::improved
