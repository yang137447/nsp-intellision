# P24B Duplicate Declaration Owner Review

## Scope

This review covers P24B from `docs/human-ai/2026-05-16-diagnostics-upgrade-execution-plan.md`.

Input baseline:

- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.{json,md}`
- Real workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`

P24B target groups in the 50-unit audit:

| Canonical group | Count | Representative file |
| --- | ---: | --- |
| `Duplicate local declaration: <symbol>.` | 1290 | `shaderlib/surface_functions.hlsl` |
| `Duplicate global declaration: <symbol>.` | 43 | `shaderlib/shading_models.hlsl` |

## Review Method

1. Aggregated P24A 50-unit audit groups and samples.
2. Checked representative real source around `surface_functions.hlsl:522-708` and `shading_models.hlsl:31-53`.
3. Cross-checked existing P14/P19 records for `COLOR_CHANGE_MODE` and `COLOR_CHANGE_*` ownership.
4. Verified audit macro focus evidence for the 50-unit run: `COLOR_CHANGE_MODE`, `EMISSIVE_MODE`, and `FOLIAGE_MODE` have no remaining undefined diagnostics and are resolved through `art-default-zero`; `undefinedMacroDiagnosticCount=0`.

## Owner Table

| Group | Representative samples | Owner | Admitted for LSP implementation | Rationale |
| --- | --- | --- | --- | --- |
| `Duplicate local declaration: <symbol>.` | `surface_functions.hlsl:570` `COLOR_NUM`, `:577` `change_mask`, `:578` `idx`, `:593` `color_array`, `:630` `color_gray` | Material-family config / policy owner | No | The duplicate locals sit in separate `#if COLOR_CHANGE_MODE == ...` branches inside `ChangeBaseColorSystem`. The selector `COLOR_CHANGE_MODE` is available as `#art` default zero, while right-side enum-like constants such as `COLOR_CHANGE_PICKER`, `COLOR_CHANGE_MULTIPLE`, `COLOR_CHANGE_GRADIENT`, `CHANNEL_COLOR_CHANGE*` are documented as material-family conflict values and currently default to conservative legacy `0` when no authoritative source/profile/config input is available. With the current effective macro environment, multiple branches are actually active from the LSP's point of view; treating them as mutually exclusive would require guessing project enum semantics. |
| `Duplicate global declaration: Init.` | `shading_models.hlsl:31` and `:53` | Source owner | No | `shading_models.hlsl` contains two global `void Init(FGBufferData, inout PixelData, inout BxDFContext, half3, half3, half3)` definitions with identical signatures. The duplicate definition itself is not guarded by mutually exclusive outer preprocessor conditions; only statements inside the bodies are conditional. This is a source/policy decision, not an include de-duplication, branch signature, macro local, or parser recovery defect. |

## Not Admitted

- No duplicate declaration suppress, allowlist, symbol-specific exception, or file-specific exception should be added.
- No branch-signature heuristic should assume `COLOR_CHANGE_MODE == COLOR_CHANGE_PICKER` and `COLOR_CHANGE_MODE == COLOR_CHANGE_GRADIENT` are mutually exclusive when the effective macro inputs currently make both sides evaluate to `0 == 0`.
- No global default should be added for conflicting material-family enum constants. P14 already recorded that these constants must come from a real parameter include, generated config, active unit profile, `nsf.preprocessorMacros`, or `nsf.defines`.
- No implementation change is admitted for `Duplicate global declaration: Init.` unless a future source review proves a missing active-branch/include-context contract.

## Admitted List

None for P24B.

P24B is therefore an owner review closure, not an implementation phase. The remaining action is external/source ownership:

- Material-family owner: confirm whether each affected active unit should provide authoritative `COLOR_CHANGE_MODE` and enum constant values, or whether the current default-zero compile behavior is intended.
- Source owner: confirm whether duplicate global `Init` should be renamed, conditionally compiled, or otherwise normalized.

## Validation

No product code was changed during this review. Validation was limited to evidence inspection:

- Parsed `real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.json`.
- Inspected representative real workspace source snippets:
  - `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\shaderlib\surface_functions.hlsl:522-708`
  - `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\shaderlib\shading_models.hlsl:31-53`
- Checked existing facts in `docs/human-ai/2026-06-08-diagnostics-p19-source-config-review.md`, `docs/resources.md`, and the P14 execution records in the main diagnostics plan.

No build, repo integration, smoke audit, or trend audit was required because no implementation or public diagnostics behavior changed.
