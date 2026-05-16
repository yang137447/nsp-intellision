# Semantic Token Variable Color Execution Plan

Status: Proposed execution plan. Not yet implemented.
Scope note: optimized to keep `.nsf` as the only active/root unit.

Date: 2026-05-15.

## Background

User feedback: variable hint / variable color does not visibly change in the
editor.

Real-workspace evidence was collected against:

```text
C:/Software/WorkTemp/G66ShaderDevelop/shader-source/base/building.nsf
```

The server currently advertises this semantic token legend:

```text
tokenTypes:
  keyword, number, macro, function, variable, type, property, operator

tokenModifiers:
  <empty>
```

Sample tokens from `building.nsf`:

```text
diffuse_color  -> variable, modifiers=0
p0             -> variable, modifiers=0
MaterialInputs -> variable, modifiers=0
```

Conclusion:

- VS Code themes only receive a coarse `variable` category for most ordinary
  identifiers.
- The editor has no semantic signal for parameter, declaration, write access,
  local variable, global variable, or field write.
- The color experience is therefore limited by server semantic-token output,
  not by a missing client-side color decorator.

## Problem Statement

`server_cpp/src/semantic_tokens.cpp` is currently a lexical semantic-token
scanner. It classifies identifiers by local text shape only:

- keyword / system semantic
- builtin type-like keyword
- function call by following `(`
- property by direct `.` prefix
- fallback variable

It does not consume the current document semantic snapshot, even though the
deferred runtime already builds one for the same document analysis key.

This makes all variable-like identifiers visually flat under themes that rely
on semantic token type / modifier differences.

## Goals

- Make variable coloring driven by server semantic truth rather than client
  decoration.
- Fully cover both registered shader document kinds, `.nsf` and `.hlsl`, through
  the same LSP semantic-token path.
- Keep `.nsf` as the only unit concept. `.hlsl` documents are covered as
  opened/requested shader documents and as shared include files under the
  currently selected `.nsf` active-unit context; they are not selected, pinned,
  discovered, or indexed as root units.
- Define "workspace coverage" as: any opened or requested workspace document
  whose language / extension is handled by the extension gets the same semantic
  role classification under its current analysis context. It does not mean
  eagerly precomputing semantic tokens for every unopened file.
- For active-unit-sensitive `.hlsl` coverage, "current analysis context" means
  the preprocessor/include context inherited from the current `.nsf` unit when
  the `.hlsl` file is reached through that unit's active include closure.
  Standalone `.hlsl` documents still receive current-document semantic
  classification, but no `.hlsl` document becomes a unit.
- Keep semantic tokens under the existing deferred document runtime.
- Reuse `SemanticSnapshot` as the semantic fact source.
- Add enough standard LSP semantic signal for themes to distinguish common
  variable roles across NSF and HLSL files:
  - parameter
  - declaration
  - modification
  - property write
- Cover the declaration / reference roles currently represented in the shared
  semantic model:
  - function parameters
  - local variables
  - globals / FX metadata globals
  - struct fields
  - cbuffer fields
  - member/property assignment targets
- Cover declaration-side `label: Type name` parameter forms only if the
  separate shared-parser gate is cleared. Do not add a semantic-token-only
  workaround for that syntax.
- Preserve current lexical highlighting for comments and strings: TextMate /
  editor grammar remains responsible for those.
- Keep implementation scoped and maintainable.

## Non-Goals

- Do not implement custom client-side color decorations.
- Do not hardcode theme colors.
- Do not move language knowledge into `client/`.
- Do not introduce feature flags, compatibility shims, dual token paths, or
  old/new behavior toggles.
- Do not redesign the full AST or semantic cache as part of this task.
- Do not implement a workspace-wide batch semantic-token scan for unopened
  documents.
- Do not make `.hlsl` an active unit or change `client_active_unit.*`,
  `active_unit.*`, active-unit selection UI, sibling-unit discovery, or
  workspace root-unit indexing to treat `.hlsl` as a unit.
- Do not attempt full type-precise read/write analysis for every expression.
- Do not change hover, completion, signature help, diagnostics, or semantic
  token scheduling beyond what this semantic-token behavior requires.

## Execution Gates

- Use the existing `DocumentRuntime.analysisSnapshotKey` / deferred snapshot
  ownership path for semantic-token caching. Do not introduce a second
  semantic-token-specific cache owner or cache key type.
