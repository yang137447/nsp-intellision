# Editing Runtime Master Plan (Reconstructed)

> Status: reconstruction document for audit and continuation.
> This file is a human-AI collaboration artifact, not a current fact document.
> Current facts remain in `README.md`, `docs/architecture.md`, `docs/resources.md`, and `docs/testing.md`.

## 1. Why This Document Exists

Two earlier draft files were referenced during planning but are no longer present in the current repository working tree:

- `docs/superpowers/specs/2026-04-09-editing-runtime-layered-rewrite-design.md`
- `docs/superpowers/plans/2026-04-09-editing-runtime-layered-rewrite.md`

Those files also do not appear in current reachable git history, so they were almost certainly untracked draft files that were never committed.

The runtime work itself did not disappear. What was lost is the single, audit-friendly "master spec + implementation plan" view.

This reconstruction consolidates the currently available sources into one place so future review can audit the work without depending on stale handoffs or missing scratch docs.

## 2. Source Documents Used For Reconstruction

Current continuation sources:

- `docs/human-ai/realtime-feedback-design.md`
- `docs/human-ai/2026-04-12-editing-runtime-layered-rewrite-handoff.md`
- `docs/human-ai/2026-04-12-editing-runtime-completion-audit-and-task6-plan.md`
- `docs/architecture.md`
- `docs/testing.md`
- current repository state on `main`

Important note:

- `docs/human-ai/2026-04-12-editing-runtime-layered-rewrite-handoff.md` explicitly marks itself stale after Task 5 merged to `main`.
- `docs/human-ai/2026-04-12-editing-runtime-completion-audit-and-task6-plan.md` is the current continuation entry.

## 3. Original Intent Of The Runtime Rewrite

The editing runtime rewrite was not meant to be a sequence of local cache patches.

Its architectural goal was to replace the old "fast vs full" mental model with a layered editing-time runtime where:

1. local structural truth
2. current-document semantic truth
3. global-context semantic truth
4. deferred artifacts

are treated as separate correctness layers rather than just separate speeds.

The motivating problems were:

- tiny local edits causing broad diagnostics churn
- macros temporarily reported as undefined and then self-healing
- current-document hot paths being polluted by late global context arrival
- deferred artifact timing visually destabilizing editing feedback

## 4. Target Architecture Summary

The intended architecture can be summarized as:

### 4.1 Runtime layers

- `Editor Shell`
- `Local Structural Runtime`
- `Current-Doc Semantic Runtime`
- `Global Context Runtime`
- `Deferred Artifacts Runtime`

### 4.2 Snapshot model

- `DocumentState`
- `LocalStructuralSnapshot`
- `CurrentDocSemanticSnapshot`
- `GlobalContextSnapshot`
- `DeferredArtifacts`

### 4.3 Diagnostics model

Diagnostics were intended to be classified into three publish layers:

- `LocalStructuralDiagnostics`
- `CurrentDocSemanticDiagnostics`
- `GlobalContextDiagnostics`

Core rule:

- global-context-sensitive diagnostics must not replace last-good truth until global context is ready for the current document state

### 4.4 Query order intent

For completion / hover / signature help / member completion / current-doc short definition, the intended order was:

1. current-doc semantic
2. last-good current-doc semantic
3. global-context enrichment / shared-visible context
4. deferred
5. workspace summary fallback

## 5. Reconstructed Task List

This is the most likely original Task 1-7 decomposition, reconstructed from the surviving spec/audit/handoff trail and current repository state.

### Task 1. Layer And Context Observability

Intent:

- expose runtime-layer debug state
- make layer ownership and readiness inspectable in tests

Typical deliverables:

- richer `nsf._getDocumentRuntimeDebug`
- integration test entry for layered-runtime assertions

Current status:

- effectively complete and merged before Task 5 audit

Evidence:

- current runtime debug surface exists
- layered integration tests/harness references exist in audit trail

### Task 2. Global Context Runtime

Intent:

- isolate active unit / include closure / branch / macro / workspace-summary state into an explicit runtime layer

Current status:

- complete and merged

Evidence from stale handoff:

- Task 2 introduced explicit `global_context_runtime.*`
- centralized active-unit / include / macro / branch / workspace-summary context

### Task 3. Local Structural Runtime

Intent:

- promote immediate syntax into an explicit local-structural runtime
- make changed-window structural truth a first-class layer

Current status:

- complete and merged

Evidence from stale handoff:

- Task 3 introduced explicit `local_structural_runtime.*`
- owner/publish ordering for local-structural snapshot was fixed

### Task 4. Current-Doc Semantic Runtime

Intent:

- make current-document semantic readiness explicit
- stop treating deferred/global paths as the implicit primary semantic truth for hot-path editing

Current status:

- complete and merged

Evidence from stale handoff:

- Task 4 introduced explicit `current_doc_semantic_runtime.*`
- current-doc semantic readiness became explicit runtime state

### Task 5. Diagnostics Runtime Layering

Intent:

- introduce explicit `diagnostics_runtime.*`
- rewrite diagnostics publishing around:
  - `local-structural`
  - `current-doc-semantic`
  - `global-context`

Current status:

- complete and merged to `main`

Evidence:

