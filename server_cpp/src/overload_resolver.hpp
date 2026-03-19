#pragma once

#include "type_desc.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct ParamDesc {
  std::string name;
  TypeDesc type;
};

struct CandidateSignature {
  std::string name;
  std::string displayLabel;
  std::vector<std::string> displayParams;
  std::vector<ParamDesc> params;
  TypeDesc returnType;
  std::string visibilityCondition;
  std::string sourceUri;
  int sourceLine = 0;
};

struct CandidateScore {
  int candidateIndex = -1;
  int totalCost = 0;
  std::vector<int> perArgCost;
  bool viable = false;
  std::string tieBreakReason;
  std::string rejectReason;
};

enum class ResolveCallStatus { Resolved, Ambiguous, NoViable };

struct ResolveCallResult {
  ResolveCallStatus status = ResolveCallStatus::NoViable;
  int bestCandidateIndex = -1;
  std::vector<CandidateScore> rankedCandidates;
  std::string reasonCode;
};

struct ResolveCallContext {
  std::unordered_map<std::string, int> defines;
  bool allowNarrowing = false;
  bool enableVisibilityFiltering = true;
  bool allowPartialArity = false;
};

ResolveCallResult
resolveCallCandidates(const std::vector<CandidateSignature> &candidates,
                      const std::vector<TypeDesc> &argumentTypes,
                      const ResolveCallContext &context = ResolveCallContext{});

const char *resolveCallStatusToString(ResolveCallStatus status);
