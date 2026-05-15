# pbr_flow_water_nodes.hlsl Diagnostics Triage

## Scope

- Date: 2026-05-12
- Target file: `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\nodes\pbr_flow_water_nodes.hlsl`
- Expected active unit: `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\pbr_flow_water.nsf`
- LSP server used: `D:\YYBWorkSpace\GitHub\nsp-intellision\server_cpp\build\nsf_lsp.exe`
- Settings used:
  - `nsf.intellisionPath = ["C:\Software\WorkTemp\G66ShaderDevelop\shader-source"]`
  - `nsf.shaderFileExtensions = [".nsf", ".hlsl", ".fx", ".usf", ".ush"]`
  - `nsf.diagnostics.mode = "balanced"`

This document records the diagnostics observed when the target include fragment is opened in the real workspace. It is a triage note for discussion, not a promoted current-fact document.

## Capture Summary

Original capture before the Category 2 / 3 / 5 fixes:

| Scenario | Diagnostics | Unique diagnostics | Initial conclusion |
| --- | ---: | ---: | --- |
| Open target file without active unit | 109 | not deduped | Expected noisy context: the file is an include fragment, not a standalone shader unit. |
| Open target file with `sfx/pbr_flow_water.nsf` as active unit | 71 | 63 | Still noisy; remaining diagnostics are mostly LSP modeling gaps. |

Latest replay after Category 2 / 3 / 5 fixes:

| Scenario | Diagnostics | Macro diagnostics | Current conclusion |
| --- | ---: | ---: | --- |
| Open target file with `sfx/pbr_flow_water.nsf` as active unit | 11 | 0 | Macro/include-context false positives are fixed; remaining items are the deferred HLSL type-compatibility category. |

