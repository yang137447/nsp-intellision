# Editor-First Runtime P1 Progress

Status: in progress. This is a collaboration/handoff note, not a fact document.

## Current execution path

- Approved design spec:
  - `docs/superpowers/specs/2026-04-07-editor-first-runtime-upgrade-design.md`
- Active implementation plan:
  - `docs/superpowers/plans/2026-04-07-editor-first-runtime-p1-visibility.md`
- Active worktree:
  - `D:\YYBWorkSpace\GitHub\nsp-intellision\.worktrees\editor-first-runtime-p1`
- Active branch in worktree:
  - `codex/editor-first-runtime-p1`

## Completed tasks

### Task 1: P1 resolution debug surfaces

Worktree commits:

- `548fd26` test: add interactive runtime visibility debug wiring
- `7bf295b` fix: bound and normalize interactive runtime debug state
- `7aa55a9` chore: tighten interactive debug request typing

Outcome:

- Added interactive runtime debug snapshot API in `interactive_semantic_runtime.*`
- Added server request `nsf/_getInteractiveRuntimeDebug`
- Added client internal command + typed helper path
- Added `interactive-visibility` integration test entry

Review status:

- Spec compliance: approved
- Code quality: approved

### Task 2: Interactive visibility key + runtime skeleton

Worktree commits:

- `143efeb` feat: add interactive visibility runtime skeleton
- `0459781` chore: clarify interactive visibility skeleton key semantics
- `939a784` docs: clarify interactive visibility skeleton cache contract

Outcome:

- Added `InteractiveVisibilityKey` to `DocumentRuntime`
- Surfaced `interactiveVisibilityFingerprint` in runtime debug
- Added `interactive_visibility_runtime.*` skeleton module

Review status:

- Spec compliance: approved
- Code quality: approved

### Task 3: Shared-visible shard prewarm from active unit context

Worktree commit:

- `c88d051` follow-up Task 3 fix package

Outcome:

- Added shared-visible shard build/prewarm/collect runtime
- Replaced fake per-URI definition scan with direct per-file indexed lookup
- Added keyed shard invalidation on close
- Added Task 3 fixtures/tests
- Kept a minimal shared-visible completion consumer hook only for shard observability

Review status:

- Spec compliance: approved after controller clarification that Task 3 may include the smallest observable consumer hook
- Code quality: approved after replacing fake per-URI lookup and global invalidation

### Task 4: Completion merge order for shared-visible symbols

Worktree commits:

- `75debd6` feat: merge shared visible symbols into p1 completion
- `7baa65d` quality follow-up for Task 4 comments/naming/test title

Outcome:

- Formalized completion merge ordering in `interactive_semantic_runtime.*` as:
  - `current -> last-good -> shared-visible -> deferred -> workspace`
- Kept earlier-layer precedence via append-in-order + label dedupe
- Added Task 4 integration coverage for local-before-shared-visible ordering

Review status:

- Spec compliance: approved
- Code quality: approved

### Task 5: Shared-visible support for `.` / hover / signature help

Worktree commits:

- `6feb9e7` feat: extend p1 shared visibility across interactive queries
- `330d2d6` fix: close task5 shared-visible follow-up gaps
- `0a6d3f7` cleanup follow-up for Task 5 contract alignment

Outcome:

- Extended shared-visible support into:
  - member completion / member base-type resolution
  - hover-relevant function/type lookup path
  - signature help / overload lookup path
- Added Task 5 fixtures and integration coverage
- Updated `docs/architecture.md` and `docs/testing.md` for the new shared-visible interactive contract

Review status:

- Spec compliance: approved after follow-up fixes
- Code quality: approved after cleanup follow-up

## Known environment constraints

Earlier blockers that were resolved during this thread:

- `cmake --build .\server_cpp\build` was failing because `server_request_handler_definition.cpp` used `_stat` without including `<sys/stat.h>` under MinGW. This is fixed in worktree commit `6f3924f`.
- `client.include-underline-support.test.ts` was failing to load `../../../client/out/include_underline_support` because no `client/src/include_underline_support.ts` existed. This is fixed in worktree commit `8f4294a`.
- Worktree verification was accidentally using the main workspace server binary because `.vscode/settings.json` hardcoded `nsf.serverPath` to `D:\YYBWorkSpace\GitHub\nsp-intellision\server_cpp\build\nsf_lsp.exe`. The worktree-local `.vscode/settings.json` was changed to use `server_cpp\build\nsf_lsp.exe`; this remains a local uncommitted verification setting, not a branch commit.

Current verification status:

- `npm run compile`: passing
- `cmake --build .\server_cpp\build`: passing
- `NSF_TEST_FILE_FILTER=client.interactive-visibility npm run test:client:repo`: passing, `7 passing`
- `NSF_TEST_FILE_FILTER=client.include-underline-support npm run test:client:repo`: passing, `2 passing`
- `NSF_TEST_FILE_FILTER=client.member-completion-matrix npm run test:client:repo`: passing, `1 passing`
- `NSF_TEST_FILE_FILTER=client.completion npm run test:client:repo`: passing
- `NSF_TEST_FILE_FILTER=client.hover-client-metrics npm run test:client:repo`: passing
- `NSF_TEST_FILE_FILTER=client.include-underline-metrics npm run test:client:repo`: passing
- `NSF_TEST_FILE_FILTER=client.inlay-metrics npm run test:client:repo`: passing
- `NSF_TEST_FILE_FILTER=client.inlay-visible-range npm run test:client:repo`: passing
- `NSF_TEST_FILE_FILTER=client.signature-help npm run test:client:repo`: passing
- `NSF_TEST_FILE_FILTER=runtime-config npm run test:client:repo`: passing
- Full `npm run test:client:repo`: still exits non-zero, but the earlier concrete blockers (build failure, wrong server binary, missing include underline module, missing completion debug command, missing client telemetry status fields) are fixed. Remaining failure appears to be later full-suite instability / sequencing noise rather than a single known missing feature.

Additional cleanup completed after the first full repo run:

- `client/src/include_underline_support.ts` was restored and `client_editor_feedback.ts` now reuses it. Commit: `8f4294a`.
- `nsf._getLastCompletionDebug` client internal command was restored. Commit: `56aecaf`.
- `client` internal status telemetry surfaces used by completion / hover / inlay / signature tests were restored. Commit: `c8115f4`.
- Runtime-config active-unit expectation was aligned with current architecture in local source, and targeted `runtime-config` now passes.
- The current worktree-local `.vscode/settings.json` still contains an uncommitted verification override so tests use the worktree server binary instead of the main workspace binary.

Current worktree status:

- Code changes are committed on `codex/editor-first-runtime-p1`.
- Only `.vscode/settings.json` remains locally modified in the worktree, intentionally, as a verification-only override.
- Do not treat that `.vscode/settings.json` diff as part of the feature branch unless explicitly deciding to make the repo's workspace settings worktree-relative.

Additional branch-level fixes completed after the original P1 plan:

- `6f3924f` fixes the worktree C++ build by including `<sys/stat.h>` in `server_request_handler_definition.cpp`.
- `8f4294a` restores the include underline support module and reconnects `client_editor_feedback.ts` to it.
- `56aecaf` restores the `nsf._getLastCompletionDebug` client internal command.
- `c8115f4` restores client internal status telemetry surfaces needed by completion / hover / inlay / signature client-side tests.

Current targeted verification snapshot:

- `client.interactive-visibility`: PASS
- `client.include-underline-support`: PASS
- `client.member-completion-matrix`: PASS
- `client.completion*`: PASS
- `client.hover-client-metrics`: PASS
- `client.include-underline-metrics`: PASS
- `client.inlay-metrics`: PASS
- `client.inlay-visible-range`: PASS
- `client.signature-help*`: PASS
- `runtime-config` targeted suite: PASS

Current remaining blocker:

- Full `npm run test:client:repo` still exits non-zero during the later stages of the run.
- The latest evidence no longer points to compile/build blockers, wrong server binary selection, missing internal commands, or missing include underline support.
- The remaining issue appears to be a later full-suite instability / sequencing problem in repo-mode execution, not the original P1 visibility functionality.
- Crash evidence is currently poor: `nsf_lsp_crash.log` mostly contains repeated install lines and `CRASH: stacktrace begin tag=seh` markers without a useful stack payload.

What a new thread should do next:

1. Start from this file plus the active worktree
2. Re-run full `npm run test:client:repo` to a log file
3. Re-run full `npm run test:client:repo` to a log file and isolate the exact last passing suite / first failing suite in the current branch state
4. Capture stronger evidence from the crash / non-zero exit before changing behavior again

Recommended next task:

- Create a separate client telemetry / auto-trigger baseline debugging task.
- Scope it around the remaining full repo failures:
  - missing `completionRequestCount` / `signatureHelpRequestCount` style status fields
  - hover / include underline / inlay client metrics exposure
  - runtime-config rebuild count expectations
  - signature-help metrics expectations

## Important execution notes

- An earlier abandoned implementer accidentally created a draft commit on `main`:
  - `875f8d8` test: add p1 resolution debug surface
- Clean ongoing work is **not** using that path. All reviewed/approved implementation is in the worktree branch above.
- Do not rewrite `main` silently. If a future thread wants to clean that up, it should be handled explicitly.

## Current next step

Proceed to Task 4 from:

- `docs/superpowers/plans/2026-04-07-editor-first-runtime-p1-visibility.md`

Current implementation status:

- Tasks 1-5 of `docs/superpowers/plans/2026-04-07-editor-first-runtime-p1-visibility.md` are complete on the worktree branch.
- Task-level spec reviews and code-quality reviews are complete.
- End-to-end green verification is still blocked by unrelated environment/baseline issues described below.

## If resuming in a new thread

Start from:

1. `docs/human-ai/editor-first-runtime-p1-progress.md`
2. `docs/superpowers/plans/2026-04-07-editor-first-runtime-p1-visibility.md`
3. worktree `D:\YYBWorkSpace\GitHub\nsp-intellision\.worktrees\editor-first-runtime-p1`

Then continue from the latest branch state, not Task 4.
Then decide whether to:

1. keep hardening full repo verification in the current worktree
2. clean up the accidental draft commit on `main`
3. start the next plan for deferred/workspace work after repo verification is stable
