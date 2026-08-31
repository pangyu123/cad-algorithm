#include "fillet/TruthEvaluation.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fillet::evaluation {
namespace {

std::vector<std::string> parseCsvRow(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char character = line[index];
    if (character == '"') {
      if (quoted && index + 1 < line.size() && line[index + 1] == '"') {
        field += '"'; ++index;
      } else {
        quoted = !quoted;
      }
    } else if (character == ',' && !quoted) {
      fields.push_back(field); field.clear();
    } else {
      field += character;
    }
  }
  fields.push_back(field);
  return fields;
}

std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

TruthLabel parseLabel(const std::string& value) {
  const std::string normalized = upper(value);
  if (normalized == "FILLET" || normalized == "POSITIVE" || normalized == "1")
    return TruthLabel::Fillet;
  if (normalized == "NOT_FILLET" || normalized == "NOT-FILLET" ||
      normalized == "NEGATIVE" || normalized == "0")
    return TruthLabel::NotFillet;
  if (normalized.empty() || normalized == "UNKNOWN" || normalized == "UNREVIEWED")
    return TruthLabel::Unknown;
  throw std::runtime_error("unknown truth label: " + value);
}

double ratio(int numerator, int denominator, bool emptyIsPerfect = false) {
  return denominator > 0 ? static_cast<double>(numerator) / denominator
                         : (emptyIsPerfect ? 1.0 : 0.0);
}

void finalize(Metrics& metrics) {
  metrics.precision = ratio(metrics.truePositive,
                            metrics.truePositive + metrics.falsePositive,
                            metrics.truthPositive == 0);
  metrics.recall = ratio(metrics.truePositive,
                         metrics.truePositive + metrics.falseNegative, true);
  const double sum = metrics.precision + metrics.recall;
  metrics.f1 = sum > 0.0 ? 2.0 * metrics.precision * metrics.recall / sum : 0.0;
  metrics.accuracy = ratio(metrics.truePositive + metrics.trueNegative,
                           metrics.truePositive + metrics.trueNegative +
                           metrics.falsePositive + metrics.falseNegative, true);
}

double intersectionOverUnion(const std::set<std::string>& first,
                             const std::set<std::string>& second) {
  std::vector<std::string> intersection;
  std::set_intersection(first.begin(), first.end(), second.begin(), second.end(),
                        std::back_inserter(intersection));
  const std::size_t unionSize = first.size() + second.size() - intersection.size();
  return unionSize > 0 ? static_cast<double>(intersection.size()) / unionSize : 0.0;
}

struct GroupEvaluation {
  Metrics metrics;
  std::vector<Match> matches;
};