No diagnostic in the active-unit capture was confirmed as a real source error. The generated/compiled `pbr_flow_water` outputs exist under `C:\Software\WorkTemp\G66ShaderDevelop\_shader_compile_out\` and `C:\Software\WorkTemp\G66ShaderDevelop\out\test\...`, which is additional evidence that the file is compiler-accepted in the intended pipeline.

## Category 1: Include Fragment Opened Without Context

Status: product decision is to keep the current behavior. Standalone include
fragments rely on an explicitly selected active unit for full compile context;
do not add diagnostics suppression, downgrade, or broader auto-selection from
this triage.

### Symptoms

- Opening `sfx\nodes\pbr_flow_water_nodes.hlsl` directly without an active unit produces 109 diagnostics.
- Setting active unit to `sfx\pbr_flow_water.nsf` reduces this to 71 diagnostics.

### Classification

Normal editor-context issue, not a source error.

### Root Cause

The target file is included by `sfx\pbr_flow_water.nsf` after several required context includes:

- `../shaderlib/common.hlsl`
- `./nodes/pbr_flow_water_parameters.hlsl`
- multiple `shaderlib/*` helper files

When opened as a standalone `.hlsl`, the language server cannot infer the full compile unit unless the active unit is known and correctly applied.

### Decision

- Keep the active-unit dependency as the intended behavior.
- Do not suppress or downgrade diagnostics for include fragments opened without
  active unit context.
- Do not expand client auto-selection policy for `nodes/*.hlsl` files as part
  of this triage.

## Category 2: Macro And Preprocessor Context Gaps

Status: fixed for active-unit macro diagnostics on `pbr_flow_water_nodes.hlsl`.

### Symptoms

22 diagnostics in the active-unit capture:

- `Undefined macro in preprocessor expression: HAS_TWO_SIDE.`
- `Undefined macro in preprocessor expression: QUALITY_SUPPORT_MIDDLE.`
- `Undefined macro in preprocessor expression: HAS_AMBIENT_OCCLUSION.`
- `Undefined macro in preprocessor expression: NORMAL_MAP_SUPPORT.`
- `Undefined macro in preprocessor expression: USE_FOAM.`
- similar `USE_*` art-toggle macros.

Examples:

- `pbr_flow_water_nodes.hlsl:40` -> `#if HAS_TWO_SIDE`
- `pbr_flow_water_nodes.hlsl:79` -> `#if QUALITY_SUPPORT_MIDDLE`
- `pbr_flow_water_nodes.hlsl:336` -> `#if USE_FOAM`

### Classification

LSP capability limitation, not a source error.

### Evidence

- `HAS_TWO_SIDE` is defined in `sfx\nodes\pbr_flow_water_parameters.hlsl`.
- `PBR_PARAM_TEX`, `USE_FOAM`, `USE_MAIN_DISTORT`, `USE_MASK01`, `USE_SOFTPARTICLE`, etc. are `#art` / `#ifndef` / `#define` macros in `pbr_flow_water_parameters.hlsl`.
- `QUALITY_SUPPORT_MIDDLE`, `QUALITY_SUPPORT_HIGH`, and `PLAYERS_SELF` are shadercompiler builtin macros recorded in `shadercompiler\data\builtin_macros.py`.
- `NORMAL_MAP_SUPPORT` and `HAS_AMBIENT_OCCLUSION` are derived from the above macro layers.

### Likely Root Causes

- Active-unit prefix include replay existed, but URI comparison did not
  lexically normalize `.` segments. The active unit includes the target through
  `./nodes/pbr_flow_water_nodes.hlsl`, while the opened document URI has no
  `./`, so the target include was not recognized and preceding macros from
  `pbr_flow_water_parameters.hlsl` were not captured.
- Shadercompiler builtin macro facts such as `QUALITY_SUPPORT_MIDDLE`,
  `QUALITY_SUPPORT_HIGH`, and `PLAYERS_SELF` were not part of the LSP
  preprocessor model.

### Implementation

- `uri_utils.*` now exposes shared URI/path comparison that decodes file URIs,
  lexically normalizes paths, normalizes separators, and lower-cases keys.
- `preprocessor_view.*`, full diagnostics preprocessor setup, and immediate
  syntax diagnostics all use the shared comparison path.
- Added `server_cpp/resources/language/preprocessor_macros/` as the shared
  resource bundle for shadercompiler-style object-like builtin preprocessor
  macros.
- `language_registry.*` loads the new bundle and exposes it to the client for
  first-run workspace `nsf.preprocessorMacros` prefill. `preprocessor_view.*`
  consumes that complete effective setting before settings defines and in-file
  directives, so normal override order is preserved without hidden preset
  layering.

### Regression

- `module_diagnostics_active_unit_dot_include_*` verifies active-unit prefix
  macros are captured when the target include path contains `./`.
- `module_diagnostics_preprocessor_builtin_macros.nsf` verifies builtin
  preprocessor macros do not produce undefined macro diagnostics.
- Real target replay with `pbr_flow_water.nsf` as active unit now reports 0
  `Undefined macro in preprocessor expression` diagnostics for
  `pbr_flow_water_nodes.hlsl`.

## Category 3: Global Parameter Declaration Extraction Gaps

### Symptoms

31 active-unit diagnostics are `Undefined identifier`, for example:

- `u_foam_sss`
- `u_water_u_tile`
- `u_water_v_tile`
- `u_water_u_speed`
- `u_maintex_tilling_x`
- `u_maintex_offset_x`
- `u_maintex_clamp`
- `u_overall_opacity`

### Classification

LSP capability limitation, not a source error. Fixed for the workspace-summary
path by indexing two-token FX/NSF metadata-block global declarations such as
`float u_name` followed by a top-level `< ... >` metadata block.

### Evidence

These identifiers are declared in `sfx\nodes\pbr_flow_water_parameters.hlsl`, for example:

- `u_water_u_tile`
- `u_maintex_tilling_x`
- `u_foam_sss`
- `u_overall_opacity`

The declarations use NSF/FX-style metadata annotations:

```hlsl
float u_water_u_tile
<
    string SasUiGroup = "...";
    ...
> = 1.0f;
```

This is legal in the project shader pipeline but easy for a line-oriented declaration extractor to miss.

### Likely Root Causes

- Workspace summary extraction did not fully understand multi-line FX annotation
  global variables whose header line has only type and name.
- Include-fragment diagnostics can see some shared global context, but not enough annotated parameter declarations from the preceding parameter file.
- Some globals from `shaderlib/common.hlsl`, such as built-in uniforms, are resolved after setting active unit, which indicates the include context is partially working; the remaining issue is concentrated around annotated NSF/FX parameter declarations.

### Discussion Points

- Workspace summary now uses the shared metadata declaration parser before the
  generic top-level definition pass, so diagnostics and definition lookup can
  consume the same indexed definition fact.
- Texture and sampler declarations with metadata blocks continue to use the
  same shared header path / FX block handling.
- Added repo fixtures for include-graph diagnostics and workspace-summary
  definition lookup using `pbr_flow_water_parameters.hlsl`-style declarations.

## Category 4: HLSL Type Compatibility Is Too Strict

### Symptoms

17 active-unit diagnostics are type compatibility complaints:

- 12 x `Assignment type mismatch: half3 = float3.`
- 4 x `Assignment type mismatch: float2 = float.`
- 1 x `Return type mismatch: expected half3 but got half.`
- 1 x `Function call argument mismatch: GetLightMultiplier. Expected: (half3, half, half3). Got: (half3, half, half).`

The count above includes duplicates in the raw publish stream; unique items are fewer.

Examples:

- `half3 L = -u_dir_direction.xyz;`
- `half3 LightColor = u_dir_color.rgb;`
- `float2 distort01_offset = 0.0;`
- `return 0.0h;` from a `half3` function.
- `GetLightMultiplier(dir_light_color, 1.0h, transmission_shadow)` where the third argument is scalar but the signature expects `half3`.

### Classification

LSP capability limitation, not a source error.

### Likely Root Causes

- Diagnostics expression type compatibility does not fully model HLSL scalar-to-vector splat conversion.
- Diagnostics treats `half` / `float` family conversions too strictly.
- Function argument matching does not accept scalar values where HLSL permits vector splat.

### Discussion Points

- Add shared HLSL conversion compatibility rules for:
  - scalar -> vector splat
  - half/float family conversion
  - same-width numeric vector family conversion
- Keep these rules in shared diagnostics/type compatibility helpers, not request handlers.
- Decide whether these should be warnings, hidden, or accepted as compatible.

## Category 5: Duplicate Diagnostic Publishing

### Symptoms

- Active-unit capture has 71 diagnostics but 63 unique diagnostics.
- Exact duplicates appeared for several type mismatch diagnostics, especially `half3 = float3` and `float2 = float`.

### Classification

LSP diagnostics pipeline issue. Fixed in the repo by adding central diagnostics
payload dedupe before diagnostics results are returned/published.

### Likely Root Causes

- Fast and full diagnostics can publish overlapping semantic diagnostics.
- The current merge/publish path does not dedupe by stable key before publication.

### Discussion Points

- Implemented dedupe by URI, range, message, code, and source in the shared
  diagnostics emit helper.
- The diagnostics facade dedupes build results, and the diagnostics runtime
  dedupes after last-good / local-structural publish merging.
- Intentionally separate diagnostics on the same range must use different
  code/source to remain distinguishable.

## Category 6: Potential Real Source Issues

### Current Status

No confirmed real source error in this target-file capture.

Items that look suspicious at first glance, such as scalar return from a vector function or scalar shadow argument to `GetLightMultiplier`, are accepted by HLSL-style scalar splat rules and are present in generated compiler outputs.

### Discussion Points

- If we want compiler-level confirmation, run the real shadercompiler on `sfx\pbr_flow_water.nsf`.
- Treat this as lower priority than fixing LSP false positives unless the real compiler reports an error.

## Suggested Discussion Order

1. Include-fragment active unit behavior: decided to keep current active-unit dependency.
2. Macro model: fixed for active-unit prefix include replay and builtin macro resources.
3. Annotated global parameter extraction: fixed for workspace-summary indexed
   definitions of two-token metadata-block global parameters.
4. HLSL type compatibility: add scalar/vector and half/float implicit conversion rules.
5. Diagnostics dedupe: fixed; repeated diagnostics are removed by the shared
   diagnostics payload dedupe path.

## Closure Checklist For Future Fixes

Any implementation that changes these diagnostics must re-check:

- Commands changed: no expected.
- Paths or naming changed: no expected.
- Architecture / single source of truth changed: yes if shadercompiler builtin macros become resources.
- Testing strategy changed: likely yes if new real-workspace or fixture regression is added.
- Docs updated: update `docs/architecture.md`, `docs/resources.md`, `docs/testing.md`, or `docs/type-method-interface-contract.md` only if the corresponding contract changes.
