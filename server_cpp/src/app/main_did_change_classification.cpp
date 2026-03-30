#include "main_did_change_classification.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "active_unit.hpp"
#include "crash_handler.hpp"
#include "definition_location.hpp"
#include "declaration_query.hpp"
#include "deferred_doc_runtime.hpp"
#include "diagnostics.hpp"
#include "document_owner.hpp"
#include "document_runtime.hpp"
#include "expanded_source.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_docs.hpp"
#include "include_resolver.hpp"
#include "immediate_syntax_diagnostics.hpp"
#include "indeterminate_reasons.hpp"
#include "interactive_semantic_runtime.hpp"
#include "json.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "nsf_lexer.hpp"
#include "preprocessor_view.hpp"
#include "semantic_snapshot.hpp"
#include "semantic_cache.hpp"
#include "server_documents.hpp"
#include "server_occurrences.hpp"
#include "server_parse.hpp"
#include "server_request_handlers.hpp"
#include "server_settings.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"
#include "workspace_summary_runtime.hpp"


static bool lineLooksSemanticNeutralForDidChange(const std::string &lineText) {
  const std::string trimmed = trimRightCopy(trimLeftCopy(lineText));
  if (trimmed.empty())
    return true;
  return trimmed.rfind("//", 0) == 0 || trimmed.rfind("/*", 0) == 0 ||
         trimmed.rfind("*", 0) == 0 || trimmed.rfind("*/", 0) == 0;
}

bool isCommentOnlyEditForDidChange(
    const std::string &text, const std::vector<ChangedRange> &changedRanges) {
  if (changedRanges.empty() || changedRanges.size() > 8)
    return false;
  int inspectedLines = 0;
  for (const auto &range : changedRanges) {
    const int startLine = std::max(0, range.startLine);
    const int endLine = std::max(startLine, range.startLine + range.newEndLine);
    inspectedLines += (endLine - startLine + 1);
    if (inspectedLines > 24)
      return false;
    for (int line = startLine; line <= endLine; line++) {
      if (!lineLooksSemanticNeutralForDidChange(getLineAt(text, line)))
        return false;
    }
  }
  return inspectedLines > 0;
}