- Keep declaration-side `label: Type name` parsing out of the semantic-token
  hot path. If the target fixtures really require that syntax, confirm the
  parser impact separately first because it can change hover, completion,
  signature help, diagnostics, inlay hints, definition, references, and
  rename.
- For the `{ language: 'hlsl' }` selector test, force the document language id
  explicitly in the test. The current extension still associates `.hlsl`
  files with the `nsf` language contribution, so file extension alone is not a
  proof of the `hlsl` selector path.

## Public Behavior Change

This task changes semantic tokens, which is editor-visible public behavior.

Accepted behavior change:

- Semantic token legend gains standard type / modifiers.
- Variable-like identifiers may now be classified as `parameter`, `variable`,
  or `property`, and may include `declaration` or `modification` modifiers.
- `.nsf` and `.hlsl` files handled by the extension receive the same role /
  modifier semantics through LSP semantic tokens, while `.nsf` remains the only
  active/root unit.
- CBuffer field declarations may now be classified as `property + declaration`.
- Declaration-side parameter labels are only included if the separate parser
  gate is explicitly cleared.

Stop and confirm again before adding any of the following:

- custom non-standard token types or modifiers
- theme color overrides in `package.json`
- client-side decoration overlays
- broad AST / semantic snapshot storage rewrites
- changes to hover, completion, signature help, diagnostics, or TextMate grammar
- fallback behavior that treats `.nsf` and `.hlsl` differently
- changes that allow `.hlsl` to be selected, pinned, inferred, or indexed as an
  active/root unit
- shared `label: Type name` parsing changes that alter hover, completion,
  signature help, diagnostics, inlay hints, definition, references, or rename
  behavior beyond the semantic-token payload accepted by this plan

## Architecture Direction

Preferred architecture:

1. `document_owner.*` and `document_runtime.*` keep owning current document
   analysis keys and snapshot freshness.
2. `client_active_unit.*`, `active_unit.*`, and workspace include-closure logic
   continue to treat `.nsf` as the only active/root unit source.
3. `deferred_doc_runtime.*` keeps owning semantic tokens as a deferred artifact.
4. `semantic_snapshot.*` remains the consumer-ready semantic fact source.
5. For an opened `.hlsl` that belongs to the active `.nsf` include closure, the
   semantic snapshot must be built from the included-document preprocessor view
   produced from that `.nsf` unit. For standalone `.hlsl`, build from the
   document's own preprocessor context. Snapshot reuse stays on the existing
   document runtime analysis key; no new cache owner is introduced.
6. CBuffer field data is promoted into `SemanticSnapshot` rather than passed to
   semantic tokens through a parallel `HlslAstDocument` input. This keeps
   field facts in one shared semantic model.
7. `semantic_tokens.*` stays responsible for token rendering and LSP encoding,
   but can consume a read-only `SemanticSnapshot` when available.
8. Client remains a consumer of LSP semantic tokens only.

This keeps semantic coloring aligned with existing server architecture instead
of creating a parallel client-specific visual truth.

## Implementation Plan

### Phase 1: Extend Token Legend

Files:

- `server_cpp/src/semantic_tokens.hpp`
- `server_cpp/src/semantic_tokens.cpp`
- `server_cpp/src/app/main.cpp` indirectly consumes the legend from
  `createDefaultSemanticTokenLegend()`

Tasks:

- Keep current token type order unchanged.
- Append standard token type:
  - `parameter`
- Add standard token modifiers:
  - `declaration`
  - `modification`
- Add resolver helpers:
  - token type indices
  - token modifier bit masks

Expected legend:

```text
tokenTypes:
  keyword, number, macro, function, variable, type, property, operator,
  parameter

tokenModifiers:
  declaration, modification
```

Acceptance:

- Existing semantic-token tests still find `keyword`.
- Existing themes that only know old types continue to work.
- No existing token type index is shifted.

### Phase 2: Pass SemanticSnapshot Into Semantic Tokens

Files:

- `server_cpp/src/semantic_tokens.hpp`
- `server_cpp/src/semantic_tokens.cpp`
- `server_cpp/src/deferred_doc_runtime.cpp`

Tasks:

- Forward declare or include `SemanticSnapshot` in `semantic_tokens.hpp`.
- Change semantic-token builders to accept an optional snapshot:

