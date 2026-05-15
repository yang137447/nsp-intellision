# Real Workspace Diagnostics Audit Handoff

## 状态

- 日期：2026-05-12
- 任务：统计真实工作区诊断，归类真实报错与插件不完善导致的误报 / 弱报。
- 当前结论：真实工作区存在大量诊断，但主体不是已坐实的源码错误，而是插件 diagnostics 模型不完善、类型规则过严、宏 / include context 不完整导致的误报或待复核项。

## 已完成

1. 新增 real workspace diagnostics audit 测试入口：
   - `src/test/suite/realWorkspace.diagnostics-audit.test.ts`
   - 默认跳过，只在 `NSF_REAL_DIAGNOSTICS_AUDIT=1` 且 file-filter 定向时运行。
   - 统计 VS Code 已发布 diagnostics，输出 JSON 与 Markdown 报告。

2. 更新测试文档：
   - `docs/testing.md`
   - 增加 audit 运行命令、报告目录和“只做统计 / triage hint，不作为门禁”的说明。

3. 完成真实工作区全量采集：
   - workspace：`C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`
   - 扫描文件：`1342/1342`
   - 有诊断文件：`172`
   - 诊断总数：`4289`
   - file errors：`0`
   - diagnostic wait timeouts：`5`

## 报告路径

原始采集报告：

- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.latest.json`
- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.latest.md`

基于抽样复核后调整分类的报告：

- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.adjusted.json`
- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.adjusted.md`

新线程优先读 adjusted 报告。

## 调整后统计结论

按 adjusted triage：

| Triage | Count | 说明 |
| --- | ---: | --- |
| `likely-plugin-limitation` | 3908 | 大概率是插件模型不完善或规则过严 |
| `check-config-or-source` | 299 | 需要真实 defines / include / 生成文件上下文确认 |
| `needs-manual-review` | 82 | 需要逐条或按类复核，不能直接判真错 |

主要类别：

| Category | Count | 归因倾向 |
| --- | ---: | --- |
| `effect-syntax-or-macro` | 1712 | 插件对 NSF / FX annotation、state block、多行初始化等语法支持不足 |
| `expression-type-analysis` | 1429 | 类型兼容规则过严，典型是 `half = float`、`half4 = float4`、`return float4` 给 `half4` |
| `undefined-identifier` | 558 | 宏 / 生成代码 / 预处理上下文不足导致的 undefined 误报倾向 |
| `preprocessor-context` | 299 | 真实 defines 或条件宏上下文待确认 |
| `call-type-analysis` | 207 | builtin、object method、用户函数调用推断不足 |
| `semantic-source-rule` | 61 | duplicate / unreachable 等，需要人工复核 |
| `syntax-structure` | 13 | 剩余多行语句结构问题，倾向仍需人工复核 |

## 抽样复核要点

- 原始报告曾把大量 `Missing semicolon` 归为 `likely-real-source`，但抽样发现大量是合法或项目约定语法：
  - `float u_wind_y_stress` 下一行 `< ... > = 1.0f;`
  - `SamplerState s_diffuse { ... };`
  - `.fx` 中 `int GlobalParameter: SasGlobal < ... >`
  - 多行数组 / 构造 / 条件表达式。
- 因此 adjusted 报告把这些转入 `effect-syntax-or-macro` / `likely-plugin-limitation`，并把剩余 `Missing semicolon` 降级为 `needs-manual-review`。
- `Assignment type mismatch` 抽样显示主体是 HLSL 常见隐式转换或插件过严：
  - `half = float`
  - `half4 = float4`
  - `half3 = float3`
  - `float2 = half2`
  - `Return type mismatch: expected half4 but got float4`
- `Duplicate local declaration` 抽样有明显误报风险，例如循环 / 分支 / 多函数作用域没处理干净；暂列 `needs-manual-review`。
- `Unreachable code` 抽样有真实可能，也有解析状态误判风险；暂列 `needs-manual-review`。

## 已运行验证

```powershell
npm run compile
```

小样本 audit：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_FILES = "25"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "600000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

全量 audit：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_FILES = "0"
$env:NSF_REAL_DIAGNOSTICS_PER_FILE_TIMEOUT_MS = "5000"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "7200000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

全量运行耗时约 18 分钟。

## 当前工作树注意事项

- 本轮新增 / 修改：
  - `src/test/suite/realWorkspace.diagnostics-audit.test.ts`
  - `docs/testing.md`
- 续接后范围已收敛为只关注 `.nsf/.hlsl`：
  - real diagnostics audit 应只采集这两个扩展名。
  - 默认语言注册、`nsf.shaderFileExtensions` 和 server fallback 也应保持 `.nsf/.hlsl` 一致。
- 工作树里还有 `nsf-lsp-1.0.0.vsix` 为 modified。本轮没有处理它，可能是先前已有或构建 / 打包产物变化；新线程不要误删或回退，除非用户明确要求。
- `out/test/diagnostics-audit/*` 是生成报告目录，未见于 `git status`，可能被 ignore。

## 建议下一步

优先按系统性根因治理，而不是逐条特判：

1. 先治理 `effect-syntax-or-macro`（已开始）
   - 目标：让 NSF metadata annotation、state block、多行构造 / 数组初始化不再触发 `Missing semicolon`。
   - 影响最大，约 `1712` 条。
   - 可能涉及 shared line scan / syntax diagnostics / semantic diagnostics 的 statement boundary 规则。
   - 续接后已按 `.nsf/.hlsl` 范围修复 NSF metadata header、state block header、多行函数签名尾行和 initializer `};` 前一行的 `Missing semicolon` 误报；修复点在 `server_parse.*` 共享判定。
   - 已新增 `test_files/module_diagnostics_nsf_effect_headers_ok.nsf` 和 diagnostics full/basic 回归。

2. 再治理 `expression-type-analysis`
   - 目标：把 HLSL 合法或项目接受的隐式转换纳入共享类型兼容规则。
   - 重点：`half/float` 家族、vector 维度兼容、texture Sample 返回值、return 类型兼容。
   - 应优先落到共享类型模型 / diagnostics semantic common，而不是 request 层特判。

3. 再看 `undefined-identifier` 与 `preprocessor-context`
   - 先确认真实 workspace 编译 defines。
   - 如果根因是宏展开 / function-like macro / generated-code context，应该在 preprocessor / macro shared 层收敛。

4. 最后人工复核 `needs-manual-review`
   - 仅 82 条，适合等大类误报压下去后逐项确认。
   - 特别看 duplicate declaration 和 unreachable 是否来自作用域 / 控制流模型缺陷。

## 最小续接上下文

新线程启动后建议先读：

1. `README.md`
2. `docs/architecture.md`
3. `docs/resources.md`
4. `docs/testing.md`
5. 本文档
6. `out/test/diagnostics-audit/real-workspace-diagnostics-audit.adjusted.md`

然后从 `effect-syntax-or-macro` 第一优先级开始定位根因。
