#include "overload_resolver.hpp"

#include "type_relation.hpp"
#include "visibility_evaluator.hpp"

#include <algorithm>
#include <limits>

namespace {
CandidateScore scoreCandidate(const CandidateSignature &candidate,
                              int candidateIndex,
                              const std::vector<TypeDesc> &argumentTypes,
                              const ResolveCallContext &context) {
  CandidateScore score;
  score.candidateIndex = candidateIndex;
  if (context.enableVisibilityFiltering &&
      !candidate.visibilityCondition.empty()) {
    VisibilityEvalResult visibility = evaluateVisibilityCondition(
        candidate.visibilityCondition, context.defines);
    if (visibility == VisibilityEvalResult::Hidden) {
      score.rejectReason = "visibility_hidden";
      return score;
    }
    if (visibility == VisibilityEvalResult::Unknown) {
      score.totalCost += 2;
      score.tieBreakReason = "visibility_unknown";
    }
  }
  if ((!context.allowPartialArity &&
       candidate.params.size() != argumentTypes.size()) ||
      (context.allowPartialArity &&
       argumentTypes.size() > candidate.params.size())) {
    score.rejectReason = "arg_count_mismatch";
    return score;
  }

  score.perArgCost.reserve(argumentTypes.size());
  int total = score.totalCost;
  for (size_t i = 0; i < argumentTypes.size(); i++) {
    if (argumentTypes[i].kind == TypeDescKind::Unknown) {
      const int unknownCost = 4;
      score.perArgCost.push_back(unknownCost);
      if (total >= std::numeric_limits<int>::max() - unknownCost)
        total = std::numeric_limits<int>::max();
      else
        total += unknownCost;
      continue;
    }
    TypeRelationResult relation = evaluateTypeRelation(
        candidate.params[i].type, argumentTypes[i], context.allowNarrowing);
    score.perArgCost.push_back(relation.cost);
    if (!relation.viable) {
      score.rejectReason =
          std::string("arg_type_") + typeRelationKindToString(relation.kind);
      return score;
    }
    if (total >= std::numeric_limits<int>::max() - relation.cost)
      total = std::numeric_limits<int>::max();
    else
      total += relation.cost;
  }

  score.totalCost = total;
  score.viable = true;
  return score;
}
} // namespace

ResolveCallResult
resolveCallCandidates(const std::vector<CandidateSignature> &candidates,
                      const std::vector<TypeDesc> &argumentTypes,
                      const ResolveCallContext &context) {
  ResolveCallResult result;
  if (candidates.empty()) {
    result.status = ResolveCallStatus::NoViable;
    result.reasonCode = "no_candidates";
    return result;
  }

  result.rankedCandidates.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); i++) {
    result.rankedCandidates.push_back(scoreCandidate(
        candidates[i], static_cast<int>(i), argumentTypes, context));
  }

  std::vector<CandidateScore> viable;
  viable.reserve(result.rankedCandidates.size());
  for (const CandidateScore &score : result.rankedCandidates) {
    if (score.viable)
      viable.push_back(score);
  }

  if (viable.empty()) {
    result.status = ResolveCallStatus::NoViable;
    result.reasonCode = "no_viable_candidate";
    return result;
  }

  std::sort(viable.begin(), viable.end(),
            [](const CandidateScore &a, const CandidateScore &b) {
              if (a.totalCost != b.totalCost)
                return a.totalCost < b.totalCost;
              return a.candidateIndex < b.candidateIndex;
            });

  result.rankedCandidates = viable;
  result.bestCandidateIndex = viable.front().candidateIndex;

  if (viable.size() > 1 && viable[0].totalCost == viable[1].totalCost) {
    result.status = ResolveCallStatus::Ambiguous;
    result.reasonCode = "ambiguous_overload";
    result.rankedCandidates[0].tieBreakReason = "same_total_cost";
    result.rankedCandidates[1].tieBreakReason = "same_total_cost";
    return result;
  }

  result.status = ResolveCallStatus::Resolved;
  result.reasonCode = "resolved";
  return result;
}

const char *resolveCallStatusToString(ResolveCallStatus status) {
  switch (status) {
  case ResolveCallStatus::Resolved:
    return "resolved";
  case ResolveCallStatus::Ambiguous:
    return "ambiguous";
  case ResolveCallStatus::NoViable:
    return "no_viable";
  }
  return "no_viable";
}