GroupEvaluation evaluateGroups(
    const std::map<std::string, std::set<std::string>>& truthGroups,
    const std::vector<std::set<std::string>>& predictedGroups,
    double threshold) {
  GroupEvaluation result;
  result.metrics.available = true;
  result.metrics.truthPositive = static_cast<int>(truthGroups.size());
  result.metrics.predictedPositive = static_cast<int>(predictedGroups.size());
  std::vector<std::string> truthIds;
  for (const auto& entry : truthGroups) truthIds.push_back(entry.first);
  std::vector<std::vector<double>> iou(predictedGroups.size(),
                                      std::vector<double>(truthIds.size(), 0.0));
  for (std::size_t predicted = 0; predicted < predictedGroups.size(); ++predicted)
    for (std::size_t truth = 0; truth < truthIds.size(); ++truth)
      iou[predicted][truth] = intersectionOverUnion(
          predictedGroups[predicted], truthGroups.at(truthIds[truth]));
  std::vector<int> matchedTruth(truthIds.size(), -1);
  const auto augment = [&](auto&& self, int predicted,
                           std::vector<bool>& visited) -> bool {
    std::vector<int> candidates(truthIds.size());
    for (std::size_t index = 0; index < candidates.size(); ++index)
      candidates[index] = static_cast<int>(index);
    std::stable_sort(candidates.begin(), candidates.end(), [&](int first, int second) {
      return iou[predicted][first] > iou[predicted][second];
    });
    for (int truth : candidates) {
      if (visited[truth] || iou[predicted][truth] < threshold) continue;
      visited[truth] = true;
      if (matchedTruth[truth] < 0 || self(self, matchedTruth[truth], visited)) {
        matchedTruth[truth] = predicted;
        return true;
      }
    }
    return false;
  };
  for (std::size_t predicted = 0; predicted < predictedGroups.size(); ++predicted) {
    std::vector<bool> visited(truthIds.size(), false);
    augment(augment, static_cast<int>(predicted), visited);
  }
  double sumIou = 0.0;
  for (std::size_t truth = 0; truth < matchedTruth.size(); ++truth) {
    if (matchedTruth[truth] < 0) continue;
    Match match;
    match.truthId = truthIds[truth];
    match.predictedId = matchedTruth[truth] + 1;
    match.intersectionOverUnion = iou[matchedTruth[truth]][truth];
    sumIou += match.intersectionOverUnion;
    result.matches.push_back(match);
  }
  result.metrics.truePositive = static_cast<int>(result.matches.size());
  result.metrics.falsePositive = result.metrics.predictedPositive - result.metrics.truePositive;
  result.metrics.falseNegative = result.metrics.truthPositive - result.metrics.truePositive;
  result.metrics.meanIntersectionOverUnion = result.metrics.truePositive > 0
      ? sumIou / result.metrics.truePositive : 0.0;
  finalize(result.metrics);
  return result;
}

const char* featureKindName(improved::FeatureKind kind) {
  switch (kind) {
    case improved::FeatureKind::IsolatedPatch: return "isolated-patch";
    case improved::FeatureKind::SimpleChain: return "simple-chain";
    case improved::FeatureKind::ClosedLoop: return "closed-loop";
    case improved::FeatureKind::CompositeJunctionNetwork: return "composite-junction-network";
  }
  return "unknown";
}

const char* radiusBehaviorName(improved::RadiusBehavior behavior) {
  switch (behavior) {
    case improved::RadiusBehavior::InsufficientEvidence: return "insufficient-evidence";
    case improved::RadiusBehavior::Constant: return "constant";
    case improved::RadiusBehavior::SmoothVariable: return "smooth-variable-candidate";
    case improved::RadiusBehavior::Segmented: return "segmented";
    case improved::RadiusBehavior::Discontinuous: return "discontinuous";
  }
  return "unknown";
}

} // namespace

std::vector<TruthRecord> read(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("cannot open truth file: " + path.string());
  std::string line;
  if (!std::getline(stream, line)) throw std::runtime_error("empty truth file");
  const std::vector<std::string> header = parseCsvRow(line);
  std::map<std::string, std::size_t> columns;
  for (std::size_t index = 0; index < header.size(); ++index)
    columns[header[index]] = index;
  if (!columns.count("persistent_id") || !columns.count("expected_label"))
    throw std::runtime_error("truth file requires persistent_id and expected_label");
  const auto field = [&](const std::vector<std::string>& row,
                         const std::string& name) -> std::string {
    const auto column = columns.find(name);
    return column != columns.end() && column->second < row.size()
        ? row[column->second] : std::string{};
  };
  std::vector<TruthRecord> records;
  int lineNumber = 1;
  while (std::getline(stream, line)) {
    ++lineNumber;
    if (line.empty()) continue;
    const std::vector<std::string> row = parseCsvRow(line);
    TruthRecord record;
    record.persistentId = field(row, "persistent_id");
    if (record.persistentId.empty())
      throw std::runtime_error("truth row missing persistent_id at line " +
                               std::to_string(lineNumber));
    record.label = parseLabel(field(row, "expected_label"));
    record.expectedChain = field(row, "expected_chain");
    record.expectedFeature = field(row, "expected_feature");
    record.expectedFeatureKind = field(row, "expected_feature_kind");
    record.expectedRadiusBehavior = field(row, "expected_radius_behavior");
    record.notes = field(row, "notes");
    records.push_back(std::move(record));
  }
  return records;
}

