#include "fillet/RecognitionReport.hpp"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepTools.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace fillet::report {
namespace {

std::string safeName(std::string value) {
  for (char& character : value)
    if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '-' ||
          character == '_')) character = '_';
  return value.empty() ? "model" : value;
}

std::string csv(const std::string& value) {
  if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
  std::string escaped = "\"";
  for (char character : value) escaped += character == '\"' ? "\"\"" : std::string(1, character);
  return escaped + "\"";
}

template <typename Range>
std::string join(const Range& values, char separator = ';') {
  std::ostringstream stream;
  bool first = true;
  for (const auto& value : values) {
    if (!first) stream << separator;
    first = false;
    stream << value;
  }
  return stream.str();
}

const char* sourceName(improved::GeometrySource value) {
  using improved::GeometrySource;
  switch (value) {
    case GeometrySource::AnalyticCylinder: return "analytic-cylinder";
    case GeometrySource::AnalyticTorus: return "analytic-torus";
    case GeometrySource::AnalyticSphere: return "analytic-sphere";
    case GeometrySource::RecoveredCylinder: return "recovered-cylinder";
    case GeometrySource::RecoveredSphere: return "recovered-sphere";
    case GeometrySource::CurvatureField: return "curvature-field";
  }
  return "unknown";
}

const char* verdictName(improved::Verdict value) {
  using improved::Verdict;
  switch (value) {
    case Verdict::Accepted: return "accepted";
    case Verdict::RejectedNoSupports: return "rejected-no-supports";
    case Verdict::RejectedRadius: return "rejected-radius";
    case Verdict::RejectedGeometry: return "rejected-geometry";
    case Verdict::RejectedLikelyPrimarySurface: return "rejected-primary-risk";
    case Verdict::RejectedNumericallyUnresolved:
      return "rejected-numerically-unresolved";
    case Verdict::EvaluationFailed: return "evaluation-failed";
  }
  return "unknown";
}

const char* convexityName(improved::Convexity value) {
  using improved::Convexity;
  switch (value) {
    case Convexity::Unknown: return "unknown";
    case Convexity::Convex: return "convex";
    case Convexity::Concave: return "concave";
  }
  return "unknown";
}

const char* chainVerdictName(improved::ChainVerdict value) {
  using improved::ChainVerdict;
  switch (value) {
    case ChainVerdict::Accepted: return "accepted";
    case ChainVerdict::NeedsReviewInsufficientExternalSupports:
      return "needs-review-insufficient-external-supports";
  }
  return "unknown";
}

const char* topologyRoleName(improved::TopologyRole value) {
  using improved::TopologyRole;
  switch (value) {
    case TopologyRole::Unknown: return "unknown";
    case TopologyRole::IsolatedPatch: return "isolated-patch";
    case TopologyRole::TerminalPatch: return "terminal-patch";
    case TopologyRole::BandPatch: return "band-patch";
    case TopologyRole::JunctionPatch: return "junction-patch";
  }
  return "unknown";
}

const char* featureKindName(improved::FeatureKind value) {
  using improved::FeatureKind;
  switch (value) {
    case FeatureKind::IsolatedPatch: return "isolated-patch";
    case FeatureKind::SimpleChain: return "simple-chain";
    case FeatureKind::ClosedLoop: return "closed-loop";
    case FeatureKind::CompositeJunctionNetwork: return "composite-junction-network";
  }
  return "unknown";
}

const char* radiusBehaviorName(improved::RadiusBehavior value) {
  using improved::RadiusBehavior;
  switch (value) {
    case RadiusBehavior::InsufficientEvidence: return "insufficient-evidence";
    case RadiusBehavior::Constant: return "constant";
    case RadiusBehavior::SmoothVariable: return "smooth-variable-candidate";
    case RadiusBehavior::Segmented: return "segmented";
    case RadiusBehavior::Discontinuous: return "discontinuous";
  }
  return "unknown";
}

const char* radiusTraceMethodName(improved::RadiusTraceMethod value) {
  using improved::RadiusTraceMethod;
  switch (value) {
    case RadiusTraceMethod::None: return "none";
    case RadiusTraceMethod::PrincipalDirectionStreamline: return "principal-direction-streamline";
    case RadiusTraceMethod::ParameterLineFallback: return "parameter-line-fallback";
  }
  return "unknown";
}

const char* orientationTransitionIssueName(
    improved::OrientationTransitionIssue value) {
  using improved::OrientationTransitionIssue;
  switch (value) {
    case OrientationTransitionIssue::None: return "none";
    case OrientationTransitionIssue::UnobservedIntermediateFaces:
      return "unobserved-intermediate-faces";
    case OrientationTransitionIssue::GapTooLarge: return "gap-too-large";
    case OrientationTransitionIssue::DirectionMismatch: return "direction-mismatch";
    case OrientationTransitionIssue::GapAndDirectionMismatch:
      return "gap-and-direction-mismatch";
  }
  return "unknown";
}

