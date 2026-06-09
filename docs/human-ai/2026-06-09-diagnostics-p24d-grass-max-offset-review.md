# P24D grass_max_offset Owner Review

## Scope

This review covers P24D from `docs/human-ai/2026-05-16-diagnostics-upgrade-execution-plan.md`.

Input baseline:

- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.{json,md}`
- Real workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`

P24D target in the 50-unit audit:

| Canonical group | Count | Representative file |
| --- | ---: | --- |
| `Undefined identifier: <symbol>.` | 49 | `shaderlib/foliage_anim_functions.hlsl`, `shaderlib/season_uniforms.hlsl` |

The P24D prompt names `grass_max_offset` as the representative symbol. In the P24A trend-50 audit, the undefined group contains 49 total diagnostics, with 5 unique `grass_max_offset` diagnostics and 44 `season_uniforms.hlsl` tail diagnostics that belong to P24F.

## Review Method

1. Aggregated P24A trend-50 undefined identifier samples by message, file, line, and active unit.
2. Checked `shaderlib/foliage_anim_functions.hlsl` around the reported `grass_max_offset` use.
3. Searched the real workspace source, shadercompiler output, generated JSON/CSV/config-like files, and shadercompiler directories for `grass_max_offset`.
4. Compared the nearby declared foliage animation parameters to confirm whether the symbol has an obvious local declaration pattern.
5. Checked whether the helper containing the use appears in inspected generated shadercompiler output.

## Evidence

P24A trend-50 undefined identifier split:

| Message | Count | File |
| --- | ---: | --- |
| `Undefined identifier: bottom_layer_color.` | 22 | `shaderlib/season_uniforms.hlsl` |
| `Undefined identifier: coverage_uv.` | 11 | `shaderlib/season_uniforms.hlsl` |
| `Undefined identifier: blend_mask.` | 11 | `shaderlib/season_uniforms.hlsl` |
| `Undefined identifier: grass_max_offset.` | 5 | `shaderlib/foliage_anim_functions.hlsl` |

`grass_max_offset` active units:

| Active unit | Count |
| --- | ---: |
| `base/animated_grass_noseason.nsf` | 1 |
| `base/animated_grass_specular_flower.nsf` | 1 |
| `base/animated_grass_specular_mask_billboard.nsf` | 1 |
| `base/animated_grass_specular_mask.nsf` | 1 |
| `base/animated_grass.nsf` | 1 |

Source evidence:

- `shaderlib/foliage_anim_functions.hlsl:579`: `float max_wind_offset = half(u_foliage_height) * foliage_scale * grass_max_offset;`
- The same file declares nearby material parameters with `u_` prefixes, including `u_grass_branch_max_offset`, `u_grass_leaf_max_offset`, `u_base_max_offset`, `u_branch_max_offset`, and `u_leaf_max_offset`.
- No declaration, `#define`, `#art`, profile macro, generated config, or shadercompiler output occurrence of `grass_max_offset` was found outside the single reported use.

Generated output evidence:

- `tmp_code_origin.hlsl` and `shadercompiler\tmp_code_dx11.hlsl` did not contain `grass_max_offset` or `FoliageAnim_IntersectGrass`.
- The reported use is inside `FoliageAnim_IntersectGrass(...)`, while the inspected animated grass paths call `CalcMeadowAnimWPO(...)` / `CalcVertexAnimationGrass(...)`; source search did not find an external call site for `FoliageAnim_IntersectGrass` in `shader-source`.

## Owner Table

| Group | Representative samples | Owner | Admitted for LSP implementation | Rationale |
| --- | --- | --- | --- | --- |
| `Undefined identifier: grass_max_offset.` | `foliage_anim_functions.hlsl:579` across 5 animated grass active units | Source / shadercompiler dead-code policy owner | No | The visible source contains an undeclared identifier. No uniform, macro, include, profile input, generated config, or shadercompiler output source was found for `grass_max_offset`. Nearby parameters use explicit declared `u_*_max_offset` names, so the evidence points to a source typo or stale helper body. The helper appears absent from inspected generated output, so shadercompiler acceptance is likely due to unused/dead-code output policy rather than an LSP semantic input gap. |

## Not Admitted

- No default value should be guessed for `grass_max_offset`.
- No workspace preset, profile macro, or resource entry should be added without a real source/provider.
- No file-specific or symbol-specific diagnostics allowlist should be added.
- No undefined-identifier suppress for unused helper functions should be added in P24D. That would be a broader public diagnostics policy change and needs a separate architecture decision.
- The remaining 44 undefined identifier diagnostics in `season_uniforms.hlsl` are not admitted under P24D; they are part of the existing P24F syntax / call-count tail.

## Admitted List

None for P24D.

P24D is therefore an owner review closure, not an implementation phase. The remaining external/source action is to decide whether `FoliageAnim_IntersectGrass(...)` should be fixed to use an existing declared parameter, receive a new declared parameter, be guarded/removed, or remain accepted as project dead-code policy.

## Validation

No product code was changed during this review. Validation was limited to evidence inspection:

- Parsed `real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.json` and confirmed the 49-diagnostic undefined identifier group split.
- Inspected real workspace source:
  - `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\shaderlib\foliage_anim_functions.hlsl:153-592`
  - `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\base\animated_grass.nsf`
  - `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\base\animated_grass_noseason.nsf`
- Searched real workspace source, shadercompiler output, generated JSON/CSV/config-like files, and shadercompiler directories for `grass_max_offset`.
- Checked that `tmp_code_origin.hlsl` and `shadercompiler\tmp_code_dx11.hlsl` do not contain `grass_max_offset` or `FoliageAnim_IntersectGrass`.

No build, repo integration, smoke audit, trend audit, or focused fixture was required because no implementation or public diagnostics behavior changed.