```cpp
Json buildSemanticTokensFull(const std::string &text,
                             const SemanticTokenLegend &legend,
                             const SemanticSnapshot *snapshot = nullptr);

Json buildSemanticTokensRange(const std::string &text, int startLine,
                              int startCharacter, int endLine,
                              int endCharacter,
                              const SemanticTokenLegend &legend,
                              const SemanticSnapshot *snapshot = nullptr);
```

- In `buildDeferredSemanticTokensFull(...)`, pass:

```cpp
deferred->semanticSnapshot.get()
```

- In `buildDeferredSemanticTokensRange(...)`, pass the same snapshot pointer.
- Preserve lexical-only behavior when the pointer is null.

Acceptance:

- Existing fallback path remains valid.
- Semantic tokens still build if deferred semantic core is unavailable.
- No new owner or cache layer is introduced.

### Phase 2A: Build SemanticSnapshot With NSF-Unit Context

Files:

- `server_cpp/src/deferred_doc_runtime.cpp`
- `server_cpp/src/semantic_snapshot.hpp`
- `server_cpp/src/semantic_snapshot.cpp`
- `server_cpp/src/expanded_source.*` only if the shared snapshot API needs an
  explicit line-preserving expanded-source input
- `server_cpp/src/preprocessor_view.*` only if an existing included-document
  helper cannot be reused as-is

Tasks:

- Keep `.nsf` as the only active unit. Do not modify active-unit selection,
  unit persistence, or `.hlsl` sibling discovery.
- In the deferred semantic core path, derive semantic snapshot input from the
  document runtime's active-unit snapshot:
  - If `doc.uri` is the active `.nsf` unit, use that unit's own preprocessor
    context.
  - If `doc.uri` is an opened `.hlsl` reached through the active `.nsf` include
    chain, build an included-document `PreprocessorView` from the active unit
    and use it for the `.hlsl` snapshot.
  - If no active `.nsf` unit reaches the `.hlsl`, build the snapshot from the
    `.hlsl` document's own preprocessor context and configured defines.
- Add a shared semantic-snapshot build entry that can consume a prepared
  line-preserving expanded source or preprocessor view, rather than making
  `semantic_tokens.cpp` rebuild include context by itself.
- Reuse the same active-unit include-context helper shape already used by
  diagnostics (`buildIncludedDocumentPreprocessorView(...)`) instead of adding
  a semantic-token-only parser or a `.hlsl` unit path.
- Reuse the existing document runtime analysis key as the owner key for the
  deferred semantic snapshot. The key already carries the active-unit path,
  include-closure fingerprint, branch fingerprint, defines, include paths,
  shader extensions, workspace folders, workspace summary version, document
  epoch, and document version.

Acceptance:

- An opened `.hlsl` include receives semantic-token roles under the selected
  `.nsf` unit's active branch and macro context.
- The same `.hlsl` opened without a reaching `.nsf` unit still gets
  current-document semantic-token roles.
- No `.hlsl` document can become an active/root unit as a side effect of this
  work.
- The active-unit analysis key invalidates stale semantic-token artifacts when
  the selected `.nsf` unit, include closure, defines, or preset changes.
- No new semantic-token cache owner or cache key type is introduced.

### Phase 3: Extend SemanticSnapshot Field Coverage

Files:

- `server_cpp/src/semantic_cache.hpp`
- `server_cpp/src/semantic_snapshot.cpp`
- `server_cpp/src/hlsl_ast.hpp` / `server_cpp/src/hlsl_ast.cpp` as the existing
  cbuffer source
- shared parameter declaration parsing helpers only if the target fixtures
  actually need declaration-side `label: Type name`

Tasks:

- Add cbuffer field facts to `SemanticSnapshot`, preserving line and
  declaration-span information:

```cpp
struct CBufferInfo {
  std::string name;
  int line = -1;
  std::vector<FieldInfo> fields;
};
std::vector<CBufferInfo> cbuffers;
std::unordered_map<std::string, size_t> cbufferByName;
```

- Populate cbuffer fields from the existing `HlslAstDocument::cbuffers`.
- Promote exact declaration spans already available from shared declaration
  parsing into `SemanticSnapshot` for globals, struct fields, and cbuffer
  fields. Do not rely on line-only matching for declaration modifiers.
- Keep struct fields and cbuffer fields distinguishable internally, but expose
  both to semantic-token classification as known property declarations.
- Add precise parameter binding-name location data to the shared AST /
  semantic snapshot path before attempting declaration modifiers. A name-only
  parameter match is not precise enough for complete coverage, especially for
  declaration-side `label: Type name`.