Result evaluate(const improved::Result& recognition,
                const std::vector<TruthRecord>& truth,
                double minimumIntersectionOverUnion) {
  if (!(minimumIntersectionOverUnion > 0.0 && minimumIntersectionOverUnion <= 1.0))
    throw std::invalid_argument("truth matching IoU must be in (0, 1]");
  Result result;
  result.modelFaceCount = static_cast<int>(recognition.faces.size());
  result.matchingIntersectionOverUnion = minimumIntersectionOverUnion;
  std::map<std::string, const TruthRecord*> truthById;
  for (const TruthRecord& record : truth) {
    if (!truthById.emplace(record.persistentId, &record).second)
      throw std::runtime_error("duplicate truth persistent_id: " + record.persistentId);
  }
  std::map<int, std::string> persistentByFaceId;
  std::set<std::string> modelIds;
  std::map<std::string, std::set<std::string>> truthChains, truthFeatures;
  std::map<std::string, std::string> expectedKinds, expectedBehaviors;
  bool chainTruthComplete = true, featureTruthComplete = true;
  result.faces.available = true;
  for (const auto& face : recognition.faces) {
    persistentByFaceId[face.faceId] = face.persistentId;
    modelIds.insert(face.persistentId);
    const auto found = truthById.find(face.persistentId);
    const TruthRecord* record = found == truthById.end() ? nullptr : found->second;
    const TruthLabel label = record ? record->label : TruthLabel::Unknown;
    if (!record) ++result.missingTruthFaceCount;
    if (label == TruthLabel::Unknown) {
      ++result.unknownFaceCount;
      continue;
    }
    ++result.reviewedFaceCount;
    const bool predicted = face.verdict == improved::Verdict::Accepted;
    const bool positive = label == TruthLabel::Fillet;
    if (positive) {
      ++result.faces.truthPositive;
      if (record && !record->expectedChain.empty())
        truthChains[record->expectedChain].insert(face.persistentId);
      else
        chainTruthComplete = false;
      if (record && !record->expectedFeature.empty()) {
        truthFeatures[record->expectedFeature].insert(face.persistentId);
        if (!record->expectedFeatureKind.empty())
          expectedKinds[record->expectedFeature] = record->expectedFeatureKind;
        if (!record->expectedRadiusBehavior.empty())
          expectedBehaviors[record->expectedFeature] = record->expectedRadiusBehavior;
      } else
        featureTruthComplete = false;
    }
    if (predicted) ++result.faces.predictedPositive;
    if (positive && predicted) ++result.faces.truePositive;
    else if (!positive && predicted) ++result.faces.falsePositive;
    else if (positive) ++result.faces.falseNegative;
    else ++result.faces.trueNegative;
  }
  for (const auto& entry : truthById)
    if (!modelIds.count(entry.first)) ++result.extraTruthRecordCount;
  result.reviewCoverage = result.modelFaceCount > 0
      ? static_cast<double>(result.reviewedFaceCount) / result.modelFaceCount : 1.0;
  finalize(result.faces);

  const bool fullyReviewed = result.unknownFaceCount == 0 &&
                             result.missingTruthFaceCount == 0;
  if (!fullyReviewed) return result;
  if (chainTruthComplete) {
    std::vector<std::set<std::string>> predictedChains;
    for (const auto& chain : recognition.chains) {
      std::set<std::string> ids;
      for (int faceId : chain.faceIds) ids.insert(persistentByFaceId.at(faceId));
      predictedChains.push_back(std::move(ids));
    }
    GroupEvaluation chains = evaluateGroups(truthChains, predictedChains,
                                             minimumIntersectionOverUnion);
    result.chains = chains.metrics;
    result.chainMatches = std::move(chains.matches);
  }
  if (!featureTruthComplete) return result;
  std::vector<std::set<std::string>> predictedFeatures;
  for (const auto& feature : recognition.features) {
    std::set<std::string> ids;
    for (int faceId : feature.faceIds) ids.insert(persistentByFaceId.at(faceId));
    predictedFeatures.push_back(std::move(ids));
  }
  GroupEvaluation features = evaluateGroups(truthFeatures, predictedFeatures,
                                             minimumIntersectionOverUnion);
  result.features = features.metrics;
  result.featureMatches = std::move(features.matches);
  for (Match& match : result.featureMatches) {
    const auto kind = expectedKinds.find(match.truthId);
    if (kind != expectedKinds.end()) {
      ++result.matchedFeatureKindCount;
      match.kindCorrect = upper(kind->second) ==
          upper(featureKindName(recognition.features[match.predictedId - 1].kind));
      if (match.kindCorrect) ++result.correctFeatureKindCount;
    }
    const auto behavior = expectedBehaviors.find(match.truthId);
    if (behavior != expectedBehaviors.end()) {
      ++result.matchedRadiusBehaviorCount;
      match.radiusBehaviorCorrect = upper(behavior->second) == upper(
          radiusBehaviorName(recognition.features[match.predictedId - 1].radiusBehavior));
      if (match.radiusBehaviorCorrect) ++result.correctRadiusBehaviorCount;
    }
  }
  return result;
}

