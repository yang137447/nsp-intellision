# P19 Source / Config Diagnostics Review

本文记录 P19 对 post-P18 100-unit diagnostics audit 的 source / config / policy / LSP owner 分流。它是 `docs/human-ai/` 协作沉淀，不改变当前事实文档、产品代码或公开 diagnostics 行为。

## 输入

- Audit: `out/test/diagnostics-audit/real-workspace-diagnostics-audit.post-p18-source-config-batch-000-099.{json,md}`
- Workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`
- Scope: unit offset `0`，unit limit `100`
- Diagnostics mode: `balanced`
- Source inspection date: 2026-06-08

健康度摘要：

| Metric | Value |
| --- | ---: |
| Units discovered / scanned | 811 / 100 |
| Files scanned | 179 |
| Diagnostics total | 3552 |
| needs-manual-review | 3405 |
| likely-plugin-limitation | 147 |
| Undefined macro diagnostics | 0 |
| Synthesized-zero macro events | 0 |
| Truncated / timed-out / file errors | 0 / 0 / 0 |

P19 没有执行真实 shadercompiler 编译对照；下表的 owner 判断基于 audit 分组、代表样本和本地源码上下文。标记为 source / config / policy 的项需要对应源码或配置 owner 复核后处理，不能由 LSP suppress 或 fallback 吞掉。

## Owner Table

| Group | Count | Representative evidence | P19 owner | Compile check | P20? | Recommendation |
| --- | ---: | --- | --- | --- | --- | --- |
| `Duplicate local declaration: <symbol>.` | 1843 | `shaderlib/surface_functions.hlsl:536` 与后续 `COLOR_CHANGE_MODE` 分支重复声明 `COLOR_NUM`、`change_mask`、`color_gray` 等；`COLOR_CHANGE_*` 在不同 parameter include 中存在 0 / 非 0 policy 差异。 | config / policy | Pending | No | 由 material-family owner 确认 active unit 是否应该提供确定的 `COLOR_CHANGE_MODE` / enum 常量。P20 不应新增 suppress 或全局猜值。 |
| `Unreachable code.` | 940 | 样本分散在 `lighting_functions.hlsl`、`shadow.hlsl`、`monte_carlo.hlsl`、`indirect_lighting.hlsl`、`position.hlsl`、`function.hlsl`；部分样本是 `if/else return` 或预处理分支后的控制流。 | policy / possible LSP control-flow | Pending | No | 需要单独 control-flow/policy 阶段确认哪些 warning 应公开发布。量级较大，不作为 P20 focused tail。 |
| `Function call argument mismatch: GetVisibility.` | 261 | `shadow.hlsl:969` 定义 `GetVisibility(float y, float3 uvs)`，`t_terrain_heightmap_shadow` 是 `Texture2DArray`；调用处传 `uv.xy` / `float2 uv`。 | source | Pending | No | 源码侧确认是否应传 array slice / `float3`，或新增真实 overload。LSP 当前诊断与签名一致。 |
| `Potential missing return on some paths.` | 174 | `shadow.hlsl:437` 的 `InShadowMapRange` 和 `function.hlsl:814` 的 `CubeUV2OctahedronUV` 都有 `if/else` return。 | possible LSP control-flow / policy | Pending | No | 这是 control-flow 模型复核候选，但不应塞入 P20；建议另建 focused control-flow fixture 后再决定是否开启专门阶段。 |
| `Undefined identifier: <symbol>.` | 130 | `grass_max_offset` 全 workspace 仅在 `foliage_anim_functions.hlsl:579` 出现；`bottom_layer_color` 仅在 `season_uniforms.hlsl:1024/1033` 出现；`coverage_uv` / `blend_mask` 样本靠近 `if UP_FACING_MASK` 与后续缺分号区域。 | mixed source / syntax-policy | Pending | No | `grass_max_offset`、`bottom_layer_color` 优先交源码/配置 owner；`season_uniforms` 局部样本先修真实语法，再复跑 audit 判断是否仍有 LSP 缺口。 |
| `Assignment type mismatch: half4 = half3.` | 87 | `indirect_lighting.hlsl:11` 将三项 `half3` 累加赋给 `half4 env_diffuse`，随后返回 `half4(env_diffuse.xyz, 1.0h)`。 | source | Pending | No | 源码侧应确认是否改为 `half3 env_diffuse`。P20 不应放宽 vector width 赋值规则。 |
| `Duplicate global declaration: Init.` | 58 | `shading_models.hlsl:31` 与 `:53` 存在同签名 `void Init(...)`，外层没有互斥 `#if` 包住两个定义。 | source | Pending | No | 源码侧确认是否需要改名、条件化或删除重复定义。 |
| `Missing semicolon.` | 21 | `season_uniforms.hlsl:1021` 为 `height_bias.r = height_offset.r`，下一行继续赋值。 | source | Pending | No | 真实语法错误，源码侧补 `;` 后复跑相关 audit。 |
| `Function call argument count mismatch: SampleTexArryPkgNormalBias.` | 21 | `season_uniforms.hlsl:323` 定义 5 参数 `(tex, sam, float2 uv, float id, float bias)`；`:1032` 调用传 `(tex, sam, float3(...), sample_bias)` 共 4 参数。 | source / API usage | Pending | No | 源码侧确认是否拆成 `float2 uv` + `id`，或新增真实 4 参数 overload。 |
| `Indeterminate builtin call: isnan(float).` | 8 | `common_pipeline/format_conversion.hlsl` 使用 `isnan(v)` 清理 NaN。 | LSP shared builtin typing | Not needed | Yes | P20 focused fixture：`isnan(float)` 返回 `bool`，保留非法参数 sentinel。入口应是 `diagnostics_expression_type.*` common builtin typing。 |
| `Indeterminate builtin call: trunc(float).` | 8 | `common_pipeline/format_conversion.hlsl` 使用 `trunc(v * 127.f + ...)` 后显式 cast 到 `int` / `uint`。 | LSP shared builtin typing | Not needed | Yes | P20 focused fixture：`trunc(float)` 返回同型 numeric，保留非法参数 sentinel。入口应是 `diagnostics_expression_type.*` common builtin typing。 |
| `Function call argument mismatch: UVToWorld.` | 1 | `add_surfel_to_scene.nsf:29-34` 定义 `float4x4 inverse_trans[6]`；`:182` 传 `cur_mesh_info.inverse_trans[face_index]`，LSP 推断为 `float4` 而非 `float4x4`。 | LSP expression/member type | Not needed | Yes | P20 focused fixture：struct matrix array field indexed by variable should preserve element type `float4x4`。入口应在 semantic snapshot / member query / expression type 的共享类型链路修正。 |