Recommended shape:

```cpp
struct ParameterInfo {
  std::string name;
  std::string type;
  int line = -1;
  int character = -1;
  size_t offset = 0;
};
```

- Record parameter binding spans for the already-supported parameter forms
  first.
- If the target fixtures still need declaration-side `label: Type name`, stop
  before changing the shared parser and confirm the cross-feature impact
  because that is a separate public behavior change.
- Update header comments in touched `*.hpp` files to document the new snapshot
  ownership / consumer contract.

Acceptance:

- `SemanticSnapshot` is still the single semantic fact source consumed by
  semantic tokens.
- Struct and cbuffer field declaration lines are available without scanning
  `HlslAstDocument` in `semantic_tokens.cpp`.
- Parameter declaration spans are available from shared semantic facts rather
  than guessed by name within the signature range.
- `.nsf` label-style parameter declarations are only added if the separate
  parser gate is explicitly cleared.
- No new request-layer or client-side language knowledge is introduced.

### Phase 4: Build A Lightweight Semantic Classification Context

File:

- `server_cpp/src/semantic_tokens.cpp`

Tasks:

- Add an internal context built from `SemanticSnapshot`:
  - function spans by line
  - parameter names and declaration spans per containing function
  - local declarations by name with declaration byte offset
  - globals by name and declaration span
  - struct fields by name and declaration span
  - cbuffer fields by name and declaration span
- Keep it private to `semantic_tokens.cpp`.
- Do not add new public snapshot fields unless a precise span cannot be
  recovered safely.

Recommended internal model:

```cpp
struct SemanticTokenClassificationContext {
  const SemanticSnapshot *snapshot = nullptr;
  // helper methods:
  // - findContainingFunction(line)
  // - isParameter(function, name)
  // - isParameterDeclaration(function, line, character, name)
  // - isVisibleLocal(function, name, byteOffset)
  // - isGlobal(name)
  // - isCBufferFieldReference(name)
  // - isKnownField(name)
  // - isFieldDeclaration(line, name)
};
```

Local offsets are usable because `ExpandedSource` is line-preserving and
inactive branches are replaced with spaces, preserving byte positions.

Acceptance:

- Classification helpers are read-only and deterministic.
- No semantic data is copied into client code.
- No new cross-module state owner is created.

### Phase 5: Classify Identifier Tokens

File:

- `server_cpp/src/semantic_tokens.cpp`

Tasks:

- Keep the existing lexical scan as the first pass.
- For identifiers that would otherwise be variable-like:
  - If token is preceded by direct `.`, classify as `property`.
  - Else if it belongs to the containing function's parameters, classify as
    `parameter`.
  - Else if it is a visible local declaration/reference, classify as `variable`.
  - Else if it is an unshadowed cbuffer field reference, classify as `property`.
  - Else if it is a global, classify as `variable`.
  - Else keep existing lexical fallback.
- Keep function-call and type classification precedence where it is already
  more specific than variable classification.

Declaration modifier:

- Local variable declaration:
  - match token byte start to `LocalInfo.offset`
  - add `declaration`
- Parameter declaration:
  - token span matches the shared parameter binding-name location
  - add `declaration`
- Global declaration:
  - token span matches the shared global declaration span
  - add `declaration`
- Struct field declaration:
  - token span matches the shared struct field declaration span
  - classify as `property + declaration`
- CBuffer field declaration:
  - token span matches the shared cbuffer field declaration span
  - classify as `property + declaration`
- CBuffer field reference:
  - bare cbuffer field references are field-like semantic facts even though
    HLSL exposes them without `.` syntax
  - classify unshadowed references as `property`
  - local / parameter shadowing wins over cbuffer field classification

Modification modifier:

- Direct assignment / compound assignment:
  - token is the last assignable identifier / property token on the left-hand
    side of:
    `=`, `+=`, `-=`, `*=`, `/=`, `%=`
  - examples:
    - `value += 1` marks `value`
    - `MaterialInputs.base_color = value` marks `base_color`, not
      `MaterialInputs`
  - add `modification`
- Increment / decrement:
  - postfix `x++`, `x--`
  - prefix `++x`, `--x`
  - add `modification`
- Member assignment:
  - `MaterialInputs.base_color = ...`
  - classify `base_color` as `property + modification`
  - base object remains parameter / variable without modification unless it is
    directly assigned.

Initial exclusions:

- Do not attempt to mark mutation through function calls such as
  `Foo(inoutValue)`.
- Do not attempt alias / reference analysis.
- Do not classify swizzle component writes beyond the existing property token
  behavior unless it naturally falls out of `.x =`.

Acceptance:

- Read-only references do not receive `modification`.
- Declaration and write modifiers can coexist only when a declaration includes
  initializer assignment and the implementation intentionally marks the
  declaration token as modified; default preferred behavior is declaration only.
- No duplicate semantic token spans are emitted.

### Phase 6: Tests

Files:

- `test_files/module_semantic_tokens.nsf` or a new focused `.nsf` fixture
- a new focused `.hlsl` fixture under `test_files/`
- `src/test/suite/integration/deferred-doc.ts`

Tasks:

- Add a decode helper for semantic tokens:
  - decode relative LSP token data into absolute line / character spans
  - map token type index to legend token type
  - decode modifier bits to modifier names
- Add focused assertions:
  - parameter declaration is `parameter + declaration`
  - parameter use is `parameter`
  - local declaration is `variable + declaration`
  - local assignment target is `variable + modification`
  - global declaration is `variable + declaration`
  - struct field declaration is `property + declaration`
  - cbuffer field declaration is `property + declaration`
  - cbuffer field reference is `property`
  - member assignment target is `property + modification`
  - declaration-side `label: Type name` marks the binding name as
    `parameter + declaration` only if the label-parser gate has been cleared
  - comment / string token types are still absent from semantic token output
- Add the same representative role assertions for a `.hlsl` document, not only
  an `.nsf` document.
- Add at least one active-unit / include-context `.hlsl` semantic-token test so
  the complete-coverage claim is proven under the same analysis context used by
  real shared include files.
- The active-unit / include-context `.hlsl` test must explicitly set an `.nsf`
  fixture as the active unit, request semantic tokens for an included `.hlsl`,
  and assert at least one token whose role depends on the `.nsf` unit's active
  macro/branch context.
- Add one check that forces a document to language id `hlsl` via
  `vscode.languages.setTextDocumentLanguage(...)`, not only a `.hlsl`
  extension mapped to the `nsf` language contribution.
- Add one assertion that the active-unit test setup still uses an `.nsf` URI;
  do not add tests that pin or select `.hlsl` as a unit.

Suggested fixture body:

```hlsl
float4 GlobalTint;

cbuffer SuiteCBuffer {
    float4 CBufferTint;
};

struct SuiteType {
    float4 color;
};

float4 SuiteFunc(float2 uv, inout PixelMaterialInputs material) : SV_Target0
{
    float value = 1.0f;
    value += uv.x;
    material.base_color = value.xxx;
    return SuiteType().color + GlobalTint + CBufferTint;
}
```

Add a separate NSF label-style case if the fixture grammar needs it:

```hlsl
float4 SuiteLabelFunc(UV: float2 uv) : SV_Target0
{
    return float4(uv, 0, 1);
}
```

Acceptance:

- Tests assert exact token type / modifier for representative spans.
- Tests do not rely on theme colors.
- Tests do not depend on real workspace file line numbers.
- Tests prove equivalent behavior for `.nsf` and `.hlsl` documents.
- Tests prove `.hlsl` behavior both for a standalone opened HLSL file and for a
  shared include file under `.nsf` active-unit analysis context.
- Tests prove the `{ language: 'hlsl' }` selector path without changing the
  `.hlsl` file-extension ownership or active-unit model.
- Tests cover declaration-side `label: Type name` binding-name classification
  only if the separate parser gate is explicitly cleared.

### Phase 7: Real Workspace Evidence

Use the same style of direct LSP semantic-token probe already used during
triage.

Targets:

```text
C:/Software/WorkTemp/G66ShaderDevelop/shader-source/base/building.nsf
At least one stable .hlsl file opened from the same shader-source workspace,
preferably one reached by the active building.nsf include closure
```

Choose and record the exact `.hlsl` path before implementation validation so
future reruns sample the same document instead of a moving target.

Probe configuration should mirror the extension's real workspace runtime:

- workspace folder: `C:/Software/WorkTemp/G66ShaderDevelop/shader-source`
- `nsf.intellisionPath`: include the shader-source root
- `nsf.shaderFileExtensions`: `.nsf`, `.hlsl`
- `nsf.semanticTokens.enabled`: `true`
- active unit: `building.nsf` or another explicitly recorded `.nsf` unit; never
  a `.hlsl` file