const char* evidenceStateName(improved::EvidenceState value) {
  using improved::EvidenceState;
  switch (value) {
    case EvidenceState::Validated: return "validated";
    case EvidenceState::Conflict: return "conflict";
    case EvidenceState::InsufficientEvidence: return "insufficient-evidence";
  }
  return "unknown";
}

const char* roleConsistencyName(improved::RoleConsistency value) {
  using improved::RoleConsistency;
  switch (value) {
    case RoleConsistency::Unknown: return "unknown";
    case RoleConsistency::Consistent: return "consistent";
    case RoleConsistency::NeedsReview: return "needs-review";
  }
  return "unknown";
}

const char* directionSourceName(improved::DirectionEvidenceSource value) {
  using improved::DirectionEvidenceSource;
  switch (value) {
    case DirectionEvidenceSource::None: return "none";
    case DirectionEvidenceSource::SharedEdge: return "shared-edge";
    case DirectionEvidenceSource::ProjectedBoundary: return "projected-boundary";
  }
  return "none";
}

std::ofstream output(const std::filesystem::path& path) {
  std::ofstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot create report file: " + path.string());
  stream << std::setprecision(17);
  return stream;
}

} // namespace

std::filesystem::path write(const improved::Result& result,
                            const TopoDS_Shape& sourceShape,
                            const std::filesystem::path& outputRoot,
                            const std::string& modelName) {
  (void)sourceShape;
  const std::filesystem::path directory = outputRoot / safeName(modelName);
  std::filesystem::create_directories(directory);

  std::size_t accepted = 0;
  for (const auto& face : result.faces) {
    if (face.verdict == improved::Verdict::Accepted) ++accepted;
  }

  auto summary = output(directory / "summary.json");
  summary << "{\n  \"valid\": " << (result.diagnostics.valid ? "true" : "false")
          << ",\n  \"model_face_count\": " << result.diagnostics.faceCount
          << ",\n  \"model_edge_count\": " << result.diagnostics.edgeCount
          << ",\n  \"short_edge_count\": " << result.diagnostics.shortEdgeCount
          << ",\n  \"sliver_face_count\": " << result.diagnostics.sliverFaceCount
          << ",\n  \"model_diagonal\": " << result.diagnostics.modelDiagonal
          << ",\n  \"effective_tolerance\": " << result.diagnostics.effectiveLinearTolerance
          << ",\n  \"accepted_face_count\": " << accepted
          << ",\n  \"chain_count\": " << result.chains.size()
          << ",\n  \"path_count\": " << result.paths.size()
          << ",\n  \"radius_profile_count\": " << result.radiusProfiles.size()
          << ",\n  \"feature_count\": " << result.features.size()
          << ",\n  \"link_count\": " << result.links.size()
          << ",\n  \"recognition_milliseconds\": "
          << result.performance.totalMilliseconds
          << ",\n  \"performance_budget_enabled\": "
          << (result.performance.budgetEnabled ? "true" : "false")
          << ",\n  \"performance_budget_exceeded\": "
          << (result.performance.budgetExceeded ? "true" : "false") << "\n}\n";

  auto performance = output(directory / "performance.csv");
  performance << "diagnostics_ms,face_analysis_ms,topology_ms,radius_profile_ms,"
                 "feature_aggregation_ms,total_ms,budget_ms,budget_enabled,budget_exceeded,"
                 "geometric_support_tests,link_pair_index_entries,link_pair_lookups,"
                 "link_candidates_visited_through_index\n"
              << result.performance.diagnosticsMilliseconds << ','
              << result.performance.faceAnalysisMilliseconds << ','
              << result.performance.topologyMilliseconds << ','
              << result.performance.radiusProfileMilliseconds << ','
              << result.performance.featureAggregationMilliseconds << ','
              << result.performance.totalMilliseconds << ','
              << result.performance.budgetMilliseconds << ','
              << result.performance.budgetEnabled << ','
              << result.performance.budgetExceeded << ','
              << result.performance.geometricSupportTestCount << ','
              << result.performance.linkPairIndexEntryCount << ','
              << result.performance.linkPairLookupCount << ','
              << result.performance.linkCandidatesVisitedThroughIndex << '\n';

  auto faces = output(directory / "faces.csv");
  faces << "face_id,persistent_id,verdict,geometry_source,radius,min_radius,max_radius,"
           "canonical_gap,angular_coverage,surface_area,area_ratio,area_to_diagonal_squared,"
           "numerically_resolved,radius_to_diagonal,"
           "confidence,secondary_curvature_ratio,same_sign_curvature_fraction,double_curved,"
           "variable_radius,radius_trace_method,radius_trace_coverage,"
           "radius_trace_minimum_tangent_alignment,radius_trace_normalized_length,"
           "radius_trace_seed_u,radius_trace_seed_v,radius_trace_seed_attempts,"
           "radius_trace_adaptive_step_reductions,radius_trace_best_streamline_coverage,"
           "radius_trace_best_streamline_normalized_length,"
           "radius_trace_best_streamline_sample_count,"
           "radius_trace_total_attempted_step_reductions,radius_trace_valid_seed_count,"
           "radius_trace_maximum_cross_seed_relative_deviation,"
           "radius_trace_stable_across_seeds,radius_trace_sample_count,"
           "primary_risk,convexity,topology_role,role_consistency,"
           "tangent_neighbor_ids,external_support_ids,"
           "inferred_support_ids,tangent_boundaries,non_tangent_boundaries,reason,role_reason\n";
  for (const auto& face : result.faces)
    faces << face.faceId << ',' << face.persistentId << ',' << verdictName(face.verdict) << ','
          << (face.verdict == improved::Verdict::RejectedGeometry
                  ? "unrecognized" : sourceName(face.geometrySource))
          << ',' << face.radius << ',' << face.minimumRadius
          << ',' << face.maximumRadius << ',' << face.canonicalGap << ',' << face.angularCoverage
          << ',' << face.surfaceArea << ',' << face.areaRatio << ','
          << face.areaToModelDiagonalSquared << ',' << face.numericallyResolved << ','
          << face.radiusToModelDiagonal
          << ',' << face.confidence << ',' << face.secondaryCurvatureRatio << ','
          << face.sameSignCurvatureFraction << ',' << face.doubleCurved << ','
          << face.variableRadius << ',' << radiusTraceMethodName(face.radiusTraceMethod) << ','
          << face.radiusTraceCoverage << ',' << face.radiusTraceMinimumTangentAlignment << ','
          << face.radiusTraceNormalizedLength << ',' << face.radiusTraceSeedU << ','
          << face.radiusTraceSeedV << ',' << face.radiusTraceSeedAttempts << ','
          << face.radiusTraceAdaptiveStepReductions << ','
          << face.radiusTraceBestStreamlineCoverage << ','
          << face.radiusTraceBestStreamlineNormalizedLength << ','
          << face.radiusTraceBestStreamlineSampleCount << ','
          << face.radiusTraceTotalAttemptedStepReductions << ','
          << face.radiusTraceValidSeedCount << ','
          << face.radiusTraceMaximumCrossSeedRelativeDeviation << ','
          << face.radiusTraceStableAcrossSeeds << ','
          << face.radiusSamples.size() << ','
          << face.likelyPrimarySurface << ',' << convexityName(face.convexity) << ','
          << topologyRoleName(face.topologyRole) << ','
          << roleConsistencyName(face.roleConsistency) << ','
          << csv(join(face.supportFaceIds)) << ',' << csv(join(face.externalSupportFaceIds)) << ','
          << csv(join(face.inferredSupportFaceIds)) << ','
          << face.tangentBoundaryCount << ',' << face.nonTangentBoundaryCount << ','
          << csv(face.reason) << ',' << csv(face.roleReason) << '\n';

  auto faceRadiusSamples = output(directory / "face-radius-samples.csv");
  faceRadiusSamples << "face_id,trace_method,trace_coverage,"
                       "trace_minimum_tangent_alignment,trace_normalized_length,"
                       "trace_seed_u,trace_seed_v,trace_seed_attempts,"
                       "trace_adaptive_step_reductions,best_streamline_coverage,"
                       "best_streamline_normalized_length,best_streamline_sample_count,"
                       "total_attempted_step_reductions,valid_seed_count,"
                       "maximum_cross_seed_relative_deviation,stable_across_seeds,sample_index,"
                       "normalized_spine_parameter,u,v,x,y,z,radius,"
                       "secondary_curvature_ratio,spine_direction_x,spine_direction_y,"
                       "spine_direction_z\n";
  for (const auto& face : result.faces)
    for (std::size_t index = 0; index < face.radiusSamples.size(); ++index) {
      const auto& sample = face.radiusSamples[index];
      faceRadiusSamples << face.faceId << ',' << radiusTraceMethodName(face.radiusTraceMethod)
          << ',' << face.radiusTraceCoverage << ','
          << face.radiusTraceMinimumTangentAlignment << ','
          << face.radiusTraceNormalizedLength << ',' << face.radiusTraceSeedU << ','
          << face.radiusTraceSeedV << ',' << face.radiusTraceSeedAttempts << ','
          << face.radiusTraceAdaptiveStepReductions << ','
          << face.radiusTraceBestStreamlineCoverage << ','
          << face.radiusTraceBestStreamlineNormalizedLength << ','
          << face.radiusTraceBestStreamlineSampleCount << ','
          << face.radiusTraceTotalAttemptedStepReductions << ','
          << face.radiusTraceValidSeedCount << ','
          << face.radiusTraceMaximumCrossSeedRelativeDeviation << ','
          << face.radiusTraceStableAcrossSeeds << ',' << index + 1 << ','
          << sample.normalizedSpineParameter << ',' << sample.u << ',' << sample.v << ','
          << sample.x << ',' << sample.y << ',' << sample.z << ',' << sample.radius << ','
          << sample.secondaryCurvatureRatio << ',' << sample.spineDirectionX << ','
          << sample.spineDirectionY << ',' << sample.spineDirectionZ << '\n';
    }

  auto links = output(directory / "links.csv");
  links << "first_face_id,second_face_id,shared_edge,shared_edge_length,"
           "shared_edge_length_to_diagonal,rejected_short_shared_edge,tangent,"
           "radius_compatible,shared_support_pair,used_in_graph,"
           "direction_evidence_source,spine_direction_evaluated,spine_direction_compatible,spine_direction_requested_samples,"
           "spine_direction_valid_samples,spine_direction_sample_coverage,minimum_spine_direction_alignment,"
           "mean_spine_direction_alignment,spine_direction_alignment_standard_deviation,"
           "spine_direction_alignment_samples,spine_direction_projection_gap_samples,"
           "direction_refinement_attempted,direction_refinement_accepted,"
           "initial_direction_valid_samples,initial_minimum_spine_direction_alignment\n";
  for (const auto& link : result.links)
    links << link.firstFaceId << ',' << link.secondFaceId << ',' << link.sharedEdge << ','
          << link.sharedEdgeLength << ',' << link.sharedEdgeLengthToModelDiagonal << ','
          << link.rejectedShortSharedEdge << ',' << link.tangent << ','
          << link.radiusCompatible << ',' << link.sharedSupportPair << ','
          << link.usedInGraph << ','
          << directionSourceName(link.directionEvidenceSource) << ','
          << link.spineDirectionEvaluated << ',' << link.spineDirectionCompatible << ','
          << link.spineDirectionRequestedSamples << ',' << link.spineDirectionValidSamples << ','
          << link.spineDirectionSampleCoverage << ',' << link.minimumSpineDirectionAlignment << ','
          << link.spineDirectionAlignment << ','
          << link.spineDirectionAlignmentStandardDeviation << ','
          << csv(join(link.spineDirectionAlignmentSamples)) << ','
          << csv(join(link.spineDirectionProjectionGapSamples)) << ','
          << link.directionRefinementAttempted << ',' << link.directionRefinementAccepted << ','
          << link.initialDirectionValidSamples << ','
          << link.initialMinimumSpineDirectionAlignment << '\n';

  auto directionSamples = output(directory / "direction-samples.csv");
  directionSamples << "link_id,first_face_id,second_face_id,evidence_source,used_in_graph,"
                       "link_compatible,sample_index,alignment,gap,first_x,first_y,first_z,"
                       "second_x,second_y,second_z,first_u,first_v,second_u,second_v,"
                       "first_secondary_curvature_ratio,second_secondary_curvature_ratio,"
                       "first_direction_x,first_direction_y,first_direction_z,"
                       "second_direction_x,second_direction_y,second_direction_z\n";
  BRep_Builder conflictBuilder;
  TopoDS_Compound conflictPoints;
  conflictBuilder.MakeCompound(conflictPoints);
  TopoDS_Compound conflictVectors;
  conflictBuilder.MakeCompound(conflictVectors);
  const double vectorHalfLength = std::max(result.diagnostics.modelDiagonal * 0.005,
                                           result.diagnostics.effectiveLinearTolerance * 10.0);
  for (std::size_t linkIndex = 0; linkIndex < result.links.size(); ++linkIndex) {
    const auto& link = result.links[linkIndex];
    for (std::size_t sampleIndex = 0; sampleIndex < link.directionSamples.size(); ++sampleIndex) {
      const auto& sample = link.directionSamples[sampleIndex];
      directionSamples << linkIndex + 1 << ',' << link.firstFaceId << ',' << link.secondFaceId
          << ',' << directionSourceName(link.directionEvidenceSource) << ',' << link.usedInGraph
          << ',' << link.spineDirectionCompatible << ',' << sampleIndex + 1 << ','
          << sample.alignment << ',' << sample.gap << ',' << sample.firstX << ','
          << sample.firstY << ',' << sample.firstZ << ',' << sample.secondX << ','
          << sample.secondY << ',' << sample.secondZ << ',' << sample.firstU << ','
          << sample.firstV << ',' << sample.secondU << ',' << sample.secondV << ','
          << sample.firstSecondaryCurvatureRatio << ','
          << sample.secondSecondaryCurvatureRatio << ','
          << sample.firstDirectionX << ',' << sample.firstDirectionY << ','
          << sample.firstDirectionZ << ',' << sample.secondDirectionX << ','
          << sample.secondDirectionY << ',' << sample.secondDirectionZ << '\n';

      if (link.usedInGraph && link.spineDirectionEvaluated &&
          !link.spineDirectionCompatible) {
        const gp_Pnt midpoint(0.5 * (sample.firstX + sample.secondX),
                             0.5 * (sample.firstY + sample.secondY),
                             0.5 * (sample.firstZ + sample.secondZ));
        conflictBuilder.Add(conflictPoints, BRepBuilderAPI_MakeVertex(midpoint).Vertex());
        const gp_Pnt firstPoint(sample.firstX, sample.firstY, sample.firstZ);
        const gp_Pnt secondPoint(sample.secondX, sample.secondY, sample.secondZ);
        const gp_Vec firstVector(sample.firstDirectionX * vectorHalfLength,
                                 sample.firstDirectionY * vectorHalfLength,
                                 sample.firstDirectionZ * vectorHalfLength);
        const gp_Vec secondVector(sample.secondDirectionX * vectorHalfLength,
                                  sample.secondDirectionY * vectorHalfLength,
                                  sample.secondDirectionZ * vectorHalfLength);
        conflictBuilder.Add(conflictVectors, BRepBuilderAPI_MakeEdge(
            firstPoint.Translated(firstVector.Reversed()),
            firstPoint.Translated(firstVector)).Edge());
        conflictBuilder.Add(conflictVectors, BRepBuilderAPI_MakeEdge(
            secondPoint.Translated(secondVector.Reversed()),
            secondPoint.Translated(secondVector)).Edge());
      }
    }
  }
  if (!BRepTools::Write(conflictPoints,
                        (directory / "direction-conflict-points.brep").string().c_str()))
    throw std::runtime_error("cannot write direction-conflict-points.brep");
  if (!BRepTools::Write(conflictVectors,
                        (directory / "direction-conflict-vectors.brep").string().c_str()))
    throw std::runtime_error("cannot write direction-conflict-vectors.brep");

  auto chains = output(directory / "chains.csv");
  chains << "chain_id,chain_verdict,face_ids,external_support_ids,min_radius,max_radius,"
            "mean_radius,confidence,closed,branched\n";
  for (std::size_t index = 0; index < result.chains.size(); ++index) {
    const auto& chain = result.chains[index];
    chains << index + 1 << ',' << chainVerdictName(chain.verdict) << ','
           << csv(join(chain.faceIds)) << ','
           << csv(join(chain.supportFaceIds)) << ',' << chain.minimumRadius << ','
           << chain.maximumRadius << ',' << chain.meanRadius << ',' << chain.confidence << ','
           << chain.isClosed << ',' << chain.isBranched << '\n';
  }

  auto paths = output(directory / "paths.csv");
  paths << "path_id,face_ids,start_face_id,end_face_id,closed,starts_at_branch,ends_at_branch\n";
  for (std::size_t index = 0; index < result.paths.size(); ++index) {
    const auto& path = result.paths[index];
    paths << index + 1 << ',' << csv(join(path.faceIds)) << ',' << path.startNodeFaceId << ','
          << path.endNodeFaceId << ',' << path.isClosed << ',' << path.startsAtBranch << ','
          << path.endsAtBranch << '\n';
  }

  auto radiusProfiles = output(directory / "radius-profiles.csv");
  radiusProfiles << "profile_id,path_id,behavior,face_ids,nominal_radii,minimum_radii,"
                     "maximum_radii,minimum_radius,maximum_radius,start_radius,end_radius,"
                     "maximum_relative_step,maximum_sample_relative_step,sampled_total_variation,"
                     "dominant_step_variation_fraction,active_step_fraction,monotonic_step_fraction,"
                     "sample_count,variable_face_count,streamline_face_count,"
                     "parameter_line_fallback_face_count,rejected_low_alignment_trace_face_count,"
                     "stable_across_seeds_trace_face_count,"
                     "unstable_across_seeds_trace_face_count,"
                     "insufficient_seed_stability_trace_face_count,"
                     "global_orientation_optimized,orientation_trace_face_count,"
                     "orientation_reversed_face_count,orientation_transition_cost,"
                     "greedy_orientation_transition_cost,orientation_closure_gap,"
                     "orientation_transition_count,accepted_orientation_transition_count,"
                     "gap_conflict_orientation_transition_count,"
                     "direction_conflict_orientation_transition_count,"
                     "unobserved_orientation_transition_count,"
                     "analytic_bridged_orientation_transition_count,"
                     "analytic_bridge_conflict_orientation_transition_count,"
                     "analytic_bridge_optimized_orientation_transition_count,"
                     "orientation_transitions_validated\n";
  for (std::size_t index = 0; index < result.radiusProfiles.size(); ++index) {
    const auto& profile = result.radiusProfiles[index];
    radiusProfiles << index + 1 << ',' << profile.pathId << ','
                   << radiusBehaviorName(profile.behavior) << ','
                   << csv(join(profile.faceIds)) << ','
                   << csv(join(profile.nominalRadii)) << ','
                   << csv(join(profile.minimumRadii)) << ','
                   << csv(join(profile.maximumRadii)) << ','
                   << profile.minimumRadius << ',' << profile.maximumRadius << ','
                   << profile.startRadius << ',' << profile.endRadius << ','
                   << profile.maximumRelativeStep << ',' << profile.maximumSampleRelativeStep << ','
                   << profile.sampledTotalVariation << ','
                   << profile.dominantStepVariationFraction << ','
                   << profile.activeStepFraction << ',' << profile.monotonicStepFraction << ','
                   << profile.samples.size() << ','
                   << profile.variableFaceCount << ',' << profile.streamlineFaceCount << ','
                   << profile.parameterLineFallbackFaceCount << ','
                   << profile.rejectedLowAlignmentTraceFaceCount << ','
                   << profile.stableAcrossSeedsTraceFaceCount << ','
                   << profile.unstableAcrossSeedsTraceFaceCount << ','
                   << profile.insufficientSeedStabilityTraceFaceCount << ','
                   << profile.globalOrientationOptimized << ','
                   << profile.orientationTraceFaceCount << ','
                   << profile.orientationReversedFaceCount << ','
                   << profile.orientationTransitionCost << ','
                   << profile.greedyOrientationTransitionCost << ','
                   << profile.orientationClosureGap << ','
                   << profile.orientationTransitions.size() << ','
                   << profile.acceptedOrientationTransitionCount << ','
                   << profile.gapConflictOrientationTransitionCount << ','
                   << profile.directionConflictOrientationTransitionCount << ','
                   << profile.unobservedOrientationTransitionCount << ','
                   << profile.analyticBridgedOrientationTransitionCount << ','
                   << profile.analyticBridgeConflictOrientationTransitionCount << ','
                   << profile.analyticBridgeOptimizedOrientationTransitionCount << ','
                   << profile.orientationTransitionsValidated << '\n';
  }

  auto orientationTransitions = output(directory / "radius-orientation-transitions.csv");
  orientationTransitions << "profile_id,path_id,transition_index,first_face_id,second_face_id,"
                            "closure,skipped_face_count,normalized_gap,direction_alignment,"
                            "cost,analytic_bridge_attempted,analytic_bridge_hop_count,"
                            "analytic_bridge_evaluated_hop_count,"
                            "minimum_analytic_bridge_alignment,analytic_bridge_validated,"
                            "analytic_bridge_included_in_optimization,"
                            "analytic_bridge_signed_alignment,analytic_bridge_orientation_cost,"
                            "accepted,issue\n";
  for (std::size_t profileIndex = 0; profileIndex < result.radiusProfiles.size();
       ++profileIndex) {
    const auto& profile = result.radiusProfiles[profileIndex];
    for (std::size_t transitionIndex = 0;
         transitionIndex < profile.orientationTransitions.size(); ++transitionIndex) {
      const auto& transition = profile.orientationTransitions[transitionIndex];
      orientationTransitions << profileIndex + 1 << ',' << profile.pathId << ','
          << transitionIndex + 1 << ',' << transition.firstFaceId << ','
          << transition.secondFaceId << ',' << transition.closure << ','
          << transition.skippedFaceCount << ',' << transition.normalizedGap << ','
          << transition.directionAlignment << ',' << transition.cost << ','
          << transition.analyticBridgeAttempted << ','
          << transition.analyticBridgeHopCount << ','
          << transition.analyticBridgeEvaluatedHopCount << ','
          << transition.minimumAnalyticBridgeAlignment << ','
          << transition.analyticBridgeValidated << ','
          << transition.analyticBridgeIncludedInOptimization << ','
          << transition.analyticBridgeSignedAlignment << ','
          << transition.analyticBridgeOrientationCost << ','
          << transition.accepted << ','
          << orientationTransitionIssueName(transition.issue) << '\n';
    }
  }

  auto radiusProfileSamples = output(directory / "radius-profile-samples.csv");
  radiusProfileSamples << "profile_id,path_id,sample_index,face_id,path_parameter,"
                          "face_parameter,u,v,x,y,z,radius,secondary_curvature_ratio,"
                          "spine_direction_x,spine_direction_y,spine_direction_z\n";
  for (std::size_t profileIndex = 0; profileIndex < result.radiusProfiles.size();
       ++profileIndex) {
    const auto& profile = result.radiusProfiles[profileIndex];
    for (std::size_t sampleIndex = 0; sampleIndex < profile.samples.size(); ++sampleIndex) {
      const auto& sample = profile.samples[sampleIndex];
      radiusProfileSamples << profileIndex + 1 << ',' << profile.pathId << ','
          << sampleIndex + 1 << ',' << sample.faceId << ',' << sample.pathParameter << ','
          << sample.evidence.normalizedSpineParameter << ',' << sample.evidence.u << ','
          << sample.evidence.v << ',' << sample.evidence.x << ',' << sample.evidence.y << ','
          << sample.evidence.z << ',' << sample.evidence.radius << ','
          << sample.evidence.secondaryCurvatureRatio << ','
          << sample.evidence.spineDirectionX << ',' << sample.evidence.spineDirectionY << ','
          << sample.evidence.spineDirectionZ << '\n';
    }
  }
  BRep_Builder radiusTraceBuilder;
  TopoDS_Compound radiusTracePoints, radiusTraceSegments;
  radiusTraceBuilder.MakeCompound(radiusTracePoints);
  radiusTraceBuilder.MakeCompound(radiusTraceSegments);
  for (const auto& profile : result.radiusProfiles) {
    for (std::size_t index = 0; index < profile.samples.size(); ++index) {
      const auto& sample = profile.samples[index].evidence;
      const gp_Pnt point(sample.x, sample.y, sample.z);
      radiusTraceBuilder.Add(radiusTracePoints, BRepBuilderAPI_MakeVertex(point).Vertex());
      if (index == 0) continue;
      const auto& previous = profile.samples[index - 1].evidence;
      const gp_Pnt previousPoint(previous.x, previous.y, previous.z);
      if (previousPoint.Distance(point) > result.diagnostics.effectiveLinearTolerance)
        radiusTraceBuilder.Add(radiusTraceSegments,
                               BRepBuilderAPI_MakeEdge(previousPoint, point).Edge());
    }
  }
  if (!BRepTools::Write(radiusTracePoints,
                        (directory / "radius-trace-points.brep").string().c_str()))
    throw std::runtime_error("cannot write radius-trace-points.brep");
  if (!BRepTools::Write(radiusTraceSegments,
                        (directory / "radius-trace-segments.brep").string().c_str()))
    throw std::runtime_error("cannot write radius-trace-segments.brep");

  auto features = output(directory / "features.csv");
  features << "feature_id,chain_id,kind,verdict,geometry_role_validated,face_ids,band_face_ids,"
              "terminal_face_ids,junction_face_ids,isolated_face_ids,role_review_face_ids,"
              "role_unknown_face_ids,path_ids,radius_profile_ids,radius_behavior,external_support_ids,"
              "direction_evaluated_link_count,"
              "direction_conflict_link_ids,direction_insufficient_coverage_link_ids,"
              "minimum_spine_direction_alignment,mean_spine_direction_alignment,"
              "mean_spine_direction_sample_coverage,spine_direction_validated,"
              "topology_evidence_state,radius_evidence_state,direction_evidence_state,"
              "aggregate_evidence_state,radius_unstable_profile_count,"
              "radius_insufficient_profile_count,orientation_conflict_transition_count,"
              "orientation_unobserved_transition_count,"
              "orientation_analytic_bridged_transition_count,base_geometry_confidence,"
              "topology_evidence_score,radius_evidence_score,direction_evidence_score,"
              "aggregate_evidence_score,min_radius,max_radius,confidence\n";
  for (std::size_t index = 0; index < result.features.size(); ++index) {
    const auto& feature = result.features[index];
    features << index + 1 << ',' << feature.chainId << ',' << featureKindName(feature.kind) << ','
             << chainVerdictName(feature.verdict) << ',' << feature.geometryRoleValidated << ','
             << csv(join(feature.faceIds)) << ','
             << csv(join(feature.bandFaceIds)) << ',' << csv(join(feature.terminalFaceIds)) << ','
             << csv(join(feature.junctionFaceIds)) << ',' << csv(join(feature.isolatedFaceIds)) << ','
             << csv(join(feature.roleReviewFaceIds)) << ','
             << csv(join(feature.roleUnknownFaceIds)) << ','
             << csv(join(feature.pathIds)) << ',' << csv(join(feature.radiusProfileIds)) << ','
             << radiusBehaviorName(feature.radiusBehavior) << ','
             << csv(join(feature.externalSupportFaceIds)) << ','
             << feature.directionEvaluatedLinkCount << ','
             << csv(join(feature.directionConflictLinkIds)) << ','
             << csv(join(feature.directionInsufficientCoverageLinkIds)) << ','
             << feature.minimumSpineDirectionAlignment << ','
             << feature.meanSpineDirectionAlignment << ','
             << feature.meanSpineDirectionSampleCoverage << ','
             << feature.spineDirectionValidated << ','
             << evidenceStateName(feature.topologyEvidenceState) << ','
             << evidenceStateName(feature.radiusEvidenceState) << ','
             << evidenceStateName(feature.directionEvidenceState) << ','
             << evidenceStateName(feature.aggregateEvidenceState) << ','
             << feature.radiusUnstableProfileCount << ','
             << feature.radiusInsufficientProfileCount << ','
             << feature.orientationConflictTransitionCount << ','
             << feature.orientationUnobservedTransitionCount << ','
             << feature.orientationAnalyticBridgedTransitionCount << ','
             << feature.baseGeometryConfidence << ','
             << feature.topologyEvidenceScore << ','
             << feature.radiusEvidenceScore << ','
             << feature.directionEvidenceScore << ','
             << feature.aggregateEvidenceScore << ','
             << feature.minimumRadius << ',' << feature.maximumRadius << ','
             << feature.confidence << '\n';
  }

  auto graph = output(directory / "graph.dot");
  graph << "graph fillet_candidates {\n  overlap=false;\n";
  for (const auto& face : result.faces)
    graph << "  f" << face.faceId << " [label=\"" << face.faceId << "\\n"
          << face.persistentId << "\\nr=" << face.radius << " c=" << face.confidence
          << "\\n" << topologyRoleName(face.topologyRole)
          << "\", style=filled, fillcolor=\""
          << (face.verdict == improved::Verdict::Accepted ? "palegreen" : "lightgray") << "\"];\n";
  for (const auto& link : result.links)
    graph << "  f" << link.firstFaceId << " -- f" << link.secondFaceId
          << " [label=\"" << (link.sharedEdge ? "edge " : "")
          << (link.tangent ? "G1 " : "") << (link.sharedSupportPair ? "supports " : "")
          << (link.radiusCompatible ? "radius" : "radius-mismatch")
          << (link.directionEvidenceSource == improved::DirectionEvidenceSource::ProjectedBoundary
                  ? " projected" : "")
          << (link.spineDirectionEvaluated
                  ? (link.spineDirectionCompatible ? " dir-ok" : " dir-review") : "")
          << "\"];\n";
  graph << "}\n";

  auto truth = output(directory / "truth-template.csv");
  truth << "face_id,persistent_id,predicted_label,expected_label,expected_chain,"
           "expected_feature,expected_feature_kind,expected_radius_behavior,notes\n";
  for (const auto& face : result.faces)
    truth << face.faceId << ',' << face.persistentId << ','
          << (face.verdict == improved::Verdict::Accepted ? "FILLET" : "NOT_FILLET")
          << ",UNKNOWN,,,,,\n";

  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  for (const auto& face : result.faces)
    if (face.verdict == improved::Verdict::Accepted) builder.Add(compound, face.face);
  if (!BRepTools::Write(compound, (directory / "accepted-faces.brep").string().c_str()))
    throw std::runtime_error("cannot write accepted-faces.brep");
  return directory;
}

