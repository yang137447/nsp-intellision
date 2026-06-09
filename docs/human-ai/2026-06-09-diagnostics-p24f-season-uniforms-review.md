# P24F Season Uniforms Syntax / Call-Count Review

本文记录 P24F 对 `shaderlib/season_uniforms.hlsl` syntax / call-count / undefined tail 的 owner 分流。它是 `docs/human-ai/` 协作沉淀，不改变当前事实文档、产品代码或公开 diagnostics 行为。

## 输入

- Audit: `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.{json,md}`
- Workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`
- Scope: unit offset `0`，unit limit `50`
- Diagnostics mode: `balanced`
- Source inspection date: 2026-06-09

P24F 没有执行真实 shadercompiler 编译对照；owner 判断基于 P24A trend-50 明细、既有 P15/P17/P19/P21 review 结论，以及本地源码 / generated output 搜索。

## Audit Split

P24A trend-50 中 `shaderlib/season_uniforms.hlsl` 共 `66` 条 diagnostics，集中在 11 个 active unit，每个 unit 6 条：

| Message | Count | Line | Owner |
| --- | ---: | --- | --- |
| `Undefined identifier: bottom_layer_color.` | 22 | `1024`, `1033` | Source owner |
| `Undefined identifier: coverage_uv.` | 11 | `999` | Source / syntax-cascade owner |
| `Undefined identifier: blend_mask.` | 11 | `999` | Source / syntax-cascade owner |
| `Missing semicolon.` | 11 | `1021` | Source owner |
| `Function call argument count mismatch: SampleTexArryPkgNormalBias. Expected 5 but got 4.` | 11 | `1032` | Source / API usage owner |

Affected active units:

| Active unit |
| --- |
| `base/animated_grass.nsf` |
| `base/animated_grass_specular_flower.nsf` |
| `base/animated_grass_specular_mask.nsf` |
| `base/animated_grass_specular_mask_billboard.nsf` |
| `base/blast.nsf` |
| `base/blast_external_lightmap_va.nsf` |
| `base/decal_bluetide_plane.nsf` |
| `base/dm51_bg_allround.nsf` |
| `base/external_lightmap.nsf` |
| `base/external_lightmap_va.nsf` |
| `base/furniture_exlightmap_va.nsf` |

## Owner Table

| Group | Count in P24A trend-50 | Representative evidence | Owner | Admitted for LSP implementation | Recommendation |
| --- | ---: | --- | --- | --- | --- |
| `Missing semicolon.` | 11 | `season_uniforms.hlsl:1021` is `height_bias.r = height_offset.r` and the next line continues with `height_bias.g = height_offset.g + mask.r;`. | Source owner | No | Source side should add the missing semicolon. P15 already reduced parser false positives to this real source-shaped remainder; P24F should not add parser suppress or recovery special cases for a genuinely unterminated statement. |
| `Function call argument count mismatch: SampleTexArryPkgNormalBias.` | 11 | `season_uniforms.hlsl:323` defines `SampleTexArryPkgNormalBias(Texture2DArray tex, sampler sam, float2 uv, float id, float bias)`, but `:1032` calls it as `SampleTexArryPkgNormalBias(..., float3(coverage_uv, array_id+1), sample_bias)` with 4 arguments. Source search found no 4-argument overload; generated output search found no replacement convention. | Source / API usage owner | No | Source side should either split the `float3` into `float2 uv` + `id` arguments or add a real overload if that is intended. LSP should keep reporting the current signature mismatch. |
| `Undefined identifier: bottom_layer_color.` | 22 | `bottom_layer_color` appears only at `season_uniforms.hlsl:1024` and `:1033`; no declaration, macro, include source, profile input, generated config, or shadercompiler output occurrence was found. | Source owner | No | Source side should provide a real declaration / parameter / sampled value or remove stale uses. P24F should not guess a global or macro default. |
| `Undefined identifier: coverage_uv.` / `blend_mask.` | 22 | Both symbols are declared earlier in `GetSeasonColorV2(...)`, but the surrounding region contains malformed source / syntax-policy evidence: `if season_factors.x > 0.0h` lacks parentheses, `if UP_FACING_MASK` is not a preprocessor directive, and `UP_FACING_MASK` has no discovered declaration in the searched source roots. P19 already marked these samples as local source / syntax-policy tail; P21 kept same-file unreachable samples out of implementation until the source syntax is fixed and rerun. | Source / syntax-cascade owner | No | Fix the real source syntax / macro policy first, then rerun audit. Only if these remain in a clean parser region should a later phase admit a shared parser/scope fix. P24F must not suppress undefined identifiers by symbol, file, or line. |

## Evidence

- P15A already drove `effect-syntax-or-macro Missing semicolon` to `0`; the remaining `syntax-structure Missing semicolon` samples were explicitly recorded as `season_uniforms.hlsl:1021` real source-shaped missing semicolons.
- P17/P19 already classified `SampleTexArryPkgNormalBias` 4/5 argument mismatch as source / API usage review: the only active definition takes 5 parameters and the reported call supplies 4.
- P19 classified `bottom_layer_color`, `coverage_uv`, and `blend_mask` under source / syntax-policy, with a recommendation to fix `season_uniforms.hlsl` source syntax before treating remaining undefined diagnostics as LSP-owned.
- P21 excluded `season_uniforms.hlsl` unreachable samples from control-flow implementation because the same region has known source syntax issues.
- Source search found `bottom_layer_color` only at the two reported uses.
- Source search found `SampleTexArryPkgNormalBias` definitions only with 5 parameters in `shaderlib/season_uniforms.hlsl` and `terrain/terrain_diffuse_common.hlsl`; the only active non-comment call in `season_uniforms.hlsl` passes 4 arguments.
- Generated output search under the real workspace `shadercompiler` folder did not find a generated replacement for `CoverageBlend`, `GetSeasonColorV2`, the missing semicolon line, `bottom_layer_color`, or a 4-argument `SampleTexArryPkgNormalBias` convention.

## Not Admitted

- No file-specific, symbol-specific, line-specific, or message-specific diagnostics suppress.
- No guessed declaration, macro preset, profile macro, resource entry, fallback overload, or compatibility shim.
- No parser recovery change for `height_bias.r = height_offset.r` without a focused clean-code fixture proving a parser boundary bug.
- No callsite compatibility rule that treats `(float3 uvAndId, bias)` as equivalent to `(float2 uv, id, bias)` without a real shared language/API contract.
- No undefined-identifier policy change for code inside source regions already known to be syntactically malformed or project-policy ambiguous.

## Admitted List

None for P24F.

P24F is therefore an owner review closure, not an implementation phase. The remaining external/source action is to fix or confirm the `season_uniforms.hlsl` source/API issues, then rerun a real diagnostics audit to see whether any clean LSP-owned parser/scope tail remains.

## Validation

No product code was changed during this review. Validation was limited to evidence inspection:

- Parsed `real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.json` and confirmed the 66-diagnostic `season_uniforms.hlsl` split by message, line, and active unit.
- Inspected real workspace source around:
  - `shaderlib/season_uniforms.hlsl:260-295`
  - `shaderlib/season_uniforms.hlsl:315-330`
  - `shaderlib/season_uniforms.hlsl:940-1045`
- Searched real workspace source and shadercompiler folders for `bottom_layer_color`, `SampleTexArryPkgNormalBias`, `CoverageBlend`, `GetSeasonColorV2`, `UP_FACING_MASK`, and generated equivalents.
- Reviewed P19 and P21 owner documents, plus the P15/P17/P19 execution records in the main diagnostics plan.

No build, repo integration, smoke audit, trend audit, or focused fixture was required because no implementation or public diagnostics behavior changed.
