# Editor-First Runtime Upgrade Design

> Status: Design spec, not current fact. Current fact remains in `README.md`, `docs/architecture.md`, `docs/resources.md`, and `docs/testing.md` until implementation lands.

## 1. Goal

Upgrade the language server from a feature-complete LSP into an editor-first runtime where typing, variable/function completion, `.` member completion, hover, and signature help remain responsive even while deferred document work and workspace refreshes are active.

This upgrade explicitly allows adjustments to public behavior and priority contracts when those adjustments improve editor-time responsiveness and result stability.

## 2. Top-Level Product Principle

The number-one principle is:

`Editing input is absolutely higher priority than background completeness.`

That principle means:

- interactive editor-time responses may return earlier, narrower, and more current-context-first answers
- deferred and workspace layers may be delayed, cancelled, or dropped
- workspace/global completeness must not retake control of hot-path queries

## 3. Strategic Decisions

The design fixes the following decisions up front:

- The upgrade is a large architecture upgrade, not a narrow cache tweak.
- The implementation strategy is in-place layered refactor, not a long-lived parallel runtime.
- Public behavior adjustments are allowed where needed to enforce editor-first priority.
- `.` member scenarios are first-class hot-path targets, not secondary paths.
- Variable and function completion must cover not only current-document symbols, but also cross-file symbols that are visible in the current editing context.
- Fallbacks are allowed only when they are explicit, contract-bound, and observable.

## 4. Non-Goals

The upgrade does not aim to:

- introduce a second permanent request/runtime stack beside the current one
- keep hidden include-graph scans or whole-workspace rescans alive as safety nets
- optimize `references`, `rename`, or `workspace symbol` before hot-path editing experience
- rewrite the parser into a fully incremental compiler frontend in the first phase
- preserve every current result ordering if a different ordering better fits current-context-first behavior

## 5. Layered Runtime Model

The target model is five layers inside one runtime skeleton.

### 5.1 `P0 Shell`

Scope:

- editor-owned immediate feel
- comments, brackets, strings, trigger behavior, local visual continuity

Rules:

- never waits for server state
- never depends on deferred or workspace readiness

### 5.2 `P1 Syntax`

Scope:

- changed-window immediate syntax diagnostics
- missing semicolon
- unmatched brackets
- unterminated block comments
- preprocessor pairing issues

Rules:

- emits before full diagnostics
- only performs low-cost, high-confidence checks
- never publishes stale final syntax results

### 5.3 `P1 Interactive`

Scope:

- variable completion
- function completion
- `.` member completion
- hover
- signature help
- current-document short-path definition

Rules:

- this is the only top-priority semantic lane
- it serves both current-document symbols and cross-file symbols visible in the current context
- it never performs hidden include-graph scans or whole-workspace rescans in request hot paths

### 5.4 `P2 DeferredDoc`

Scope:

- semantic tokens
- inlay hints
- document symbols
- full diagnostics

Rules:

- latest-only
- cancellation-friendly
- background isolated
- cannot slow `P1` by queue position, blocking, or synchronous dependency

### 5.5 `P3 Workspace`

Scope:

- references
- rename
- workspace symbol
- workspace summary
- reverse-include refresh

Rules:

- may supplement `P1/P2`, but cannot become the hot-path semantic engine
- correctness is important, but it is subordinate to editor responsiveness

## 6. Core Architectural Shape

The runtime continues to use the current single-owner skeleton:

- `document_owner.*`
- `document_runtime.*`
- request handlers under `server_cpp/src/requests/`

The upgrade changes the boundaries inside that skeleton, not the top-level ownership model.

### 6.1 `document_owner.*`

Responsibilities:

- single serialized mutation entry for one open document
- orders `didOpen`, `didChange`, active-unit changes, config changes, and workspace-summary refreshes
- remains the only snapshot/publication switching point

Non-responsibilities:

- does not merge semantic answers
- does not decide result priority

### 6.2 `document_runtime.*`

Responsibilities:

- document text, version, epoch, changed ranges
- `AnalysisSnapshotKey`
- `ActiveUnitSnapshot`
- published current-document state pointers
- shared invalidation boundary

New target responsibility:

- also stores the document’s `InteractiveVisibilityKey`, which binds the document to a shared cross-file-visible interactive shard

Non-responsibilities:

- does not compute final completion/hover/signature answers
- does not directly own workspace search plans

### 6.3 `interactive_semantic_runtime.*`

Target responsibility:

- the one true `P1 Interactive` query engine

It becomes responsible for:

- current-document first querying
- `last-good` eligibility checks
- merging current-document and shared-visible cross-file data
- explicit fallback order enforcement

### 6.4 Shared Interactive Visibility Layer

This is the key new conceptual layer.

It should be implemented as a new shared module rather than hidden inside request handlers.

Responsibilities:

- maintain prewarmed cross-file-visible symbol shards for the current editing context
- define what “visible in the current context” means for active unit, include closure, branch context, and defines
- expose consumer-ready queries for functions, globals, types, and member-visible cross-file facts

Non-responsibilities:

- not a workspace-global search engine
- not the implementation home of references/rename/workspace symbol
- must not perform ad hoc request-time rescans

### 6.5 `deferred_doc_runtime.*`

Target responsibility:

- a pure `P2 DeferredDoc` artifact runtime

It should own:

- semantic core
- full/range artifact caches
- lazy materialization
- latest-only background scheduling

It must stop acting like the hidden primary engine of interactive answers.

### 6.6 `workspace_summary_runtime.*`

Target responsibility:

- a pure `P3 Workspace` boundary

It remains the place for:

- cross-file summary lookup
- reverse-include knowledge
- workspace-level correctness queries

It must not override already-resolved `P1` current-context answers.

## 7. Query Order Contract

For variable completion, function completion, `.` member completion, hover, signature help, and current-document short-path definition, the target query order is:

1. current-document current snapshot
2. current-document last-good snapshot
3. shared interactive visibility shard
4. deferred document snapshot
5. workspace summary fallback

This order is a formal contract, not a guideline.

### 7.1 Meaning of Each Layer

- current snapshot
  - newest document-local state for the current version/epoch/key
- last-good snapshot
  - stale-eligible local state only while stable context remains unchanged
- shared interactive visibility shard
  - prewarmed cross-file-visible symbols valid for the current context
- deferred doc snapshot
  - background-borne same-document semantic supplement
- workspace summary fallback
  - final supplement layer only after earlier misses

### 7.2 Override Rules

- workspace summary may add miss-only candidates, but must not replace current-document or shared-visible winners
- deferred may supplement `P1`, but must not become the dominant path for `.` completion or ordinary identifier completion
- request handlers must not insert new hidden pre-query scans ahead of the contract

## 8. Interactive Visibility Design

The upgrade target includes “cross-file visible symbol completion at near-hot-path quality,” but without making workspace rescans part of every completion request.

### 8.1 Visibility Scope

The shared interactive visibility layer should cover:

- active unit visible top-level functions
- include-closure visible helper functions
- include-closure visible globals and types
- branch-aware and define-aware visible symbols

It should not attempt to represent the entire workspace as if all symbols were equally hot-path relevant.

### 8.2 Why This Layer Exists

Without this layer, the system is forced into one of two bad outcomes:

- keep hot-path requests document-local only, which fails the cross-file editing experience goal
- or pull workspace summary directly into `P1`, which threatens latency and stability

The shared visibility layer solves that by moving “currently visible cross-file semantics” into a prewarmed middle layer.

### 8.3 `.` Member and Ordinary Identifier Scenarios

For `.` member scenarios:

- base-type resolution stays in `P1`
- if the base type depends on cross-file-visible type facts, the shared visibility layer must supply them before deferred/workspace fallback

For ordinary identifier completion:

- locals and params always win
- current-document top-level symbols come next
- cross-file-visible symbols come next
- workspace-global symbols come last

## 9. Cache Model

The cache model must move away from “one large document snapshot does everything.”

### 9.1 Document State Cache

Owned by `document_runtime.*`

Contents:

- text
- version
- epoch
- changed ranges
- `AnalysisSnapshotKey`
- `ActiveUnitSnapshot`
- `InteractiveVisibilityKey`

Purpose:

- represent state and invalidation boundaries, not final UI answers

### 9.2 Current Interactive Cache

Owned by `interactive_semantic_runtime.*`

Contents:

- locals
- params
- current-document functions
- current-document globals
- struct/type info
- member base-type results
- `last-good interactive snapshot`

Purpose:

- serve the real hot path
- allow small-scope promote or slice rebuild

### 9.3 Shared Interactive Visibility Cache

Owned by the new shared visibility layer

Contents:

- active-unit visible cross-file functions
- include-closure visible top-level symbols
- branch-aware visibility shards
- define-aware visibility shards

Purpose:

- provide cross-file-visible symbols at `P1` quality
- avoid request-time workspace rescans

### 9.4 Deferred Artifact Cache

Owned by `deferred_doc_runtime.*`

Contents:

- semantic core
- semantic tokens full/range
- inlay hints full/range
- document symbols
- full diagnostics

Purpose:

- serve background artifacts with fine-grained invalidation

## 10. Invalidation Model

### 10.1 Small text edit, semantic context unchanged

Allowed behavior:

- `P1` may reuse `last-good` or perform narrow promote
- shared interactive visibility remains valid
- deferred range caches invalidate only overlapping buckets

### 10.2 Current-document semantics changed, shared context unchanged

Required behavior:

- rebuild the current-document interactive portion
- keep shared visibility shard if active unit/include/defines did not change
- defer full artifact rebuild to background

### 10.3 Active unit / include closure / defines changed

Required behavior:

- invalidate shared visibility shard
- switch to a new `AnalysisSnapshotKey`
- restore `P1` to a quickly usable state before waiting for complete background enrichment

### 10.4 Workspace summary version changed

Required behavior:

- do not mass-dirty all open documents
- refresh only impacted documents and impacted shared visibility shards

## 11. Allowed and Forbidden Fallbacks

Fallback is allowed only when it is explicit, contract-bound, and safe.

### 11.1 Allowed Fallbacks

- formal interactive query-order fallback:
  - `current -> last-good -> shared-visible -> deferred -> workspace`
- `last-good` fallback while stable context is unchanged
- temporary `P1` narrowing when shared visibility is not yet ready:
  - current-document and last-good results may return first
  - deferred/workspace may supplement only as explicit lower-priority misses

### 11.2 Forbidden Fallbacks

- hidden include-graph scan in hot paths
- ad hoc whole-workspace recomputation in hot paths
- long-lived compatibility layer that keeps both old and new main logic active
- silent “best effort” fake answers that conceal missing semantic state
- stale final result publish from `P2` or `P3`
- workspace results overriding already-resolved current-document or shared-visible winners

## 12. Public Behavior Changes

This upgrade intentionally allows the following public behavior changes:

- completion, hover, signature help, and `.` member behavior become more current-context-first
- current-document and current-context-visible cross-file symbols may appear earlier than workspace-global candidates
- workspace-global candidates may appear later or be omitted in some editing-time situations
- result ordering may change when needed to improve stability and editor relevance
- deferred/full diagnostics may arrive later relative to interactive responses

The design does not allow uncontrolled behavior drift. The new ordering and priority rules must be documented and tested.

## 13. Milestone Plan

### M0. Contract and observability

- write the runtime spec
- add metrics/debug surfaces to identify which layer answered a request
- identify and document old hidden fallback paths that must leave the hot path

### M1. `P1` skeleton enforcement

- enforce `P1 Syntax` and `P1 Interactive` as true top-priority lanes
- make the formal query order executable
- keep variable/function/`.`/hover/signature help stable under background pressure

### M2. Shared interactive visibility

- add the shared visibility layer
- prewarm active-unit/include-closure visible shards
- elevate cross-file-visible completion into `P1`

### M3. Deferred document reconstruction

- split semantic core from lazy artifacts
- introduce artifact/range caches
- finish latest-only, cancellation, and overlap invalidation at `P2`

### M4. Workspace boundary cleanup

- move references/rename/workspace symbol fully behind `P3`
- restrict workspace influence to explicit supplement roles
- limit reverse-include refresh to impacted documents

### M5. Behavior freeze and release criteria

- finalize facts docs
- lock tests and performance gates
- verify that no hidden hot-path fallback has been reintroduced

## 14. Testing and Acceptance

### 14.1 Correctness suites

Need explicit coverage for:

- variable completion
- function completion
- `.` member completion
- hover
- signature help
- short-path definition

Each of those should include:

- current-document hit
- current-context-visible cross-file hit
- deferred/workspace supplement or miss behavior

### 14.2 Priority suites

Need tests that prove:

- semantic tokens, inlay hints, document symbols, and workspace refresh pressure do not meaningfully degrade `P1`
- `P1 Syntax` emits ahead of full diagnostics

### 14.3 Stability suites

Need tests for:

- rapid edit bursts
- active-unit switches
- defines switches
- include-closure changes
- stale final publish prevention

### 14.4 Observability

Metrics/debug output must show which layer answered:

- current
- last-good
- shared-visible
- deferred
- workspace

### 14.5 Documentation acceptance

When implementation lands, at minimum update:

- `README.md`
- `docs/architecture.md`
- `docs/testing.md`

Update those docs in the same implementation tasks that change runtime contracts or public behavior.

## 15. Risks

Primary risks:

- shared visibility scope becoming ambiguous or too broad
- old fallback paths surviving inside request handlers
- workspace summary bleeding back into `P1` main-result selection
- background improvements accidentally reintroducing queue or lock pressure into `P1`

The mitigation is to keep the query order, fallback policy, and ownership boundaries explicit and test-visible.

## 16. Why This Design Fits The Current Codebase

This design fits the repository because it:

- keeps `document_owner.*` and `document_runtime.*` as the existing single-owner backbone
- reuses the current active-unit and shared-context concepts instead of replacing them wholesale
- upgrades `interactive_semantic_runtime.*`, `deferred_doc_runtime.*`, and `workspace_summary_runtime.*` into clearer roles instead of building a second architecture next to them
- supports gradual milestone execution and repo-mode verification

## 17. Exit Condition

The upgrade should be considered architecturally complete only when all of the following are true:

- `P1` hot paths consistently outrank background work
- current-document and current-context-visible cross-file completion feel like one coherent interactive experience
- `P2` and `P3` no longer leak hidden fallback scans into hot paths
- stale final results are prevented in background layers
- facts docs, tests, and metrics reflect the new contract