## P20 Admission List

P19 确认可以进入 P20 的 focused LSP tail：

1. `isnan(float)` builtin typing。
   - Shared entry: `diagnostics_expression_type.*`
   - Fixture shape: legal `isnan(float)` / `isnan(floatN)` and invalid sentinel for non-numeric/object arguments.
   - Expected behavior change: remove `NSF_INDET_BUILTIN_UNMODELED` hint for legal forms only.

2. `trunc(float)` builtin typing。
   - Shared entry: `diagnostics_expression_type.*`
   - Fixture shape: legal scalar/vector numeric return type preservation and invalid sentinel.
   - Expected behavior change: remove `NSF_INDET_BUILTIN_UNMODELED` hint for legal forms only.

3. Struct matrix array field indexing type preservation.
   - Shared entry: semantic snapshot / member query / diagnostics expression type chain.
   - Fixture shape: `struct S { float4x4 m[6]; }; S s; acceptsMatrix(s.m[i]);` plus invalid sentinel proving `float4` still mismatches where expected.
   - Expected behavior change: remove the single `UVToWorld` false argument mismatch if the indexed field is truly `float4x4`.

Not admitted to P20:

- Source / config / policy groups above, including duplicate declarations, `GetVisibility`, `half4 = half3`, duplicate global `Init`, missing semicolon and `SampleTexArryPkgNormalBias` argument count.
- Control-flow groups (`Unreachable code` and `Potential missing return`) until a separate control-flow/policy stage defines expected public diagnostics behavior.
- `Undefined identifier` as a category. Individual samples should be revisited only after source syntax/config fixes remove known cascades.

## Closeout

- Commands changed: no.
- Paths or naming changed: added this review document only.
- Architecture or single source of truth changed: no.
- Test strategy changed: no.
- Public diagnostics behavior changed: no.
- New fallback / compat / shim / feature flag: no.
- Resource bundle/path/loading changed: no.
- Stable audit sample: post-P18 100-unit report remains the P19 input.
- Validation: document-only review; `git diff --check` passed with only the usual Windows CRLF warning.
