#include "compiler_macro_snapshot_provider.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

struct MacroCandidate {
  bool hasValue = false;
  bool blocked = false;
  std::string replacement;
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
  CompilerMacroSnapshotMacroKind kind =
      CompilerMacroSnapshotMacroKind::PrivateAlias;
};

struct SnapshotCollector {
  std::unordered_map<std::string, MacroCandidate> candidates;
  std::unordered_set<std::string> usedNames;
};

bool isSimpleIdentifier(const std::string &text) {
  if (text.empty())
    return false;
  const unsigned char first = static_cast<unsigned char>(text[0]);
  if (!(std::isalpha(first) || first == '_'))
    return false;
  for (size_t i = 1; i < text.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (!(std::isalnum(ch) || ch == '_'))
      return false;
  }
  return true;
}

bool tryParseStrictInteger(const std::string &text) {
  if (text.empty())
    return false;
  try {
    size_t consumed = 0;
    (void)std::stoi(text, &consumed, 0);
    return consumed == text.size();
  } catch (...) {
    return false;
  }
}

bool isFunctionLikeDefineLine(const ConditionalAstLine &line) {
  return line.tokens.size() >= 4 &&
         line.tokens[3].kind == LexToken::Kind::Punct &&
         line.tokens[3].text == "(" &&
         line.tokens[2].end == line.tokens[3].start;
}

void collectIdentifierUsesFromTokens(const std::vector<LexToken> &tokens,
                                     size_t start,
                                     std::unordered_set<std::string> &out) {
  for (size_t i = start; i < tokens.size(); ++i) {
    if (tokens[i].kind != LexToken::Kind::Identifier)
      continue;
    if (tokens[i].text == "defined")
      continue;
    out.insert(tokens[i].text);
  }
}

std::string firstIdentifierAfterDirective(const ConditionalAstLine &line) {
  for (size_t i = 2; i < line.tokens.size(); ++i) {
    if (line.tokens[i].kind == LexToken::Kind::Identifier)
      return line.tokens[i].text;
  }
  return {};
}

bool extractSingleTokenObjectDefine(
    const ConditionalAstLine &line, const std::string &sourceUri,
    CompilerMacroSnapshotMacroKind kind, CompilerMacroSnapshotMacro &out) {
  out = CompilerMacroSnapshotMacro{};
  if (!line.isDirective || line.directiveKind != ConditionalDirectiveKind::Define)
    return false;
  if (line.tokens.size() < 3 ||
      line.tokens[2].kind != LexToken::Kind::Identifier)
    return false;
  if (isFunctionLikeDefineLine(line))
    return false;
  if (line.tokens.size() != 4)
    return false;

  const std::string replacement = line.tokens[3].text;
  const bool replacementIsInteger = tryParseStrictInteger(replacement);
  const bool replacementIsIdentifier = isSimpleIdentifier(replacement);
  if (!replacementIsInteger && !replacementIsIdentifier)
    return false;

  if (kind == CompilerMacroSnapshotMacroKind::PrivateAlias &&
      replacementIsInteger) {
    return false;
  }

  out.name = line.tokens[2].text;
  out.replacement = replacement;
  out.sourceUri = sourceUri;
  out.sourceLine = line.line;
  out.sourceStart = static_cast<int>(line.tokens[2].start);
  out.sourceEnd = static_cast<int>(line.tokens[2].end);
  out.kind = kind;
  return true;
}

void recordCandidate(SnapshotCollector &collector,
                     const CompilerMacroSnapshotMacro &macro) {
  if (macro.name.empty())
    return;
  MacroCandidate &candidate = collector.candidates[macro.name];
  if (candidate.blocked)
    return;
  if (candidate.hasValue && candidate.replacement != macro.replacement) {
    candidate.blocked = true;
    return;
  }
  if (candidate.hasValue &&
      candidate.kind == CompilerMacroSnapshotMacroKind::PrivateAlias &&
      macro.kind == CompilerMacroSnapshotMacroKind::PublicDefault) {
    return;
  }
  candidate.hasValue = true;
  candidate.replacement = macro.replacement;
  candidate.sourceUri = macro.sourceUri;
  candidate.sourceLine = macro.sourceLine;
  candidate.sourceStart = macro.sourceStart;
  candidate.sourceEnd = macro.sourceEnd;
  candidate.kind = macro.kind;
}

void blockCandidate(SnapshotCollector &collector, const std::string &name) {
  if (name.empty())
    return;
  collector.candidates[name].blocked = true;
}

void clearCandidate(SnapshotCollector &collector, const std::string &name) {
  if (name.empty())
    return;
  MacroCandidate &candidate = collector.candidates[name];
  if (candidate.blocked)
    return;
  candidate.hasValue = false;
  candidate.replacement.clear();
  candidate.sourceUri.clear();
  candidate.sourceLine = -1;
  candidate.sourceStart = 0;
  candidate.sourceEnd = 0;
  candidate.kind = CompilerMacroSnapshotMacroKind::PrivateAlias;
}

bool lineDefinesName(const ConditionalAst &ast, size_t nodeIndex,
                     const std::string &name,
                     const std::string &sourceUri,
                     CompilerMacroSnapshotMacro &macroOut) {
  if (nodeIndex >= ast.nodes.size())
    return false;
  const ConditionalAstNode &node = ast.nodes[nodeIndex];
  if (node.kind != ConditionalAstNode::Kind::Line || node.line < 0 ||
      node.line >= static_cast<int>(ast.lines.size()))
    return false;
  const ConditionalAstLine &line = ast.lines[node.line];
  if (!extractSingleTokenObjectDefine(
          line, sourceUri, CompilerMacroSnapshotMacroKind::PublicDefault,
          macroOut)) {
    return false;
  }
  return macroOut.name == name;
}

