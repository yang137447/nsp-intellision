# NSF Unit Diagnostics Audit

## Scope

- Date: 2026-05-16
- Workspace: `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`
- Search root: `C:\Software\WorkTemp\G66ShaderDevelop\shader-source`
- Goal: use `.nsf` as the analysis unit, collect diagnostics for each unit include closure, classify error / warning output, identify likely LSP defects, and propose improvement order.

This is a collaboration note, not a promoted current-fact document.

## Audit Tooling Update

The real workspace diagnostics audit now uses `.nsf` units as the primary statistic dimension:

1. Discover `.nsf` files under `nsf.intellisionPath`.
2. Set each discovered `.nsf` as the server active unit.
3. Ask the server for the indexed include closure of that unit.
4. Build diagnostics for each `.nsf/.hlsl` file in the closure with that unit context.
5. Report counts by unit, file, severity, category, triage, canonical message, and bounded diagnostic samples.

Implementation:

- `src/test/suite/realWorkspace.diagnostics-audit.test.ts`
- `server_cpp/src/app/main_diagnostics_audit_debug.*`
- `server_cpp/src/app/main.cpp` dispatches the two internal requests:
  - `nsf/_debugIncludeClosureForUnit`
  - `nsf/_debugBuildDiagnostics`

These requests are internal audit probes. They do not publish diagnostics and do not change public LSP behavior.

## Validation

Passed:

```powershell
npm run compile
cmake --build .\server_cpp\build
```

Smoke audit:

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "5"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "600000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

Larger sample:

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "50"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "1800000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

The 50-unit sample took about 6 minutes 15 seconds.

Full audit:

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "0"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "7200000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

The full 813-unit audit took about 41 minutes after the exponent-literal triage correction.

## Full Audit Summary

Latest report:

- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.latest.json`
- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.latest.md`

Stats:

| Metric | Count |
| --- | ---: |
| NSF units discovered | 813 |
| NSF units scanned | 813 |
| NSF units with diagnostics | 786 |
| Unit file visits | 25985 |
| Unique files discovered | 1191 |
| Unique files with diagnostics | 750 |
| Diagnostics total | 463556 |
| Truncated file builds | 1 |
| Timed-out file builds | 1 |
| File errors | 0 |

Triage:

| Triage | Count |
| --- | ---: |
| `likely-plugin-limitation` | 373861 |
| `needs-manual-review` | 53329 |
| `check-config-or-source` | 36363 |
| `likely-real-source` | 3 |

Categories:

| Category | Count |
| --- | ---: |
| `expression-type-analysis` | 257008 |
| `undefined-identifier` | 53071 |
| `call-type-analysis` | 38920 |
| `preprocessor-context` | 36363 |
| `semantic-source-rule` | 36042 |
| `numeric-literal` | 15676 |
| `other` | 9432 |
| `effect-syntax-or-macro` | 9178 |
| `syntax-structure` | 7858 |
| `indeterminate-analysis` | 8 |

Top canonical messages:

| Count | Category | Message |
| ---: | --- | --- |
| 223382 | `expression-type-analysis` | `Assignment type mismatch: <lhs> = <rhs>.` |
| 53071 | `undefined-identifier` | `Undefined identifier: <symbol>.` |
| 36363 | `preprocessor-context` | `Undefined macro in preprocessor expression: <macro>.` |
| 27151 | `expression-type-analysis` | `Return type mismatch: <expected> vs <actual>.` |
| 23495 | `semantic-source-rule` | `Duplicate local declaration: <symbol>.` |
| 16656 | `call-type-analysis` | `Builtin call type mismatch: <function>.` |
| 15676 | `numeric-literal` | `Invalid numeric literal suffix: <suffix>.` |
| 13855 | `call-type-analysis` | `Built-in method call type mismatch: <method>.` |
| 12227 | `semantic-source-rule` | `Unreachable code.` |
| 9178 | `effect-syntax-or-macro` | `Missing semicolon.` |

