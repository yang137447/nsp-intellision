# P24 Source / Config Owner Handoff

本文是 P24 后剩余 diagnostics 的源码 / 配置 / policy 交接清单。它属于 `docs/human-ai/` 协作沉淀，不改变产品代码、测试入口、资源规则或公开 diagnostics 行为。

## 输入

- 主计划：`docs/human-ai/2026-05-16-diagnostics-upgrade-execution-plan.md`
- P24A implementation baseline：`out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.{json,md}`
- P24G validation baseline：`out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-24g-post-p24-trend-50.{json,md}`
- Real workspace：`C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`

P24 结论：

- P24A `modf` 是唯一 admitted LSP implementation，已完成。
- P24B-F 均已 review，admitted LSP implementation 为 None。
- P24G 复跑 5-unit / 50-unit audit，确认剩余统计未漂移。

## Handoff Summary

| Area | Count in P24G 50-unit | Owner | Requested action | Review doc |
| --- | ---: | --- | --- | --- |
| Duplicate local declarations in `surface_functions.hlsl` | 1290 | Material-family config / policy owner | 确认 affected units 是否应提供权威 `COLOR_CHANGE_MODE` / enum 常量值，或是否接受当前 default-zero 多分支 active 语义。 | `docs/human-ai/2026-06-09-diagnostics-p24b-duplicate-declaration-review.md` |
| Duplicate global `Init` in `shading_models.hlsl` | 43 | Source owner | 确认两个同签名 global `Init` 是否应重命名、条件编译或规范化。 | `docs/human-ai/2026-06-09-diagnostics-p24b-duplicate-declaration-review.md` |
| `GetVisibility(float, float3)` called with `float2` | 178 | Source / shadercompiler dead-code policy owner | 确认 unused helper 是否应修为 `float3`、删除、guard，或明确接受 dead-code policy。 | `docs/human-ai/2026-06-09-diagnostics-p24c-getvisibility-review.md` |
| `grass_max_offset` undefined | 5 | Source / shadercompiler dead-code policy owner | 确认是 typo、缺参数、应使用已有 `u_*_max_offset`，还是 unused helper dead-code policy。 | `docs/human-ai/2026-06-09-diagnostics-p24d-grass-max-offset-review.md` |
| `indirect_lighting.hlsl` `half4 = half3` | 50 | Source / shadercompiler dead-code policy owner | 确认局部变量应为 `half3`、RHS 应扩为 `half4`，还是该 helper 不进入真实 generated output。 | `docs/human-ai/2026-06-09-diagnostics-p24e-indirect-lighting-assignment-review.md` |
| `season_uniforms.hlsl` syntax / call-count / undefined tail | 66 | Source / API usage / syntax-policy owner | 清理真实 source-shaped remainder：缺分号、4 参数 `SampleTexArryPkgNormalBias` 调用、未声明 `bottom_layer_color` 和 syntax-cascade locals。 | `docs/human-ai/2026-06-09-diagnostics-p24f-season-uniforms-review.md` |

## Requested Owner Decisions

### Material-family Config / Policy

Open decision:

- 对 `COLOR_CHANGE_MODE == COLOR_CHANGE_PICKER / MULTIPLE / GRADIENT / CHANNEL_COLOR_CHANGE*` 这类分支，当前 active unit 是否应提供真实 material-family enum 值？

Evidence:

- P24B 50-unit duplicate local declarations 均来自 `shaderlib/surface_functions.hlsl` 的 `ChangeBaseColorSystem`。
- `COLOR_CHANGE_MODE` 已由 `#art` default-zero 提供。
- 右值 enum-like constants 属于 material-family 冲突值；没有真实 parameter include / generated config / profile / user config 时，LSP 只能按当前 conservative inputs 推导多个分支 active。

Do not ask LSP to:

- 猜非零 enum 真值。
- 把这些分支强行视为互斥。
- suppress duplicate local diagnostics by symbol / file.

### Source Owners

Open source actions:

- `shaderlib/shading_models.hlsl`: resolve duplicate global `Init(...)` signatures.
- `shaderlib/shadow.hlsl`: fix or retire `GetVisibility` helper calls that pass `float2` to a `float3` parameter.
- `shaderlib/foliage_anim_functions.hlsl`: resolve `grass_max_offset`.
- `shaderlib/indirect_lighting.hlsl`: align `env_diffuse` declaration with RHS shape or remove dead helper.
- `shaderlib/season_uniforms.hlsl`: fix missing semicolon, undefined locals / globals, and `SampleTexArryPkgNormalBias` arity.

Evidence:

- Review docs found no LSP signature discovery, expression typing, parser span, macro input, profile input, or resource bundle defect for these tails.
- Several samples appear absent from inspected generated shadercompiler outputs, so shadercompiler acceptance may be due to unused/dead-code output policy rather than source validity.

Do not ask LSP to:

- Add file / symbol / message suppressions.
- Add broad `float2 -> float3` or `half3 -> half4` compatibility.
- Guess undefined globals or macro defaults.
- Add dead-code based diagnostics suppression without a separate architecture / public behavior decision.

## Re-audit Guidance After Owner Fixes

After source / config owners change real workspace inputs, recommended validation:

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "50"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "3600000"
$env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL = "phase-25-post-owner-fix-trend-50"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

If the 50-unit trend shows new LSP-dominated groups after owner fixes, open a new focused phase with:

- owner review first,
- admitted list,
- focused fixture,
- 5-unit smoke,
- 50-unit trend,
- `npm run test:client:perf` if product diagnostics or hot path behavior changes.

## Current Residual Risk

- P24G did not run 100-unit or full audit. The 50-unit batch proves the reviewed P24B-F tails are stable in the sampled range, but files outside that range may contain additional source / policy tails.
- No current evidence admits another LSP implementation stage. A future P25 should start from post-owner-fix audit evidence, not from suppressing the known P24B-F groups.
