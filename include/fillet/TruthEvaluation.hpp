#pragma once

#include "fillet/ImprovedFilletRecognizer.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fillet::evaluation {

enum class TruthLabel { Fillet, NotFillet, Unknown };

struct TruthRecord {
  std::string persistentId;
  TruthLabel label = TruthLabel::Unknown;
  std::string expectedChain;
  std::string expectedFeature;
  std::string expectedFeatureKind;
  std::string expectedRadiusBehavior;
  std::string notes;
};

struct Metrics {
  bool available = false;
  int truthPositive = 0;
  int predictedPositive = 0;
  int truePositive = 0;
  int falsePositive = 0;
  int falseNegative = 0;
  int trueNegative = 0;
  double precision = 0.0;
  double recall = 0.0;
  double f1 = 0.0;
  double accuracy = 0.0;
  double meanIntersectionOverUnion = 0.0;
};

struct Match {
  std::string truthId;
  int predictedId = 0;
  double intersectionOverUnion = 0.0;
  bool kindCorrect = false;
  bool radiusBehaviorCorrect = false;
};

struct Result {
  int modelFaceCount = 0;
  int reviewedFaceCount = 0;
  int unknownFaceCount = 0;
  int missingTruthFaceCount = 0;
  int extraTruthRecordCount = 0;
  double reviewCoverage = 0.0;
  double matchingIntersectionOverUnion = 0.50;
  Metrics faces;
  Metrics chains;
  Metrics features;
  int matchedFeatureKindCount = 0;
  int correctFeatureKindCount = 0;
  int matchedRadiusBehaviorCount = 0;
  int correctRadiusBehaviorCount = 0;
  std::vector<Match> chainMatches;
  std::vector<Match> featureMatches;
};

std::vector<TruthRecord> read(const std::filesystem::path& path);
Result evaluate(const improved::Result& recognition,
                const std::vector<TruthRecord>& truth,
                double minimumIntersectionOverUnion = 0.50);
void write(const Result& evaluation, const std::filesystem::path& reportDirectory);

} // namespace fillet::evaluation
