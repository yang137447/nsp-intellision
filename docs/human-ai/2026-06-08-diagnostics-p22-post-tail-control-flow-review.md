# P22 Post-tail Control-flow Diagnostics Review

本文记录 P22 后对 `phase-22-control-flow-tail-trend-50` real diagnostics audit 中剩余 control-flow 类 diagnostics 的分流。它是 `docs/human-ai/` 协作沉淀，不修改产品代码或公开 diagnostics 行为。

## 输入

- Audit: `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-22-control-flow-tail-trend-50.{json,md}`
- Workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`
- Scope: unit offset `0`，unit limit `50`
- Diagnostics mode: `balanced`
- Source inspection date: 2026-06-08

健康度摘要：

| Metric | Value |
| --- | ---: |
| Units discovered / scanned | 811 / 50 |
| Files scanned | 119 |
| Diagnostics total | 1804 |
| `Unreachable code.` | 50 |
| `Potential missing return on some paths.` | 50 |
| Wait timeouts / truncated / timed-out / file errors | 0 / 0 / 0 / 0 |

P22 后 control-flow 剩余已经收敛为两个共享 include 样本，各自随 50 个 scanned units 重复计数。本 review 没有执行真实 shadercompiler 编译对照；owner 判断基于 audit 分组、代表样本和本地源码上下文。若进入实现，仍必须先加 focused fixture，不能通过 suppress 或 allowlist 消除。

## Owner Table

| Group / sample | Count | Representative evidence | Owner | P23? | Recommendation |
| --- | ---: | --- | --- | --- | --- |
| `Potential missing return on some paths.` for `IsRaySphereIntersect` | 50 | `shaderlib/atmosphere_common.hlsl:97-102` has `if (all(sol < 0.0f)) return false; return true;`. The final unconditional `return true;` covers the false branch. | LSP control-flow | Yes | Add focused fixture for same-line conditional early return followed by an unconditional final return. Expected: no potential missing-return. Investigate why the current top-level return state still remains conditional in audit. |
| `Unreachable code.` after unbraced `if` body separated by a comment line | 50 | `shaderlib/position.hlsl:223-227` has `if(any(...))`, then a comment-only line, then `return -0.000001f;`, followed by `float depth_ndc;`. The return is controlled by the `if`; the following declaration is reachable. | LSP control-flow | Yes | Add focused fixture where an unbraced `if` controls a return after intervening blank/comment trivia. Expected: return remains branch-local and does not poison following sibling statements. |

Not admitted to P23 from this control-flow review:

- Non-control-flow groups in the same audit, including duplicate local/global declarations, `GetVisibility` argument mismatch, `modf` indeterminate builtin modeling, `grass_max_offset` unresolved symbol, `season_uniforms.hlsl` syntax / call-count issues, and `indirect_lighting.hlsl` assignment mismatch. These require separate owner review if prioritized.
- 100-unit batch or full real audit expansion. Those are validation scope choices, not direct implementation admissions from this 50-unit review.
- Broad policy changes such as disabling `Unreachable code` or `Potential missing return` globally.

## P23 Admission List

P22 后可以进入下一 focused implementation 阶段的 LSP control-flow tail：

1. Conditional early return followed by unconditional final return.
   - Shared entry: semantic diagnostics control-flow / block-flow helper under `diagnostics/*`.
   - Fixture shape: `if (cond) return a; return b;`.
   - Expected behavior change: no `Potential missing return on some paths.`.
   - Sentinel: a function with only `if (cond) return a;` and no final return must still publish potential missing-return.

2. Unbraced `if` body with intervening trivia before `return`.
   - Shared entry: same control-flow helper; the immediate controlled statement lookup must skip blank/comment-only trivia.
   - Fixture shape: `if (cond)` followed by a comment-only or blank line, then `return a;`, then a reachable sibling statement.
   - Expected behavior change: no false `Unreachable code.` on the sibling statement.
   - Sentinel: a truly sequential `return a; statement;` must still publish unreachable.

Implementation constraints:

- Do not add diagnostic suppressions, message allowlists, compatibility layers, fallback paths or feature flags.
- Keep the fix in shared control-flow state, not in source-path-specific or message-specific branches.
- Preserve P22 behavior: branch-local returns must not leak to sibling branches, and inactive preprocessor branches must not publish semantic unreachable.

## Closeout

- Commands changed: no.
- Paths or naming changed: added this review document only.
- Architecture or single source of truth changed: no product architecture changed in this review; P23 implementation would touch public diagnostics behavior.
- Test strategy changed: no code/test change yet; P23 must add focused fixture before product changes.
- Public diagnostics behavior changed: no.
- New fallback / compat / shim / feature flag: no.
- Resource bundle/path/loading changed: no.
- Stable audit sample: `phase-22-control-flow-tail-trend-50`.
- Validation: document-only review; run `git diff --check` before closing.