- preprocessor macro preset / `nsf.defines`: use the same values as the test
  workspace when available; if the probe intentionally omits them, record that
  limitation beside the sample table.

Expected samples:

```text
diffuse_color declaration  -> variable + declaration
diffuse_color assignment   -> variable + modification
MaterialInputs parameter   -> parameter
base_color member write    -> property + modification
CBuffer field declaration   -> property + declaration
CBuffer field reference     -> property
.hlsl parameter/local/global/property samples -> same role semantics as .nsf
p0 pass name               -> unchanged or explicit effect classification if
                              already supported by existing lexical path
```

Acceptance:

- The semantic legend includes `parameter`, `declaration`, and `modification`.
- Real workspace token samples show role and modifier changes in both `.nsf`
  and `.hlsl` files.
- The `.hlsl` sample table records whether the file was analyzed as a
  standalone current document or as part of the active `.nsf` include context.
- The probe confirms no `.hlsl` file was selected as the active unit.
- No diagnostics / completion / hover behavior is evaluated in this probe.

### Phase 8: Documentation

Files:

- `docs/architecture.md`
- `docs/type-method-interface-contract.md` if declaration-side
  `label: Type name` parsing is promoted into shared semantics
- touched `*.hpp` files as local interface contracts

Tasks:

- Update the semantic token module description from lexical-only wording to:
  - semantic tokens consume deferred semantic snapshot when available
  - comment and string coloring remains TextMate / editor grammar responsibility
  - semantic tokens provide variable role / declaration / modification signals
  - semantic token classification applies uniformly to `.nsf` and `.hlsl`
    documents handled by the extension
  - `.nsf` remains the only active/root unit; `.hlsl` semantic-token context is
    standalone or inherited from the selected `.nsf` include context
- If parameter label parsing changes the shared semantic contract, update
  `docs/type-method-interface-contract.md` to describe the supported
  declaration-side binding-name boundary.
- Do not update `docs/client-editor-features.md` unless grammar, language
  configuration, snippets, or manifest editor defaults change.
- Do not update `docs/resources.md` unless resources change.
- Do not update `docs/testing.md` unless the validation command matrix or test
  helper policy changes.

Acceptance:

- Current fact docs reflect the accepted runtime behavior after implementation.
- This human-ai plan remains historical execution context, not the final fact
  source.

## Validation Commands

Minimum validation for implementation:

```powershell
npm run compile
cmake --build .\server_cpp\build
npm run test:client:repo
npm run test:client:repo:m4
```

If C++ link fails with `permission denied`, first ensure any VS Code extension
host or test process holding `server_cpp\build\nsf_lsp.exe` has exited, then
rerun the build.

Recommended extra validation:

```powershell
$env:NSF_TEST_FILE_FILTER = "Deferred Doc Runtime / Semantic Tokens"
npm run test:client:repo
Remove-Item Env:\NSF_TEST_FILE_FILTER
```

If the implementation touches shared parameter parsing used by more than
semantic tokens, also rerun the same M4 set after the parser change:

```powershell
npm run test:client:repo:m4
```

This optimized plan is expected to touch active-unit-aware semantic snapshot
inputs for `.hlsl`, so `npm run test:client:repo:m4` is part of the minimum
implementation validation.

Then run the real-workspace semantic-token probe for both a `.nsf` file and a
`.hlsl` file, and record the resulting legend / sample token table in the final
implementation summary.

## Risk And Mitigation

Risk: theme differences make the visual result subjective.

Mitigation:

- Test the semantic token payload, not colors.
- Let user themes decide actual colors.

Risk: parameter or global declaration location is guessed by name / line.

Mitigation:

- Keep matching boundary-aware.
- Prefer exact declaration spans already emitted by the parser / AST layer.
- Add snapshot spans later only if repeated consumers need them.

Risk: `.hlsl` include-context snapshots survive after the selected `.nsf`
context changes.

Mitigation:

- Reuse `DocumentRuntime.analysisSnapshotKey` as the deferred snapshot owner
  key.
- Treat active-unit path, include-closure fingerprint, branch fingerprint,
  defines, preset fingerprint, include paths, shader extensions, workspace
  folders, workspace summary version, and document version / epoch as
  invalidation inputs.
- Do not add an independent semantic-token cache key that can diverge from the
  runtime key.

