# Editing Runtime Layered Rewrite Handoff

> Historical note: this handoff is stale as of 2026-04-12 after Task 5 was merged to `main` in `D:\YYBWorkSpace\GitHub\nsp-intellision`.
> Do not use its old `nsp-intellision_recovered` continuation instructions as current guidance.
> Use `docs/human-ai/2026-04-12-editing-runtime-completion-audit-and-task6-plan.md` as the continuation entry instead.

Date: 2026-04-12
Active repo for continuation: `D:\YYBWorkSpace\GitHub\nsp-intellision_recovered`
Remote: `https://github.com/yang137447/nsp-intellision`
Recommended continuation branch: `codex/editing-runtime-task5`
Remote `main` HEAD at handoff: `5b4f513` (`feat: add current-doc semantic runtime`)

## Why This Handoff Exists

The original local repo at `D:\YYBWorkSpace\GitHub\nsp-intellision` had its shared `.git` metadata damaged during repeated worktree cleanup. Work continued by recovering from the remote into `D:\YYBWorkSpace\GitHub\nsp-intellision_recovered`.

Use `nsp-intellision_recovered` for all follow-up work. Do not continue from the original `nsp-intellision` directory.

## Completed So Far

### Task 1

- Status: completed and merged to remote `main`
- Outcome:
  - added layered runtime debug surface
  - exposed analysis/runtime observability fields
  - added initial layered integration test entry

### Task 2

- Status: completed and merged to remote `main`
- Outcome:
  - introduced explicit `global_context_runtime.*`
  - centralized active-unit / include / macro / branch / workspace-summary context
  - added macro/include fixtures and shared global-context identity assertions

### Task 3

- Status: completed and merged to remote `main`
- Outcome:
  - introduced explicit `local_structural_runtime.*`
  - promoted immediate syntax into explicit local-structural runtime state
  - fixed owner/publish ordering so local-structural snapshot is available/current in the VS Code integration harness
  - added changed-window local-structural integration coverage

### Task 4

- Status: completed and merged to remote `main`
- Outcome:
  - introduced explicit `current_doc_semantic_runtime.*`
  - made current-doc semantic readiness explicit runtime state
  - removed non-publishing stale-last-good answer path
  - added layered test proving deferred semantic can exist while explicit current-doc semantic readiness is still false, until completion republishes current-doc semantic state

## Current Task

### Task 5

- Status: implementation in progress, not merged
- Working branch: `codex/editing-runtime-task5`
- Goal:
  - introduce explicit `diagnostics_runtime.*`
  - rewrite diagnostics publishing around three layers:
    - `local-structural`
    - `current-doc-semantic`
    - `global-context`
  - prevent not-ready global-context diagnostics from replacing last-good truth

## Task 5 Current Code State

Uncommitted Task 5 changes currently live on `codex/editing-runtime-task5` in `nsp-intellision_recovered`.

Files currently changed for Task 5:

- `server_cpp/CMakeLists.txt`
- `server_cpp/src/diagnostics_runtime.hpp`
- `server_cpp/src/diagnostics_runtime.cpp`
- `server_cpp/src/app/main.cpp`
- `server_cpp/src/document_runtime.hpp`
- `server_cpp/src/document_runtime.cpp`
- `src/test/suite/integration/diagnostics.ts`
- `src/test/suite/integration/editing-runtime-layered.ts`

There is also a temporary `.vscode/settings.json` local diff from test-binary overrides. It should not be committed.

## Task 5 What Is Already Good

- `client.diagnostics` filtered suite passes against the recovered repo build
- new macro/global-context continuity tests exist
- `diagnostics_runtime.*` boundary exists
- publish-layer policy is no longer only ad hoc code inside `main.cpp`

## Task 5 Current Blocker

Only one known blocker remains before Task 5 can be reviewed/merged:

- `client.editing-runtime-layered` still fails on:
  - `marks local structural snapshot ready after changed-window structural analysis`

Latest observed failure:

```text
AssertionError [ERR_ASSERTION]: Expected to observe local-structural diagnostics publish after changed-window edit. Last diagnostics:
3:Missing semicolon.
4:Missing semicolon.
```

Meaning:

- the changed-window edit definitely publishes the expected diagnostics result
- but the test still fails to reliably observe the runtime debug state at the exact `local-structural` publish moment
- this is now a test-observation/timing issue, not the main Task 5 diagnostics-runtime architecture problem

## Latest Verified Task 5 Commands

Run in `D:\YYBWorkSpace\GitHub\nsp-intellision_recovered` with temporary workspace override:

- `nsf.serverPath = d:\YYBWorkSpace\GitHub\nsp-intellision_recovered\server_cpp\build\nsf_lsp.exe`
- `nsf.intellisionPath = [d:\YYBWorkSpace\GitHub\nsp-intellision_recovered\test_files]`

Results:

- `npm run compile`
  - pass
- `cmake --build .\server_cpp\build`
  - pass
- `NSF_TEST_FILE_FILTER=client.diagnostics`
  - `npm run test:client:repo`
  - pass (`58 passing`)
- `NSF_TEST_FILE_FILTER=client.editing-runtime-layered`
  - `npm run test:client:repo`
  - fail on exactly one test:
    - `marks local structural snapshot ready after changed-window structural analysis`

## Most Important Conclusion For The Next Thread

Do not re-open Task 1-4. The next thread should continue directly from Task 5 in `nsp-intellision_recovered` on branch `codex/editing-runtime-task5`.

The main question to solve next is narrow:

- how to make the changed-window local-structural layered test observe the `local-structural` publish authority reliably, without weakening the assertion

The most likely good direction is to keep Task 5’s new macro/global-context continuity tests, and tighten only the observation strategy for the changed-window local-structural test, rather than changing the diagnostics runtime architecture again.

## Suggested Next Steps

1. Open `D:\YYBWorkSpace\GitHub\nsp-intellision_recovered`
2. Checkout `codex/editing-runtime-task5`
3. Inspect current uncommitted diff
4. Stabilize the one failing changed-window local-structural layered test
5. Re-run:
   - `npm run compile`
   - `cmake --build .\server_cpp\build`
   - `NSF_TEST_FILE_FILTER=client.diagnostics`
   - `npm run test:client:repo`
   - `NSF_TEST_FILE_FILTER=client.editing-runtime-layered`
   - `npm run test:client:repo`
6. If green:
   - run spec review
   - run code quality review
   - commit Task 5
   - push branch
   - merge to `main`
7. Then continue with Task 6 and Task 7

## Notes

- No fact-doc updates have been required through Task 4.
- The original repo `D:\YYBWorkSpace\GitHub\nsp-intellision` should be treated as broken/inactive for continuation.
- The recovered repo `D:\YYBWorkSpace\GitHub\nsp-intellision_recovered` is the live continuation point.
