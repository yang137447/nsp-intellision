# P24E Indirect Lighting Assignment Review

本文记录 P24E 对 `shaderlib/indirect_lighting.hlsl` assignment mismatch tail 的 owner 分流。它是 `docs/human-ai/` 协作沉淀，不改变当前事实文档、产品代码或公开 diagnostics 行为。

## 输入

- Audit: `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.{json,md}`
- Workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`
- Scope: unit offset `0`，unit limit `50`
- Diagnostics mode: `balanced`
- Source inspection date: 2026-06-09

P24E 没有执行真实 shadercompiler 编译对照；下表的 owner 判断基于 audit 分组、代表样本、既有 P17/P19 review 结论和本地源码 / generated output 上下文。

## Owner Table

| Group | Count in P24A trend-50 | Representative evidence | Owner | Admitted for LSP implementation | Recommendation |
| --- | ---: | --- | --- | --- | --- |
| `Assignment type mismatch: half4 = half3.` | 50 | `shaderlib/indirect_lighting.hlsl:11` 中 `half4 env_diffuse = square.x * half3(cube_data[normal_idx.x]) + ...`，右侧三项均为 `half3`，完整表达式类型为 `half3`；后续 `return half4(env_diffuse.xyz, 1.0h)` 也说明 `env_diffuse` 实际按三通道使用。 | Source / shadercompiler dead-code policy owner | No | 源码侧应确认是否将局部变量改为 `half3 env_diffuse`，或确认该 helper 是否属于当前生成路径外的 dead code。P24E 不应放宽 vector grow assignment，也不应按文件或 message suppress。 |

## Evidence

- P24A trend-50 中目标 canonical group 为 `50` 条，全部为 `Assignment type mismatch: half4 = half3.`，代表样本位于 `shaderlib/indirect_lighting.hlsl:11`，覆盖 50 个 active unit。
- 真实源码中 `CalcEnvCubeBasis(float3 world_normal, float4 cube_data[6])` 的 RHS 是三个 `half3(...)` 乘以 scalar 后相加，类型应保持 `half3`，不存在数组索引、constructor 或 macro-like alias 推断缺口。
- P17 已确认 `half4 = half3` 属于 source / type policy review：标准 HLSL 不支持 vector grow assignment，LSP 继续发布 assignment mismatch，audit triage 迁到 `needs-manual-review`。
- P19 source/config review 对同一模式给出相同结论：源码侧应确认是否改为 `half3 env_diffuse`，不应放宽 vector width 赋值规则。
- 搜索当前 generated outputs 时未发现 `CalcEnvCubeBasis` 或 `cube_data` 目标 helper；已生成的其他 `env_diffuse` 路径使用 `half3 env_diffuse`，与源码侧修正方向一致。

## Not Admitted

- 不新增 `half3 -> half4` 隐式扩维 conversion。当前共享 type relation 的边界是 scalar splat、同形 component-wise conversion 和 vector / matrix truncation；目标 shape 更大仍为 mismatch。
- 不按 `indirect_lighting.hlsl`、`env_diffuse`、行号或 message 增加 allowlist / suppress。
- 不把 assignment mismatch 全局降级为 warning；合法但有风险的 conversion 已由 `type_relation.*` 统一建模，找不到官方 conversion sequence 时继续发布 mismatch error。
- 不新增 dead-code based diagnostics suppress；这会改变公开 diagnostics policy，需要独立架构决策。

## Closeout

- Commands changed: no.
- Paths or naming changed: added this review document only.
- Architecture or single source of truth changed: no.
- Test strategy changed: no.
- Public diagnostics behavior changed: no.
- New fallback / compat / shim / feature flag: no.
- Resource bundle/path/loading changed: no.
- Stable audit sample: P24A trend-50 report remains the input.
- Validation: document-only review; no build or integration test was run because product code and public behavior were unchanged.
