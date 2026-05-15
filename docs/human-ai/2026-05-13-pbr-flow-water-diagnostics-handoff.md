# pbr_flow_water diagnostics handoff

## Purpose

This is a continuation note for the `pbr_flow_water_nodes.hlsl` diagnostics triage. It summarizes what has already been changed in the repo and what a new thread should pick up next.

Primary triage source:

- `docs/human-ai/2026-05-12-pbr-flow-water-nodes-diagnostics-triage.md`

Target real-workspace file:

- `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\nodes\pbr_flow_water_nodes.hlsl`

Expected active unit:

- `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\pbr_flow_water.nsf`

## Completed

### Category 2: macro and preprocessor context gaps

Status: fixed for active-unit macro diagnostics.

Root cause:

- Active-unit prefix include replay compared include candidate URIs without
  lexical path normalization, so `./nodes/pbr_flow_water_nodes.hlsl` did not
  match the opened target URI.
- Shadercompiler builtin preprocessor macros such as
  `QUALITY_SUPPORT_MIDDLE`, `QUALITY_SUPPORT_HIGH`, and `PLAYERS_SELF` were not
  loaded into the LSP preprocessor model.

Implementation:

- Added shared URI/path identity helpers in `server_cpp/src/uri_utils.*` and
  used them from `preprocessor_view.*`, full diagnostics preprocessor setup,
  and immediate syntax diagnostics.
- Added `server_cpp/resources/language/preprocessor_macros/` with bundle
  schema/base/override files.
- Extended `language_registry.*` to load builtin preprocessor macros. The
  client now uses the server registry to prefill the workspace
  `nsf.preprocessorMacros` setting; `preprocessor_view.*` consumes that
  complete effective setting before `nsf.defines` and source directives.
- Included the new bundle in resource model hashing and
  `scripts/json/validate_resources.js`.

Regression:

- Added repo fixtures for active-unit `./` include macro capture.
- Added repo fixture for builtin `QUALITY_SUPPORT_*` / `PLAYERS_SELF`
  preprocessor evaluation.

Real target check:

- Replayed `pbr_flow_water_nodes.hlsl` with `pbr_flow_water.nsf` as active unit
  against `server_cpp\build\nsf_lsp.exe`.
- Latest replay result: 11 diagnostics total, 0
  `Undefined macro in preprocessor expression` diagnostics.
- Remaining diagnostics are the previously deferred type-compatibility items
  such as `half3 = float3`, scalar-to-vector splat, and related function
  argument mismatch cases.

### Category 5: duplicate diagnostic publishing

Status: fixed.

Root cause:

- diagnostics build / layered publish merge could carry the same diagnostic more than once.

Implementation:

- Added `dedupeDiagnosticsForUri(...)` in `server_cpp/src/diagnostics/diagnostics_emit.*`.
- Called it from `server_cpp/src/diagnostics/diagnostics.cpp` before build results are returned.
- Called it from `server_cpp/src/diagnostics_runtime.cpp` after local-structural / last-good publish merging.
- Dedupe key is document URI, range, message, code, and source.

Regression:

- `src/test/suite/integration/diagnostics.ts` asserts the published diagnostics payload has no duplicate stable keys.

Docs:

- `docs/architecture.md` records the diagnostics publish dedupe contract.
- The triage document marks Category 5 as fixed.

### Category 3: FX/NSF metadata global parameter extraction

Status: fixed for workspace-summary indexed definitions.

Root cause:

- workspace summary definition extraction only recognized metadata-block declarations through a generic branch that required at least three tokens.
- Real parameter declarations such as `float u_water_u_tile` followed by a top-level `< ... >` metadata block have only two tokens on the header line, so they were not indexed.

Implementation:

- `server_cpp/src/workspace/workspace_index_extract.cpp` now calls the shared `findMetadataDeclarationHeaderPosShared(...)` before the generic top-level definition pass.
- This keeps the fix in the shared workspace summary fact source instead of adding diagnostics-local `u_*` special cases.

Regression:

- `test_files/module_diagnostics_include_graph_provider.hlsl` adds `u_include_metadata_scale`.
- `test_files/module_diagnostics_include_graph_consumer.nsf` uses that symbol and diagnostics asserts it is not undefined.
- `test_files/module_definition_multiline_fx_decl_b.nsf` adds `u_multiline_fx_scale`.
- `test_files/module_definition_multiline_fx_decl_a.nsf` uses it and workspace-summary tests assert definition lookup resolves to the provider file.

Docs:

- `docs/architecture.md` records that `workspace_index_extract.*` covers FX/NSF metadata-block globals.
- The triage document marks Category 3 as fixed for workspace-summary indexed definitions.

## Validation Run

Commands already run and passed:

- `npm run json:validate`
- `npm run compile`
- `cmake --build .\server_cpp\build`
- `$env:NSF_TEST_FILE_FILTER='workspace-summary'; npm run test:client:repo`
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo`
- `npm run test:client:repo`
- JSON-RPC replay against the real target with active unit set to
  `pbr_flow_water.nsf`

## Remaining Work

Recommended next item depends on product priority:

- Category 3 and 5 are already addressed.
- Category 2 is now addressed for the active-unit target.
- Category 4 was explicitly put on hold by the user, so do not change HLSL half/float or scalar/vector compatibility rules yet.
- Category 1 has a product decision: keep the current behavior. Standalone
  include fragments depend on active unit context and should not get a new
  suppression, downgrade, or auto-selection policy from this triage.

Important constraint:

- Any further change to macro diagnostics, include-fragment diagnostics, or
  HLSL type compatibility changes published diagnostics behavior. Stop and
  confirm the intended behavior before implementation.

## Do Not Do Yet

- Do not implement Category 4 type compatibility rules unless the user re-approves it. They said to keep this behavior unchanged for now.
- Do not suppress or downgrade diagnostics for standalone include fragments from
  Category 1; the confirmed behavior is to rely on active unit context.
- Do not add compatibility layers, feature flags, fallback layouts, or diagnostics-local hardcoded language tables.

## Working Tree Notes

The repo already has many unrelated uncommitted changes. Do not revert them.

Relevant files touched by this triage work include:

- `server_cpp/src/uri_utils.*`
- `server_cpp/src/preprocessor_view.cpp`
- `server_cpp/src/language_registry.*`
- `server_cpp/src/document_runtime.cpp`
- `server_cpp/src/diagnostics/diagnostics_preprocessor.cpp`
- `server_cpp/src/immediate_syntax_diagnostics.cpp`
- `server_cpp/resources/language/preprocessor_macros/`
- `scripts/json/validate_resources.js`
- `server_cpp/src/diagnostics_emit.hpp`
- `server_cpp/src/diagnostics/diagnostics_emit.cpp`
- `server_cpp/src/diagnostics/diagnostics.cpp`
- `server_cpp/src/diagnostics_runtime.cpp`
- `server_cpp/src/workspace/workspace_index_extract.cpp`
- `src/test/suite/integration/diagnostics.ts`
- `src/test/suite/integration/workspace-summary.ts`
- `test_files/module_diagnostics_active_unit_dot_include_root.nsf`
- `test_files/module_diagnostics_active_unit_dot_include_prefix.hlsl`
- `test_files/module_diagnostics_active_unit_dot_include_target.hlsl`
- `test_files/module_diagnostics_preprocessor_builtin_macros.nsf`
- `test_files/module_diagnostics_include_graph_provider.hlsl`
- `test_files/module_diagnostics_include_graph_consumer.nsf`
- `test_files/module_definition_multiline_fx_decl_a.nsf`
- `test_files/module_definition_multiline_fx_decl_b.nsf`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`
- `docs/human-ai/2026-05-12-pbr-flow-water-nodes-diagnostics-triage.md`

Minimum context for a new thread:

1. Read `README.md`, `docs/architecture.md`, `docs/resources.md`, and `docs/testing.md`.
2. Read `docs/human-ai/2026-05-12-pbr-flow-water-nodes-diagnostics-triage.md`.
3. Read this handoff.
4. There is no approved Category 1 implementation work. Continue only with
   Category 4 if the user explicitly reopens HLSL type compatibility.
