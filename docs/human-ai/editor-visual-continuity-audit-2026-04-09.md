# Editor Visual Continuity Audit (2026-04-09)

This note is a human-AI collaboration artifact, not a current fact document.

## Scope

Audit user-visible "results disappear, then come back" behavior after `didChange`,
especially around:

- diagnostics squiggles
- inlay hints
- deferred/full-cache rebuild timing

## Commands run

- `npm run compile`
- `node .\out\test\runCodeTests.js --mode repo --file-filter diagnostics`

## Shared root causes

### RC1. Partial fast results overwrite older full results

- `server_cpp/src/app/main.cpp:324-340`
- `server_cpp/src/app/main.cpp:395`

`didChange` fast diagnostics publish `immediateResult.diagnostics` directly through
`textDocument/publishDiagnostics`. In LSP this is a whole-document replacement, so
older semantic diagnostics are not preserved while full diagnostics are still pending.

### RC2. Deferred full artifacts are invalidated eagerly on edit

- `server_cpp/src/document_runtime.cpp:695-711`

For stale-eligible edits, the current deferred snapshot is copied, then:

- `semanticTokensFull` is cleared
- `inlayHintsFull` is cleared
- `fullDiagnostics` is cleared

This happens before a replacement full artifact is guaranteed to exist.

### RC3. Deferred full diagnostics and full inlay prewarm are serialized into one publish point

- `server_cpp/src/deferred_doc_runtime.cpp:517-552`

The deferred worker computes `fullDiagnostics`, then computes `inlayHintsFull`, and
only after both complete does it store the new deferred snapshot. This couples
diagnostics visual recovery to inlay work.

### RC4. Client inlay provider turns transient instability into empty UI state

- `client/src/client_editor_feedback.ts:343-371`
- `client/src/client_editor_feedback.ts:724-786`

The client returns `[]` when:

- the client is not ready
- the request token is canceled
- the indexing state is not stable
- the RPC is canceled or treated as transient

Returning `[]` gives VS Code an explicit empty result instead of "keep old hints".

## Collected issues

### Issue A. Semantic squiggles disappear after edit until full diagnostics comes back

Status: Confirmed

Severity: High

Evidence:

- `server_cpp/src/app/main.cpp:2319-2350`
- `server_cpp/src/app/main.cpp:324-340`
- `server_cpp/src/immediate_syntax_diagnostics.cpp:434-438`

Mechanism:

1. `didChange` schedules fast diagnostics immediately.
2. Fast diagnostics publish a changed-window-oriented immediate syntax result.
3. That publish replaces the previous whole diagnostics set.
4. Full diagnostics come later, so semantic squiggles visibly disappear in between.

This matches the user-reported "wave lines disappear during inlay/deferred work and
come back later" behavior.

### Issue B. Comment-only / syntax-only edits can permanently drop semantic diagnostics until a later unrelated full rebuild

Status: High-confidence inference from code, not yet isolated by a dedicated test

Severity: High

Evidence:

- `server_cpp/src/document_runtime.cpp:685-711`
- `server_cpp/src/app/main.cpp:2324-2350`
- `server_cpp/src/app/main_did_change_classification.cpp:52-73`

Mechanism:

1. `documentRuntimeUpsert(...)` clears cached `fullDiagnostics`.
2. For comment-only / semantic-neutral / syntax-only edits, `skipExpensivePostChangeWork`
   may be true.
3. When that happens, no replacement full diagnostics job is scheduled.
4. Fast diagnostics still publish immediate syntax only.

Result:

- unrelated semantic diagnostics can disappear even though the edit did not change
  semantic meaning
- they may stay gone until some later event triggers full diagnostics again

This is stricter than Issue A because the gap is not just "slow recovery"; in some
paths it may be "no recovery yet".

### Issue C. Inlay hints can blink to empty during indexing-state wobble or request cancellation

Status: Confirmed from client behavior

Severity: High

Evidence:

- `client/src/client_editor_feedback.ts:724-786`
- `client/src/client_editor_feedback.ts:343-371`
- `server_cpp/src/app/main.cpp:1296-1327`
- `server_cpp/src/app/main.cpp:1389-1393`

Mechanism:

1. The inlay provider runs.
2. If indexing is not stable, it returns `[]`.
3. If the request is canceled or hits a transient RPC error, it also returns `[]`.
4. Server latest-only logic actively cancels older queued inlay requests.

Result:

- visible inlay hints can clear even when old hints were still better than nothing
- repeated re-requests can produce visible blinking rather than stable continuity

### Issue D. Inlay refresh waves amplify the chance of visible inlay blinking

Status: Confirmed amplifier

Severity: Medium

Evidence:

- `client/src/client_editor_feedback.ts:283-320`
- `client/src/client_editor_feedback.ts:557-568`
- `client/src/client_editor_feedback.ts:664-685`
- `client/src/client_editor_feedback.ts:690-699`

Mechanism:

- refreshes are scheduled with multiple delayed waves such as `[0, 120]`,
  `[40, 160]`, `[80, 260]`
- `nsf/inlayHintsChanged` also triggers another refresh
- git-storm polling can trigger yet another wave after indexing stabilizes

By itself this is not the root cause, but it increases the number of times the
provider can hit the "return empty array" path from Issue C.

### Issue E. Slow inlay resolve can trigger a second inlay blink without text changes

Status: High-confidence inference from code

Severity: Medium

Evidence:

- `server_cpp/src/inlay_hints_runtime.cpp:165-170`
- `server_cpp/src/deferred_doc_runtime.cpp:756-762`

Mechanism:

1. Slow inlay resolve finishes and updates parameter metadata.
2. It invalidates `inlayHintsFull`.
3. It sends `nsf/inlayHintsChanged`.
4. Client refreshes again.

If that refresh hits cancellation or unstable-indexing paths, hints can briefly drop
to empty even though no new edit happened.

## Lower-risk / not-currently-equivalent paths

### Semantic tokens

Lower risk than diagnostics/inlay for this specific symptom.

Evidence:

- `server_cpp/src/deferred_doc_runtime.cpp:633-668`

Why lower risk:

- on cache miss, semantic tokens rebuild synchronously instead of intentionally
  returning empty because indexing is unstable
- no client-side "if unstable then return empty tokens" gate was found

Still worth watching because they share latest-only cancellation machinery, but the
current code does not show the same direct "empty-on-instability" behavior as inlay.

### Document symbols / outline

Potentially affected by latest-only cancellation, but user-visible impact is lower
and no direct editor-content flicker path was confirmed in this audit.

## Recommended batch-fix order

1. Decouple diagnostics continuity from immediate syntax publish.
   - Keep previous semantic diagnostics until new full diagnostics for the same
     stable context are ready.
   - Do not let changed-window immediate syntax replace unrelated semantic results.

2. Stop clearing deferred `fullDiagnostics` on semantic-neutral / syntax-only edits
   unless a replacement full result is scheduled in the same flow.

3. Split deferred artifact commit timing.
   - Store/publish `fullDiagnostics` as soon as they are ready.
   - Do not make diagnostics wait for full inlay prewarm.

4. Change inlay continuity policy on the client.
   - Prefer "keep old hints" semantics on transient cancel / unstable indexing.
   - Avoid mapping every transient state to `[]`.

5. Reduce inlay refresh amplification.
   - Collapse redundant waves.
   - Revisit whether `nsf/inlayHintsChanged` should always force an immediate refresh
     when another refresh is already pending.

## Missing tests worth adding before or during the fix

1. Semantic diagnostics persist across comment-only edits.
2. Semantic diagnostics persist across punctuation-only edits that do not change
   semantic meaning.
3. Inlay hints do not disappear when an earlier inlay request is canceled by
   latest-only replacement.
4. Inlay hints do not disappear during a temporary unstable-indexing window if a
   previous visible result already exists.
5. Deferred full diagnostics can publish before inlay full prewarm finishes.