## Findings

1. The dominant defect is still HLSL type compatibility.
   - `half` / `float` family conversions are treated too strictly.
   - Scalar-to-vector splat and vector family conversions are not consistently accepted.
   - Return diagnostics show the same root cause, for example `expected half but got float`, `expected half3 but got float3`, and macro-like type aliases such as `MaterialFloat4`.
   - This should be fixed in shared diagnostics type compatibility helpers, not request handlers.

2. Undefined identifier diagnostics are heavily contaminated by parser / scope / generated-context gaps.
   - Samples include `true` and `for` loop variable `i`, which are not real source errors.
   - This points to language keyword/literal recognition and local scope tracking problems.
   - Some symbols such as `grass_max_offset` still need source/config confirmation.

3. Preprocessor context remains incomplete for platform/API macros.
   - Repeated samples include `API_MOBILE_HIGH_QUALITY`, `API_SUPPORT_SV_INSTANCE_ID`, and related platform macros.
   - These should be reconciled against shadercompiler defaults / real compile defines before adding resources.
   - If confirmed as builtin configuration facts, add them through `language/preprocessor_macros` and config seeding, not diagnostics-local suppressions.

4. Duplicate local declaration and unreachable diagnostics are not trustworthy yet.
   - Repeated loop-local samples in `shaderlib/lighting_functions.hlsl` suggest scope tracking across loop / branch blocks is still too coarse.
   - Unreachable code may be real in some cases, but current parser-state errors can produce false positives.
   - Fix local scope modeling before treating these as source-quality findings.

5. Numeric literal classification is wrong for exponent literals.
   - `1e-5` is reported as `Invalid numeric literal suffix`.
   - This is a clear lexer / numeric-literal parser defect and should be an early, low-risk fix.
   - Audit triage now classifies exponent-literal numeric suffix reports as `likely-plugin-limitation`, not real source errors.

6. Remaining `Missing semicolon` groups still point to HLSL syntax coverage gaps.
   - Samples include multiline function signatures and complex conditional expressions.
   - The previous FX/NSF metadata fixes reduced one class, but shared syntax boundary logic still needs broadening.

## Recommended Improvement Order

1. Fix numeric literal parsing for exponent forms such as `1e-5`.
   - Low-risk, clear false positive.
   - Add focused repo diagnostics fixture.

2. Normalize HLSL numeric compatibility in shared expression/type helpers.
   - Cover `half` / `float`, scalar-to-vector splat, same-width numeric vector family conversions, return compatibility, and argument compatibility.
   - This touches public diagnostics behavior and should be confirmed before implementation.

3. Fix local scope and control-flow modeling.
   - Ensure `for` initializer variables are visible inside the loop and duplicate local checks respect nested block / loop scopes.
   - Re-evaluate `Unreachable code` only after scope/control-flow fixes.

4. Audit platform/API macro sources.
   - Compare undefined macros against shadercompiler builtin macro sources and real workspace compile settings.
   - Add confirmed entries to shared preprocessor macro resources or document missing workspace config.

5. Improve object method / builtin call type matching.
   - Consume shared type compatibility rules from step 2.
   - Re-check `Sample` / `SampleLevel` texture-family signatures and builtin overload matching after numeric compatibility is fixed.

6. Re-run unit audit after each category fix.
   - Use a small 5-unit smoke first, then 50-unit trend sample.
   - Run full 813-unit audit only after high-volume categories drop enough to make manual review useful.

## Full Audit Command

Expected to be long-running:

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "0"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "7200000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

## Closure Notes

- Commands changed: no user-facing command changed.
- Paths / naming changed: yes, new internal module `server_cpp/src/app/main_diagnostics_audit_debug.*`.
- Architecture changed: internal debug/audit module added; public LSP behavior unchanged.
- Testing strategy changed: real diagnostics audit is now unit-based.
- Docs updated: `docs/architecture.md` and `docs/testing.md`.
