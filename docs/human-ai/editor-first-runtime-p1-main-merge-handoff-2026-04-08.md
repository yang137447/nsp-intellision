# Editor-First Runtime P1 Main Merge Handoff

Status: in progress. This is a collaboration/handoff note, not a fact document.

## Why this file exists

This handoff captures the current attempt to merge `codex/editor-first-runtime-p1` into the main workspace after stabilizing the local repo/client/server test chain.

Use this file as the entry point for a new thread.

## Current repo state

- Workspace: `D:\YYBWorkSpace\GitHub\nsp-intellision`
- Current branch: `main`
- Current `HEAD`: `1eca175 fix: restore client fallback and deferred test plumbing`
- Merge target currently in progress: `3fb5ae1 fix: restore client metrics and runtime config guards`
- `MERGE_HEAD` is present
- `main` is ahead of `origin/main` by local commits and is not currently clean

## What was completed before the merge attempt

The current main workspace had been stabilized before starting the merge attempt.

Verified passing before the merge attempt:

- `cmake --build .\server_cpp\build`
- `py -3 .\server_cpp\tools\hover_smoke_test.py`
- `npm run test:client:repo`
- `npm run test:client:all`
- `npm run test:client:perf`
- `npm run test:client:real:perf`

Main local fixes already present on `main` before the merge attempt:

- restored client-side fallback for identifier suggest / parameter hints in `client/src/client_editor_events.ts`
- restored/added client internal test commands in `client/src/client_internal_commands.ts`
- restored include underline support in `client/src/include_underline_support.ts`
- deferred-doc range cache + inlay metrics/test-plumbing fixes in:
  - `server_cpp/src/deferred_doc_runtime.*`
  - `server_cpp/src/document_runtime.*`
  - `server_cpp/src/inlay_hints_runtime.cpp`
  - `src/test/suite/integration/deferred-doc.ts`
- auto-trigger test stabilization in:
  - `src/test/suite/client.completion-auto-trigger.test.ts`
  - `src/test/suite/client.signature-help-auto-trigger.test.ts`
- added stable deferred-doc fixture:
  - `test_files/module_semantic_tokens_fresh.nsf`

## What branch is being merged

The merge source is `codex/editor-first-runtime-p1`, which implements the plan:

- `docs/superpowers/plans/2026-04-07-editor-first-runtime-p1-visibility.md`

Supporting context documents:

- `docs/human-ai/editor-first-runtime-p1-progress.md`
- `docs/superpowers/specs/2026-04-07-editor-first-runtime-upgrade-design.md`

High-level capabilities from that branch that are not yet integrated into `main`:

- interactive runtime debug surface
- `InteractiveVisibilityKey` and visibility fingerprint plumbing
- `interactive_visibility_runtime.*`
- shared-visible symbol shards
- shared-visible completion merge order
- shared-visible support for member completion / hover / signature help
- related fixtures and integration coverage for `interactive-visibility`

## Merge status right now

Files already merged/staged cleanly:

- `docs/architecture.md`
- `docs/testing.md`
- `server_cpp/CMakeLists.txt`
- `server_cpp/src/app/main.cpp`
- `server_cpp/src/document_owner.cpp`
- `server_cpp/src/document_owner.hpp`
- `server_cpp/src/document_runtime.cpp`
- `server_cpp/src/document_runtime.hpp`
- `server_cpp/src/interactive_visibility_runtime.cpp`
- `server_cpp/src/interactive_visibility_runtime.hpp`
- `server_cpp/src/requests/server_request_handler_definition.cpp`
- `server_cpp/src/requests/server_request_handler_hover.cpp`
- `server_cpp/src/workspace/workspace_index.cpp`
- `server_cpp/src/workspace/workspace_index_extract.cpp`
- `server_cpp/src/workspace/workspace_index_scan.cpp`
- `server_cpp/src/workspace_index.hpp`
- `server_cpp/src/workspace_index_owner.hpp`
- `server_cpp/src/workspace_summary_runtime.cpp`
- `server_cpp/src/workspace_summary_runtime.hpp`
- `src/test/suite/integration/runtime-config.ts`
- `test_files/visibility_globals_root.nsf`
- `test_files/visibility_member_root.nsf`
- `test_files/visibility_member_types.hlsli`
- `test_files/visibility_root.nsf`
- `test_files/visibility_shared.hlsli`

Files with unresolved conflicts:

- `client/src/client_editor_feedback.ts`
- `client/src/client_internal_commands.ts`
- `client/src/extension.ts`
- `server_cpp/src/interactive_semantic_runtime.cpp`
- `server_cpp/src/interactive_semantic_runtime.hpp`
- `src/test/suite/integration/interactive-visibility.ts`
- `src/test/suite/test_helpers.ts`

## Most likely conflict resolution strategy

Resolve conflicts by keeping both sides where they are additive:

- keep current-main client test/fallback plumbing:
  - identifier suggest fallback
  - parameter hints fallback
  - `_sendServerRequest`
  - `_invalidateInlayHintsForTests`
  - include underline support plumbing
- keep branch-side shared-visible / interactive-visibility plumbing:
  - `nsf._getInteractiveRuntimeDebug`
  - visibility fingerprint fields
  - shared-visible query order additions
  - `interactive-visibility` tests and helpers

In practice, expect the conflict resolution theme to be:

- `client/src/client_internal_commands.ts`
  - preserve all current-main test-only commands
  - preserve branch-side interactive runtime debug command/types
- `client/src/extension.ts`
  - preserve current-main client telemetry/fallback/test-command wiring
  - preserve branch-side interactive runtime debug support
- `client/src/client_editor_feedback.ts`
  - preserve current-main include underline + inlay metrics + fallback behavior
  - reconcile with any branch-side older version carefully
- `server_cpp/src/interactive_semantic_runtime.*`
  - preserve current-main duplicate-declaration cleanup and any fallback-adjacent fixes
  - merge in branch-side shared-visible visibility logic
- `src/test/suite/test_helpers.ts`
  - preserve current-main helper additions for deferred-doc / current fixes
  - merge in branch-side `getInteractiveRuntimeDebug(...)` and visibility-related helper typing
- `src/test/suite/integration/interactive-visibility.ts`
  - likely prefer branch-side content, then adjust imports/helper names to current-main helper layout

## Important caution

Do not assume the currently passing local-main fixes are present in the branch version.

The branch progress note explicitly says that some later local fixes were done on the worktree after the original P1 tasks:

- include underline module restore
- completion debug command restore
- client telemetry status restore
- runtime-config expectation alignment

Those fixes must not be lost during conflict resolution.

## Recommended next steps in a new thread

1. Open this file first
2. Inspect the seven unresolved files listed above
3. Resolve merge conflicts by combining:
   - current-main stabilization/test plumbing
   - branch-side shared-visible implementation
4. `git add` the resolved files
5. Run:
   - `npm run compile`
   - `cmake --build .\server_cpp\build`
   - `npm run test:client:repo`
   - `npm run test:client:all`
   - `npm run test:client:perf`
   - `npm run test:client:real:perf`
6. Only after green verification, decide whether to commit the merge

## Minimal context for continuation

- current main had been locally stabilized and fully green before this merge attempt
- the repo is now inside an unfinished merge from `codex/editor-first-runtime-p1`
- the main task for the next thread is conflict resolution, not fresh feature design
- the primary risk is accidentally dropping current-main client fallback/test plumbing while taking branch-side shared-visible code