std::filesystem::path writeSewingComparison(
    const improved::SewingComparison& comparison,
    const std::filesystem::path& outputRoot,
    const std::string& modelName) {
  const std::filesystem::path directory = outputRoot / safeName(modelName);
  std::filesystem::create_directories(directory);
  write(comparison.original, TopoDS_Shape(), directory, "original");
  write(comparison.sewn, comparison.sewnShape, directory, "sewn");

  auto summary = output(directory / "sewing-comparison.csv");
  summary << "sewing_tolerance,free_edges,contiguous_edges,multiple_edges,deleted_faces,"
             "free_edge_total_length,free_edge_max_length,partitions,max_partition_faces,"
             "mapped_faces,unmapped_faces,split_face_mappings,modified_faces,"
             "original_faces,sewn_faces,original_edges,sewn_edges,original_shells,sewn_shells,"
             "original_closed_shells,sewn_closed_shells,topology_safety_gate_passed,"
             "original_chains,sewn_chains,original_features,sewn_features,original_valid,sewn_valid\n";
  summary << comparison.sewingTolerance << ',' << comparison.freeEdgeCount << ','
          << comparison.contiguousEdgeCount << ',' << comparison.multipleEdgeCount << ','
          << comparison.deletedFaceCount << ',' << comparison.totalFreeEdgeLength << ','
          << comparison.maximumFreeEdgeLength << ',' << comparison.partitionCount << ','
          << comparison.maximumPartitionFaceCount << ',' << comparison.mappedFaceCount << ','
          << comparison.unmappedFaceCount << ',' << comparison.splitFaceMappingCount << ','
          << comparison.modifiedFaceCount << ',' << comparison.original.diagnostics.faceCount << ','
          << comparison.sewn.diagnostics.faceCount << ','
          << comparison.original.diagnostics.edgeCount << ','
          << comparison.sewn.diagnostics.edgeCount << ',' << comparison.originalShellCount << ','
          << comparison.sewnShellCount << ',' << comparison.originalClosedShellCount << ','
          << comparison.sewnClosedShellCount << ',' << comparison.topologySafetyGatePassed << ','
          << comparison.original.chains.size() << ','
          << comparison.sewn.chains.size() << ',' << comparison.original.features.size() << ','
          << comparison.sewn.features.size() << ',' << comparison.original.diagnostics.valid << ','
          << comparison.sewn.diagnostics.valid << '\n';

  auto mappings = output(directory / "sewing-face-mapping.csv");
  mappings << "original_face_id,original_persistent_id,sewn_face_ids,sewn_persistent_ids,modified,mapped\n";
  for (const auto& mapping : comparison.faceMappings)
    mappings << mapping.originalFaceId << ',' << mapping.originalPersistentId << ','
             << csv(join(mapping.sewnFaceIds)) << ',' << csv(join(mapping.sewnPersistentIds)) << ','
             << mapping.modified << ',' << mapping.mapped << '\n';

  if (!BRepTools::Write(comparison.sewnShape,
                        (directory / "sewn-shape.brep").string().c_str()))
    throw std::runtime_error("cannot write sewn-shape.brep");
  return directory;
}

} // namespace fillet::report