void write(const Result& evaluation, const std::filesystem::path& reportDirectory) {
  std::filesystem::create_directories(reportDirectory);
  std::ofstream summary(reportDirectory / "evaluation-summary.csv");
  if (!summary) throw std::runtime_error("cannot write evaluation summary");
  summary << "level,available,truth_positive,predicted_positive,true_positive,"
             "false_positive,false_negative,true_negative,precision,recall,f1,accuracy,"
             "mean_iou\n";
  const auto row = [&](const char* level, const Metrics& metrics) {
    summary << level << ',' << metrics.available << ',' << metrics.truthPositive << ','
            << metrics.predictedPositive << ',' << metrics.truePositive << ','
            << metrics.falsePositive << ',' << metrics.falseNegative << ','
            << metrics.trueNegative << ',' << metrics.precision << ',' << metrics.recall << ','
            << metrics.f1 << ',' << metrics.accuracy << ','
            << metrics.meanIntersectionOverUnion << '\n';
  };
  row("face", evaluation.faces);
  row("chain", evaluation.chains);
  row("feature", evaluation.features);
  std::ofstream coverage(reportDirectory / "evaluation-coverage.csv");
  coverage << "model_faces,reviewed_faces,unknown_faces,missing_truth_faces,extra_truth_records,"
              "review_coverage,matching_iou,matched_feature_kinds,correct_feature_kinds,"
              "matched_radius_behaviors,correct_radius_behaviors\n"
           << evaluation.modelFaceCount << ',' << evaluation.reviewedFaceCount << ','
           << evaluation.unknownFaceCount << ',' << evaluation.missingTruthFaceCount << ','
           << evaluation.extraTruthRecordCount << ',' << evaluation.reviewCoverage << ','
           << evaluation.matchingIntersectionOverUnion << ','
           << evaluation.matchedFeatureKindCount << ','
           << evaluation.correctFeatureKindCount << ','
           << evaluation.matchedRadiusBehaviorCount << ','
           << evaluation.correctRadiusBehaviorCount << '\n';
  std::ofstream matches(reportDirectory / "evaluation-matches.csv");
  matches << "level,truth_id,predicted_id,iou,kind_correct,radius_behavior_correct\n";
  for (const Match& match : evaluation.chainMatches)
    matches << "chain," << match.truthId << ',' << match.predictedId << ','
            << match.intersectionOverUnion << ",,\n";
  for (const Match& match : evaluation.featureMatches)
    matches << "feature," << match.truthId << ',' << match.predictedId << ','
            << match.intersectionOverUnion << ',' << match.kindCorrect << ','
            << match.radiusBehaviorCorrect << '\n';
}

} // namespace fillet::evaluation
