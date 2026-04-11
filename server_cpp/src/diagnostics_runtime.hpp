#pragma once

#include "document_runtime.hpp"
#include "json.hpp"
#include "local_structural_runtime.hpp"

#include <string>

// Diagnostics publish-policy boundary for editing-time layered runtime.
//
// Responsibilities:
// - decide which diagnostics payload is allowed to replace the currently
//   visible document diagnostics for a given build result
// - make publish authority explicit as one of:
//   `local-structural`, `current-doc-semantic`, `global-context`
// - preserve last-good global-context diagnostics while the new global context
//   is not yet ready
//
// Non-goals:
// - this module does not build diagnostics rules itself
// - it does not schedule background jobs or mutate document runtime state

enum class DiagnosticsPublishLayer {
  None,
  LocalStructural,
  CurrentDocSemantic,
  GlobalContext,
};

struct DiagnosticsPublishDecision {
  Json diagnostics;
  DiagnosticsPublishLayer layer = DiagnosticsPublishLayer::None;
};

// Returns the stable debug/publish string for a diagnostics layer.
std::string
diagnosticsRuntimePublishLayerName(DiagnosticsPublishLayer layer);

// Decides the visible diagnostics payload for the local-structural layer.
//
// For changed-window structural publishes, this preserves last-good
// same-context diagnostics outside the changed window so whole-document LSP
// replacement does not transiently clear unrelated semantic diagnostics.
DiagnosticsPublishDecision diagnosticsRuntimeBuildLocalStructuralPublish(
    const DocumentRuntime *runtimeBeforeBuild,
    const LocalStructuralSnapshot &localStructuralSnapshot);

// Decides the visible diagnostics payload for a full semantic build.
//
// When the current global context is ready, the new semantic result owns the
// visible diagnostics as `global-context`. While global context for the new edit
// is not ready, callers keep publishing through `current-doc-semantic` and
// preserve the last-good full diagnostics truth from the pre-build runtime.
DiagnosticsPublishDecision diagnosticsRuntimeBuildSemanticPublish(
    const DocumentRuntime &runtimeAfterBuild,
    const DocumentRuntime *runtimeBeforeBuild,
    const Json &builtDiagnostics);
