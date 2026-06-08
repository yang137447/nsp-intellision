# P21 Control-flow / Diagnostics Policy Review

本文记录 P21 对 post-P20 final 50-unit diagnostics audit 中 control-flow 类 diagnostics 的分流。它是 `docs/human-ai/` 协作沉淀，不修改产品代码或公开 diagnostics 行为。

## 输入

- Audit: `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-20-focused-lsp-tail-trend-50-final.{json,md}`
- Workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`
- Scope: unit offset `0`，unit limit `50`
- Diagnostics mode: `balanced`
- Source inspection date: 2026-06-08

健康度摘要：

| Metric | Value |
| --- | ---: |
| Units discovered / scanned | 811 / 50 |
| Files scanned | 119 |
| Diagnostics total | 2229 |
| `Unreachable code.` | 525 |
| `Potential missing return on some paths.` | 100 |
| Wait timeouts / truncated / timed-out / file errors | 0 / 0 / 0 / 0 |

P21 没有执行真实 shadercompiler 编译对照；owner 判断基于 audit 分组、代表样本和本地源码上下文。控制流诊断属于公开 diagnostics 行为，后续实现前必须用 focused fixture 锁定合法 / 非法 sentinel，不能通过 suppress 或 allowlist 消除。

## Owner Table

| Group / sample | Count | Representative evidence | P21 owner | P22? | Recommendation |
| --- | ---: | --- | --- | --- | --- |
| `Potential missing return on some paths.` for `InShadowMapRange` | 50 sampled units include repeated hits; group total shares 100 with `CubeUV2OctahedronUV` | `shaderlib/shadow.hlsl:437-443` has `if (...) return true; else return false;`. All paths return. Current diagnostics also marks the `else` line unreachable. | LSP control-flow | Yes | Focused fixture for if/else where both arms return. Expected: no missing-return and no unreachable on the `else` arm. |
| `Potential missing return on some paths.` for `CubeUV2OctahedronUV` | 50 sampled units include repeated hits; group total shares 100 with `InShadowMapRange` | `shaderlib/function.hlsl:814-821` has `if (...) return ...; else return ...;`. All paths return. Current diagnostics marks `else` unreachable. | LSP control-flow | Yes | Same focused fixture should cover single-line return arms without braces and vector return values. |
| `Unreachable code.` after unbraced conditional return | 2+ representative samples | `shaderlib/atmosphere_common.hlsl:100-101` has `if (...) return false; return true;`; `shaderlib/position.hlsl:223-227` has `if (...) return ...;` followed by normal work. The following statement is reachable when condition is false. | LSP control-flow | Yes | Add focused fixture proving an unbraced conditional return only affects the `then` statement, not following sibling statements. |
| `Unreachable code.` inside `if/else` with runtime condition | 3+ representative samples | `shaderlib/monte_carlo.hlsl:564-574` returns in both arms; current diagnostics marks statements inside each arm unreachable. `shaderlib/indirect_lighting.hlsl:584-624` marks the `else` block after the `if (linear_sample)` branch returns, but `linear_sample` is a runtime parameter. | LSP control-flow | Yes | The control-flow state must fork/merge per branch instead of treating a return in one branch as poisoning sibling branches. |
| `Unreachable code.` in `#if/#else` block | 2 sampled hits | `shaderlib/lighting_functions.hlsl:486-492` reports the `#else` assignment. The branch is selected by `QUALITY_SUPPORT_SUPER_HIGH`, so a semantic unreachable diagnostic on an inactive branch is a preprocessor/active-branch publication issue, not source control flow. | LSP preprocessor/control-flow boundary | Yes, separate sentinel | Add a focused fixture with an active `#if/#else` inside an `if` block. Expected: diagnostics only for active code; inactive branch code should not receive semantic unreachable. |
| `Unreachable code.` in `season_uniforms.hlsl` | sampled singleton lines around `452`, `493`, `580`, `608`, `644`, `668`, `801`, `828`, `862`, `910`, `957` | P19 already found real syntax issues in `season_uniforms.hlsl` around missing semicolons and malformed calls; these unreachable samples are likely cascades from source syntax/recovery, not a clean P21 signal. | source / syntax-cascade | No | Do not admit until source syntax is fixed and a follow-up audit proves the warning remains in clean code. |

## P22 Admission List

P21 确认可以进入下一实现阶段的 focused LSP control-flow tail：

1. Branch-local `return` handling.
   - Shared entry: semantic diagnostics control-flow / block-flow helper under `diagnostics/*`.
   - Fixture shape: `if (cond) return a; else return b;` and braced equivalents.
   - Expected behavior change: remove false `Potential missing return` and false `Unreachable code` on the `else` arm.

2. Unbraced conditional return must not poison following sibling statements.
   - Shared entry: same control-flow helper.
   - Fixture shape: `if (cond) return a; statement; return b;`.
   - Expected behavior change: following statement remains reachable.

3. Runtime-condition branch merge.
   - Shared entry: same control-flow helper.
   - Fixture shape: `if (runtimeFlag) { return true; } else { statement; return false; }`.
   - Expected behavior change: return in one branch does not make sibling branch code unreachable.

4. Active-branch gating for semantic unreachable.
   - Shared entry: diagnostics prerequisites / active preprocessor mask consumed by block-flow diagnostics.
   - Fixture shape: active `#if/#else` inside an `if` block.
   - Expected behavior change: inactive branch code does not receive semantic unreachable.

Not admitted to P22:

- `season_uniforms.hlsl` unreachable samples until the known source syntax issues are fixed.
- Any source/config/policy groups already assigned by P19, including duplicate declarations, `GetVisibility`, assignment width mismatch, duplicate global `Init`, missing semicolon and argument count mismatch.
- Broad policy changes such as disabling `Unreachable code` or `Potential missing return` globally.

## Closeout

- Commands changed: no.
- Paths or naming changed: added this review document only.
- Architecture or single source of truth changed: no product architecture changed in P21 review; next implementation would touch public diagnostics behavior.
- Test strategy changed: no code/test change yet; P22 must add focused fixture before product changes.
- Public diagnostics behavior changed: no.
- New fallback / compat / shim / feature flag: no.
- Resource bundle/path/loading changed: no.
- Stable audit sample: `phase-20-focused-lsp-tail-trend-50-final`.
- Validation: document-only review; run `git diff --check` before closing.