- `docs/human-ai/2026-04-12-editing-runtime-completion-audit-and-task6-plan.md`
  states Task 5 is merged as `a547558`

### Task 6. Completion Audit And Fact Reconciliation

Intent:

- audit whether the editor-first runtime is complete enough in practice
- verify P2 / P3 correctness subsets
- reconcile stale handoff docs and current facts

Current status:

- defined in the audit doc as the next step after Task 5
- partial audit results already recorded in the same file

### Task 7. Release Hardening And Final Fact Freeze

Intent:

- run release gate
- decide whether facts docs must still change
- determine whether remaining hot-path fallback risks are acceptable or require more implementation

Current status:

- not yet completed in the surviving documentation trail

## 6. Current Completion State

Based on the surviving documents, the current best-effort reconstruction is:

### Confirmed complete

- Task 1
- Task 2
- Task 3
- Task 4
- Task 5

### Defined but not fully frozen

- Task 6
- Task 7

### Already verified in the surviving audit record

From `docs/human-ai/2026-04-12-editing-runtime-completion-audit-and-task6-plan.md`:

- `npm run test:client:repo` passed on `main`
- `client.deferred-doc-runtime` filtered suite passed
- `client.deferred-visual-continuity` filtered suite passed
- `client.workspace-summary` filtered suite passed
- `client.references-rename` filtered suite passed
- `client.analysis-context-workspace` filtered suite passed
- `npm run test:client:perf` passed with `26 passing`, `3 pending`

## 7. Current Known Risk

The main risk explicitly called out by the audit is:

- `interactive_semantic_runtime.cpp` still contains a member-base fallback path that reads active include text and scans declarations through include-closure state, with a debug resolution path of `include_closure_decl`

Why this matters:

- it may still violate the intended "no hidden hot-path fallback" architecture direction
- even if current tests pass, the runtime may not yet be considered architecturally clean/frozen

The audit recommended that before final freeze, the project should decide whether this path:

1. is acceptable as a bounded documented exception, or
2. should be removed by pushing the remaining cross-file member-base fact behind the shared-visible / workspace-summary boundary

## 8. Audit Checklist For Future Review

If someone wants to audit the full editing-runtime rewrite now, the minimal checklist is:

### 8.1 Architecture audit

- confirm `global_context_runtime.*` exists and is the sole producer of active-unit/include/macro semantic prerequisites
- confirm `local_structural_runtime.*` exists and owns changed-window-first structural truth
- confirm `current_doc_semantic_runtime.*` exists and current-doc semantic readiness is explicit
- confirm `diagnostics_runtime.*` exists and diagnostics publish authority is layered
- confirm deferred artifact code is not acting as hidden hot-path truth

### 8.2 Behavior audit

- local edits should not broad-churn unrelated diagnostics
- macros in active include/active unit context should not transiently degrade into broad `Undefined identifier` noise
- completion / hover / signature help should prefer current-doc or current-context-visible truth over workspace-global fallback
- deferred artifacts should not clobber additive same-version writes

### 8.3 Verification audit

Recommended commands:

```powershell
npm run test:client:repo

$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER

$env:NSF_TEST_FILE_FILTER='client.deferred-visual-continuity'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER

$env:NSF_TEST_FILE_FILTER='client.workspace-summary'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER

$env:NSF_TEST_FILE_FILTER='client.references-rename'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER

$env:NSF_TEST_FILE_FILTER='client.analysis-context-workspace'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER

npm run test:client:perf
```

Release freeze commands:

```powershell
npm run gate:d3
npm run package:vsix
```

## 9. How To Continue From Here

Do not continue from the stale Task 5 handoff.

Use this order instead:

1. `docs/human-ai/2026-04-12-editing-runtime-completion-audit-and-task6-plan.md`
2. this reconstructed master plan
3. current `main` source tree

Decision tree:

- if the remaining `include_closure_decl` fallback is acceptable:
  - update facts docs if needed
  - finish Task 7 release hardening

- if that fallback is not acceptable:
  - write a new small follow-up implementation plan scoped only to removing that hot-path exception
  - do not reopen Tasks 1-5 wholesale

## 10. What Was Lost And What Was Recovered

Lost:

- the original 2026-04-09 master spec file
- the original 2026-04-09 master implementation plan file
- the exact wording of the original task decomposition drafts

Recovered:

- the architectural intent
- the 1-7 task structure
- the completion state through Task 5
- the current continuation entry for Task 6/7
- the current known residual risk
- the recommended audit/verification path

## 11. Recommended Future Document Hygiene

To avoid repeating this:

1. never leave the only master spec/plan as untracked files
2. whenever a handoff supersedes an earlier plan, add a stable “master continuation” doc instead of only a task-local note
3. if a task sequence spans multiple threads, keep one canonical document with:
   - architecture summary
   - task table
   - current status
   - next entry point

## 12. Executive Summary

The editing runtime rewrite does not appear to have lost its implementation progress.

What was lost was the clean, single-file planning view.

As of the surviving 2026-04-12 audit:

- Tasks 1-5 are effectively done
- Task 6 is the audit/reconciliation phase
- Task 7 is release hardening/fact freeze
- the main unresolved concern is whether a residual member-base include-closure fallback is acceptable or must still be removed
