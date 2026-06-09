# P24C GetVisibility Owner Review

## Scope

This review covers P24C from `docs/human-ai/2026-05-16-diagnostics-upgrade-execution-plan.md`.

Input baseline:

- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.{json,md}`
- Real workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`

P24C target group in the 50-unit audit:

| Canonical group | Count | Representative file |
| --- | ---: | --- |
| `Function call argument mismatch: GetVisibility. Expected: (float, float3). Got: (float, float2).` | 178 | `shaderlib/shadow.hlsl` |

## Review Method

1. Aggregated P24A 50-unit audit samples containing `GetVisibility`.
2. Checked representative source around `shaderlib/shadow.hlsl:969-1132`.
3. Confirmed the visible source signature and all reported call sites.
4. Cross-checked generated shadercompiler output in `tmp_code_origin.hlsl` and `shadercompiler\tmp_code_dx11.hlsl`.
5. Checked source references for the helper functions that contain the mismatching calls.

## Evidence

Source signature:

- `shaderlib/shadow.hlsl:969`: `half GetVisibility(float y,float3 uvs)`

P24A trend-50 reported call sites:

| Source line | Count | Units | Call shape |
| ---: | ---: | ---: | --- |
| `shadow.hlsl:1050` | 60 | 50 | `GetVisibility(y, uv.xy)` inside `GetVisibilityPcf(...)` |
| `shadow.hlsl:1071` | 60 | 50 | `GetVisibility(pos_world.y, uv)` inside `GetHeightMapShadowPcfDisk(...)` |
| `shadow.hlsl:1091` | 58 | 50 | `GetVisibility(pos_world.y, uvvis)` inside `GetHeightMapShadowPcfDiskOneSample(...)` |

The LSP signature discovery and expression typing are consistent with source:

- Expected parameter type is `float3`.
- Actual argument type is `float2`.
- No alternate visible `GetVisibility(float, float2)` overload was found in `shader-source`.
- No macro-generated `GetVisibility` signature was found for these samples.

Shadercompiler output evidence:

- `tmp_code_origin.hlsl` and `shadercompiler\tmp_code_dx11.hlsl` keep `GetVisibility(float, float3)` and the active `GetHeightMapShadow(...)` path that calls it with a `float3 uvs`.
- The helper functions containing the reported `float2` calls are not present in the inspected generated output.
- Source search found definitions for `GetVisibilityPcf(...)`, `GetHeightMapShadowPcfDisk(...)`, `GetHeightMapShadowPcfDiskOneSample(...)`, and `GetHeightMapShadowPcf(...)` only in `shaderlib/shadow.hlsl`; no external caller in `shader-source` was found for the first three helper functions.

## Owner Table

| Group | Representative samples | Owner | Admitted for LSP implementation | Rationale |
| --- | --- | --- | --- | --- |
| `Function call argument mismatch: GetVisibility.` | `shadow.hlsl:1050`, `:1071`, `:1091` | Source / shadercompiler dead-code policy owner | No | The visible source really declares `GetVisibility(float, float3)` while the reported helper calls pass `float2`. The current generated shadercompiler output appears to omit those helper paths, so the real compile path accepts the unit because the mismatching helpers are unused or removed before final output, not because `float2` is a valid `float3` argument or because LSP missed an overload. Relaxing user-function call compatibility, suppressing unused helper diagnostics, or adding a `GetVisibility` allowlist would be broader public diagnostics policy, not a P24C shared call/type fix. |

## Not Admitted

- No `GetVisibility` symbol allowlist or file-specific suppress should be added.
- No global `float2 -> float3` user-function argument compatibility should be added. That would hide real mismatches and conflict with the existing shared type relation contract.
- No call-graph or shadercompiler dead-code based diagnostics suppress should be added in this phase. That would be a broad public diagnostics behavior change requiring a separate architecture/policy decision.
- No macro-generated overload or signature fallback is needed for these samples; the currently found source signature is correct.

## Admitted List

None for P24C.

P24C is therefore an owner review closure, not an implementation phase. The remaining external/source action is to decide whether the unused helper calls should be fixed to pass `float3`, guarded away, removed, or accepted as project dead-code policy.

## Validation

No product code was changed during this review. Validation was limited to evidence inspection:

- Parsed `real-workspace-diagnostics-audit.phase-24a-modf-builtin-trend-50.json` and confirmed 178 `GetVisibility` diagnostics across 50 units.
- Inspected real workspace source:
  - `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\shaderlib\shadow.hlsl:969-1132`
  - `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\shaderlib\const_macros.hlsl:16-19`
- Inspected generated shadercompiler output:
  - `C:\Software\WorkTemp\G66ShaderDevelop\tmp_code_origin.hlsl:2226-2254`
  - `C:\Software\WorkTemp\G66ShaderDevelop\shadercompiler\tmp_code_dx11.hlsl:2606-2634`
- Searched `shader-source` for `GetVisibility`, `GetVisibilityPcf`, `GetHeightMapShadowPcfDisk`, `GetHeightMapShadowPcfDiskOneSample`, and `GetHeightMapShadowPcf` references.

No build, repo integration, smoke audit, trend audit, or focused fixture was required because no implementation or public diagnostics behavior changed.