void scanPublicDefaultFromIfndef(const ConditionalAst &ast,
                                 const ConditionalAstNode &node,
                                 const std::string &sourceUri,
                                 SnapshotCollector &collector) {
  if (node.branches.empty())
    return;
  const ConditionalAstBranch &first = node.branches.front();
  if (first.directiveKind != ConditionalDirectiveKind::Ifndef ||
      first.directiveLine < 0 ||
      first.directiveLine >= static_cast<int>(ast.lines.size())) {
    return;
  }
  const std::string guardName =
      firstIdentifierAfterDirective(ast.lines[first.directiveLine]);
  if (guardName.empty())
    return;

  for (size_t childIndex : first.childNodeIndices) {
    CompilerMacroSnapshotMacro macro;
    if (lineDefinesName(ast, childIndex, guardName, sourceUri, macro)) {
      recordCandidate(collector, macro);
      return;
    }
  }
}

void scanRootLine(const ConditionalAst &ast, const ConditionalAstLine &line,
                  const std::string &sourceUri, SnapshotCollector &collector) {
  if (line.directiveKind == ConditionalDirectiveKind::Define) {
    if (line.tokens.size() >= 3 &&
        line.tokens[2].kind == LexToken::Kind::Identifier) {
      CompilerMacroSnapshotMacro macro;
      if (extractSingleTokenObjectDefine(
              line, sourceUri, CompilerMacroSnapshotMacroKind::PrivateAlias,
              macro)) {
        recordCandidate(collector, macro);
      } else {
        blockCandidate(collector, line.tokens[2].text);
      }
    }
    return;
  }
  if (line.directiveKind == ConditionalDirectiveKind::Undef) {
    if (line.tokens.size() >= 3 &&
        line.tokens[2].kind == LexToken::Kind::Identifier) {
      clearCandidate(collector, line.tokens[2].text);
    }
    return;
  }

  if (line.directiveKind == ConditionalDirectiveKind::If ||
      line.directiveKind == ConditionalDirectiveKind::Ifdef ||
      line.directiveKind == ConditionalDirectiveKind::Ifndef ||
      line.directiveKind == ConditionalDirectiveKind::Elif) {
    collectIdentifierUsesFromTokens(line.tokens, 2, collector.usedNames);
  }
}

void scanSource(const CompilerMacroSnapshotSource &source,
                SnapshotCollector &collector) {
  if (!source.ast)
    return;
  const ConditionalAst &ast = *source.ast;
  for (size_t nodeIndex : ast.rootNodeIndices) {
    if (nodeIndex >= ast.nodes.size())
      continue;
    const ConditionalAstNode &node = ast.nodes[nodeIndex];
    if (node.kind == ConditionalAstNode::Kind::Line) {
      if (node.line < 0 || node.line >= static_cast<int>(ast.lines.size()))
        continue;
      scanRootLine(ast, ast.lines[node.line], source.uri, collector);
      continue;
    }
    scanPublicDefaultFromIfndef(ast, node, source.uri, collector);
  }

  for (const auto &line : ast.lines) {
    if (!line.isDirective) {
      collectIdentifierUsesFromTokens(line.tokens, 0, collector.usedNames);
      continue;
    }
    if (line.directiveKind == ConditionalDirectiveKind::If ||
        line.directiveKind == ConditionalDirectiveKind::Ifdef ||
        line.directiveKind == ConditionalDirectiveKind::Ifndef ||
        line.directiveKind == ConditionalDirectiveKind::Elif) {
      collectIdentifierUsesFromTokens(line.tokens, 2, collector.usedNames);
    }
  }
}

std::unordered_set<std::string> buildUsedClosure(
    const SnapshotCollector &collector) {
  std::unordered_set<std::string> used = collector.usedNames;
  bool changed = true;
  while (changed) {
    changed = false;
    std::vector<std::string> current(used.begin(), used.end());
    for (const auto &name : current) {
      auto it = collector.candidates.find(name);
      if (it == collector.candidates.end())
        continue;
      const MacroCandidate &candidate = it->second;
      if (!candidate.hasValue || candidate.blocked)
        continue;
      if (!isSimpleIdentifier(candidate.replacement))
        continue;
      if (used.insert(candidate.replacement).second)
        changed = true;
    }
  }
  return used;
}

} // namespace

CompilerMacroSnapshot buildCompilerMacroSnapshotFromSources(
    const std::vector<CompilerMacroSnapshotSource> &sources) {
  SnapshotCollector collector;
  for (const auto &source : sources)
    scanSource(source, collector);

  const std::unordered_set<std::string> used = buildUsedClosure(collector);
  CompilerMacroSnapshot snapshot;
  snapshot.macros.reserve(collector.candidates.size());
  for (const auto &entry : collector.candidates) {
    const MacroCandidate &candidate = entry.second;
    if (!candidate.hasValue || candidate.blocked)
      continue;
    if (used.find(entry.first) == used.end())
      continue;
    CompilerMacroSnapshotMacro macro;
    macro.name = entry.first;
    macro.replacement = candidate.replacement;
    macro.sourceUri = candidate.sourceUri;
    macro.sourceLine = candidate.sourceLine;
    macro.sourceStart = candidate.sourceStart;
    macro.sourceEnd = candidate.sourceEnd;
    macro.kind = candidate.kind;
    snapshot.macros.push_back(std::move(macro));
  }
  std::sort(snapshot.macros.begin(), snapshot.macros.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
              return lhs.sourceUri < rhs.sourceUri;
            });
  return snapshot;
}
