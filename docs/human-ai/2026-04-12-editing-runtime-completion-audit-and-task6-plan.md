# Editing Runtime Completion Audit And Task 6/7 Plan

Date: 2026-04-12
Repo: `D:\YYBWorkSpace\GitHub\nsp-intellision`
Current baseline: `main` at `a547558` (`feat: add layered diagnostics runtime`)

This document is a human-AI collaboration note, not a current fact document. Current facts remain in `README.md`, `docs/architecture.md`, `docs/resources.md`, and `docs/testing.md`.

## Why This Exists

The previous handoff only defined Task 5 and mentioned Task 6/7 without specifying their goals. Task 5 has now been merged to `main`, so the remaining work is no longer implementation continuation from that handoff. The next useful step is to audit completion against the editor-first runtime design and turn the remaining undefined Task 6/7 slots into explicit review gates.

## Current Completion Status

Confirmed complete:

- Task 1-4 are already merged to `main` before Task 5.
- Task 5 is merged to `main` as `a547558`.
- `diagnostics_runtime.*` exists and is part of the server build.
- The changed-window local-structural integration blocker has been resolved.
- Fresh verification on current `main`:
  - `npm run test:client:repo` passed on 2026-04-12.

Not yet claimable as complete:

- The whole editor-first runtime upgrade cannot be marked architecturally complete yet.
- The tracked P1 plan explicitly left `P2 DeferredDoc`, `P3 Workspace`, and final performance/release hardening as follow-up plans.
- The old handoff at `docs/human-ai/2026-04-12-editing-runtime-layered-rewrite-handoff.md` is now stale because it still says to continue in `nsp-intellision_recovered` and describes Task 5 as in-progress.

## Task 6: Completion Audit And Fact Reconciliation

Goal:

- Establish a reliable, evidence-backed completion state after Task 5.
- Reconcile stale collaboration docs with current `main`.
- Decide whether remaining P2/P3 work is already factually complete or needs new implementation plans.

Files:

- Read: `README.md`
- Read: `docs/architecture.md`
- Read: `docs/testing.md`
- Read: `docs/superpowers/specs/2026-04-07-editor-first-runtime-upgrade-design.md`
- Read: `docs/superpowers/plans/2026-04-07-editor-first-runtime-p1-visibility.md`
- Read/update: `docs/human-ai/2026-04-12-editing-runtime-layered-rewrite-handoff.md`
- Create/update: `docs/human-ai/2026-04-12-editing-runtime-completion-audit-and-task6-plan.md`

Steps:

1. Confirm repository state.

```powershell
git status --short --branch
git log --oneline --decorate -8 main
```

Expected:

- `main` is at or after `a547558`.
- No source changes are pending unless they are audit-doc changes.

2. Verify the default repo regression on current `main`.

```powershell
npm run test:client:repo
```

Expected:

- All repo-mode integration suites pass.

3. Verify the P2 DeferredDoc correctness subset.

```powershell
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER

$env:NSF_TEST_FILE_FILTER='client.deferred-visual-continuity'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected:

- Deferred semantic tokens, inlay hints, document symbols, full diagnostics, range-cache continuity, and visual continuity suites pass.

4. Verify the P3 Workspace correctness subset.

```powershell
$env:NSF_TEST_FILE_FILTER='client.workspace-summary'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER

$env:NSF_TEST_FILE_FILTER='client.references-rename'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER

$env:NSF_TEST_FILE_FILTER='client.analysis-context-workspace'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected:

- Workspace summary fallback, reverse include refresh, references, prepareRename, rename, and workspace-summary-driven analysis context suites pass.

5. Verify performance/hardening gates for the editor-first runtime path.

```powershell
npm run test:client:perf
```

Expected:

- Perf suite passes and writes reports under `out/test/perf-reports/`.
- M4 deferred-doc and M5 workspace-summary/reverse-include scenarios pass.

6. Audit hidden hot-path fallback risk.

```powershell
git grep -n "include graph\|includeGraph\|scan\|workspace summary\|workspaceSummary" -- server_cpp/src/requests server_cpp/src/interactive_semantic_runtime.cpp server_cpp/src/interactive_visibility_runtime.cpp server_cpp/src/workspace_summary_runtime.cpp
```

Expected:

- Interactive request handlers do not reintroduce direct include-graph scans or full-workspace hot-path computation.
- Any workspace summary use in interactive handlers is an explicit lower-priority miss path documented in `docs/architecture.md`.

7. Reconcile stale collaboration docs.

Options:

- Rewrite `docs/human-ai/2026-04-12-editing-runtime-layered-rewrite-handoff.md` as historical-only, with a top note saying Task 5 is complete and the original repo path is active again.
- Or leave it untracked and do not rely on it for future work.

Recommended:

- Keep this audit plan as the continuation entry.
- Do not commit the stale Task 5 handoff unless it is corrected first.

Task 6 completion criteria:

- Current `main` passes repo regression.
- P2/P3 filtered suites pass.
- Perf suite passes or any failures are classified with concrete follow-up.
- Stale handoff has either been corrected or explicitly excluded from future continuation.
- Final audit summary states which of `M3`, `M4`, and `M5` can be considered complete and which still need implementation.

## Task 7: Release Hardening And Final Fact Freeze

Goal:

- Convert the audit result into a final release gate.
- Update current fact docs only if the audit proves behavior/architecture facts have changed since the last documentation pass.

Files:

- Check/update: `README.md`
- Check/update: `docs/architecture.md`
- Check/update: `docs/testing.md`
- Check/update: `docs/human-ai/2026-04-12-editing-runtime-completion-audit-and-task6-plan.md`

Steps:

1. Run the full release gate.

```powershell
npm run gate:d3
```

Expected:

- Resource validation passes.
- TypeScript compile passes.
- C++ server build passes.
- Hover smoke test passes.
- `npm run test:client:all` passes.

2. Run packaging validation.

```powershell
npm run package:vsix
```

Expected:

- VSIX package is produced successfully.

3. Compare fact docs against implementation.

Checklist:

- Commands changed: yes/no
- Paths or resource names changed: yes/no
- Architecture or single-source-of-truth changed: yes/no
- Testing strategy changed: yes/no
- Public LSP behavior changed: yes/no

Expected:

- If any answer is yes, update the corresponding fact document in the same commit.
- If all answers are no, final report says `No doc updates needed` and explains why.

4. Prepare final completion report.

The final report must include:

- Root cause of the original Task 5 blocker.
- What Task 1-5 now cover.
- Which follow-up design milestones are satisfied by existing implementation.
- Which milestones remain unimplemented or only partially verified.
- Actual commands run and their pass/fail status.
- Documentation update decision.

Task 7 completion criteria:

- `npm run gate:d3` passes or failures are triaged.
- `npm run package:vsix` passes or packaging blockers are documented.
- Current fact docs are either updated or explicitly deemed unchanged.
- The completion audit can be used as the next-thread entry point without relying on stale recovered-repo instructions.

## Recommended Next Action

Run Task 6 first. Do not start new runtime implementation until the audit answers whether P2/P3 are already complete enough to freeze, or whether they need separate implementation plans.

## Task 6 Execution Result

Executed on 2026-04-12 from branch `codex/editing-runtime-task6-audit`.

Verification run:

- `npm run test:client:repo`
  - Result: pass on current `main` at `a547558`.
- `$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'; npm run test:client:repo`
  - Result: pass.
- `$env:NSF_TEST_FILE_FILTER='client.deferred-visual-continuity'; npm run test:client:repo`
  - Result: pass.
- `$env:NSF_TEST_FILE_FILTER='client.workspace-summary'; npm run test:client:repo`
  - Result: pass, `7 passing`.
- `$env:NSF_TEST_FILE_FILTER='client.references-rename'; npm run test:client:repo`
  - Result: pass, `12 passing`.
- `$env:NSF_TEST_FILE_FILTER='client.analysis-context-workspace'; npm run test:client:repo`
  - Result: pass, `1 passing`.
- `npm run test:client:perf`
  - Result: pass, `26 passing`, `3 pending`.

Audit command:

```powershell
git grep -n "include graph\|includeGraph\|scan\|workspace summary\|workspaceSummary" -- server_cpp/src/requests server_cpp/src/interactive_semantic_runtime.cpp server_cpp/src/interactive_visibility_runtime.cpp server_cpp/src/workspace_summary_runtime.cpp
```

Audit result:

- No direct `includeGraph` hot-path scan was found in the inspected request/interactive runtime files.
- Interactive completion / hover / definition / references paths use `workspace_summary_runtime.*` as the explicit P3 boundary.
- `interactive_semantic_runtime.cpp` still has member-base fallback paths that read active include text and scan declarations through `preprocessorView.activeIncludeUris` / `runtime.activeUnitSnapshot.includeClosureUris`, with a debug resolution path of `include_closure_decl`.

Completion judgment:

- Task 1-5 can be considered complete and merged.
- P2 DeferredDoc correctness and continuity are covered by passing filtered suites and perf M4 coverage.
- P3 Workspace correctness and reverse-include behavior are covered by passing filtered suites and perf M5 coverage.
- The whole editor-first runtime upgrade should not yet be declared fully architecturally frozen because the member-base include-closure declaration scan remains a residual hidden-hot-path-fallback risk that should either be:
  - intentionally documented as a bounded current fact, or
  - removed in a small follow-up task by routing the remaining member-base cross-file fact through `interactive_visibility_runtime.*` / `workspace_summary_runtime.*`.

Recommended follow-up before Task 7 release freeze:

1. Decide whether `include_closure_decl` in `interactive_semantic_runtime.cpp` is acceptable as a bounded hot-path helper.
2. If acceptable, update `docs/architecture.md` to describe it explicitly.
3. If not acceptable, write a small implementation plan to move that path behind the shared-visible/workspace-summary boundary.
4. Only after that decision, run Task 7 release gate (`npm run gate:d3`, `npm run package:vsix`).

## Task 7 Execution Result

Executed on 2026-04-12 from branch `codex/editing-runtime-task7-freeze`.

Additional implementation/freeze work:

- fixed definition resolution for non-call-like current-unit function symbols so shader entrypoint references such as `VertexShader = vs_main` prefer current-unit include-closure targets before workspace-global fallback
- documented the remaining bounded `active-include-decl` member-base helper and the non-call current-unit definition path in `docs/architecture.md`
- tightened integration assertions:
  - `interactive-visibility` no longer assumes cross-file function completion must always record `shared-visible`; `current` is now accepted when the higher-priority layer legitimately owns the result
  - `realWorkspace.smoke` now prints actual returned basenames on failure

Verification run:

- `npm run gate:d3`
  - Result: pass after sequential rerun and definition-path fix.
- `npm run package:vsix`
  - Result: pass.

Observed intermediate failures and classification:

- Parallel `gate:d3` + `package:vsix` run was invalid because both commands mutate `server_cpp/build`; this caused a non-actionable build/output race and was not treated as a product regression.
- A stable real-workspace failure remained after sequential rerun:
  - direct include definition for `vs_main` in `building.nsf` resolved to unrelated workspace-global `pbr_flow_water.fx`
  - this was treated as a real correctness bug in definition fallback ordering, not a test-only issue
  - the fix was to consult current-unit include-closure function targets for non-call-like function references before workspace fallback
- A one-off `hover_smoke_test.py` diagnostics line-mapping failure was reproduced as passing on direct rerun and did not persist after the final `gate:d3`; it was classified as transient.

Documentation decision:

- `docs/architecture.md` updated
  - reason: Task 6 audit proved that current implementation still had two behaviorally relevant contracts not reflected in the fact docs:
    - `.` member base-type keeps a bounded `active-include-decl` helper before workspace fallback
    - definition for non-call-like shader entrypoint symbols can resolve through current-unit include-closure function targets before workspace summary
- `README.md` not updated
  - reason: no user-facing command/config summary changed
- `docs/testing.md` not updated
  - reason: validation commands and test entry structure did not change

Packaging notes:

- `package:vsix` completed successfully
- non-blocking warnings remain:
  - `package.json` is missing a `repository` field
  - no `LICENSE.md` / `LICENSE.txt` / `LICENSE` file is present

Task 7 completion judgment:

- release gate passed
- packaging validation passed
- architecture fact drift found by Task 6 is now documented
- current release-freeze blocker is cleared

Current completion status after Task 7:

- Task 1-7 are complete enough for the current release-freeze goal
- the editor-first runtime still contains a bounded `active-include-decl` helper by design, but it is no longer an undocumented residual path
- future work, if any, should be treated as a new follow-up optimization/refinement task rather than an unresolved continuation of Task 5-7
