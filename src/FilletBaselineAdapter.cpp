#include "fillet/FilletBaseline.hpp"

#include <iostream>

// Compile the user's source verbatim as the algorithm baseline. This adapter
// only maps public input/output structures; it contains no recognition rules.
#include FILLET_BASELINE_SOURCE

namespace fillet::baseline {

std::vector<Chain> search(const TopoDS_Shape& shape,
                          double minimumRadius,
                          double maximumRadius,
                          const Options& options) {
  occ::FilletSearchOptions sourceOptions;
  sourceOptions.includeCylinder = options.includeCylinder;
  sourceOptions.includeTorus = options.includeTorus;
  sourceOptions.includeSphere = options.includeSphere;
  sourceOptions.requireSimilarRadius = options.requireSimilarRadius;
  sourceOptions.radiusAbsTolerance = options.radiusAbsTolerance;
  sourceOptions.radiusRelTolerance = options.radiusRelTolerance;
  sourceOptions.tangentAngleToleranceRadians = options.tangentAngleToleranceRadians;
  sourceOptions.minimumTangentSupportSides = options.minimumTangentSupportSides;
  sourceOptions.rejectWithoutEnoughSupports = options.rejectWithoutEnoughSupports;

  const std::vector<occ::FilletChain> sourceChains =
      occ::FilletChainSearcher::Search(shape, minimumRadius, maximumRadius, sourceOptions);

  std::vector<Chain> result;
  result.reserve(sourceChains.size());
  for (const occ::FilletChain& source : sourceChains) {
    Chain target;
    target.faces = source.faces;
    target.boundaryEdges = source.boundaryEdges;
    target.tangentSupportEdges = source.tangentSupportEdges;
    target.endEdges = source.endEdges;
    target.minimumRadius = source.minimumRadius;
    target.maximumRadius = source.maximumRadius;
    target.meanRadius = source.meanRadius;
    target.tangentSupportSideCount = source.tangentSupportSideCount;
    target.isClosed = source.isClosed;
    target.isBranched = source.isBranched;
    result.push_back(std::move(target));
  }
  return result;
}

} // namespace fillet::baseline