Risk: cbuffer field support creates a second field fact source.

Mitigation:

- Promote cbuffer fields into `SemanticSnapshot` and keep
  `semantic_tokens.cpp` consuming only that snapshot.

Risk: declaration-side `label: Type name` support could leak inconsistent
parameter parsing into other consumers.

Mitigation:

- Keep the label case in a separate gate.
- Confirm hover, completion, signature help, diagnostics, inlay hints,
  definition, references, and rename impact before changing the shared parser.
- Do not add a semantic-token-only parser fork.

Risk: write detection overreaches on complex expressions.

Mitigation:

- Start with direct assignment, compound assignment, and increment/decrement.
- Avoid inout call mutation and alias analysis in this task.

Risk: duplicate or overlapping tokens degrade VS Code rendering.

Mitigation:

- Keep one token per lexical span.
- Apply semantic override to the existing token before pushing it.

Risk: cached semantic-token artifacts survive with old legend semantics.

Mitigation:

- Legend changes are process-level; tests run after server restart/build.
- Runtime caches are document analysis-key scoped.
- No old/new dual behavior is retained.

Risk: `.hlsl` coverage accidentally expands the unit model.

Mitigation:

- Keep active-unit ownership in `.nsf`-only modules unchanged.
- Test `.hlsl` include-context coverage by setting an `.nsf` active unit and
  requesting tokens for the included `.hlsl`, not by selecting `.hlsl` as a
  unit.
- Add a final acceptance item that no `.hlsl` active/root unit path was added.

## Rollback Plan

If implementation causes broad semantic-token regressions:

1. Keep the test decode helper if useful.
2. Revert semantic-token classification changes.
3. Restore lexical-only builder calls.
4. Keep no feature flag or compatibility path.

If only modifier detection is unstable:

1. Keep `parameter` type classification.
2. Temporarily remove `modification` logic.
3. Re-run repo semantic token tests.
4. Reintroduce write modifiers only with tighter cases.

## Final Acceptance Checklist

- `parameter` appears in semantic token legend.
- `declaration` and `modification` appear in semantic token modifiers.
- Repo tests prove parameter / local / global / struct property / cbuffer
  property declaration and reference token payloads.
- Repo tests prove equivalent representative behavior for `.nsf` and `.hlsl`.
- Repo tests prove `.hlsl` behavior under `.nsf` active-unit / include-context
  analysis context.
- Repo tests prove the `{ language: 'hlsl' }` document-selector path without
  changing `.hlsl` files into active/root units.
- Repo tests cover declaration-side `label: Type name` binding-name
  classification only if the separate parser gate is explicitly cleared.
- Real workspace probes show variable role changes in both `building.nsf` and
  at least one `.hlsl` file.
- Real workspace probes record the selected `.nsf` active unit and confirm no
  `.hlsl` file was used as a unit.
- `npm run compile` passes.
- `cmake --build .\server_cpp\build` passes.
- `npm run test:client:repo` passes.
- `npm run test:client:repo:m4` passes because active-unit-aware `.hlsl`
  semantic snapshot inputs are part of the optimized plan.
- `docs/architecture.md` is updated after implementation.
- `docs/type-method-interface-contract.md` is updated if parameter label
  parsing becomes supported shared behavior.
- No client-side color decoration is added.
- No theme override is added.
- No language knowledge is duplicated in `client/`.

## Handoff Notes

Most relevant files:

- `server_cpp/src/semantic_tokens.hpp`
- `server_cpp/src/semantic_tokens.cpp`
- `server_cpp/src/deferred_doc_runtime.cpp`
- `server_cpp/src/hlsl_ast.hpp`
- `server_cpp/src/hlsl_ast.cpp`
- `server_cpp/src/semantic_cache.hpp`
- `server_cpp/src/semantic_snapshot.cpp`
- `src/test/suite/integration/deferred-doc.ts`
- `test_files/module_semantic_tokens.nsf`
- new focused `.hlsl` semantic-token fixture
- `docs/architecture.md`
- `docs/type-method-interface-contract.md` if shared parameter label support is
  promoted
- Do not touch `client_active_unit.*` for `.hlsl` unit support; `.nsf` remains
  the only unit.

Current root cause:

- Semantic tokens are lexical and do not expose variable role / modifier
  differences.

Preferred implementation:

- Add semantic-token role / modifier output in server.
- Reuse deferred semantic snapshot.
- Keep client as plain LSP consumer.
