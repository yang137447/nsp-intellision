# NSF LSP

NSF LSP 是面向 `.nsf/.hlsl/.hlsli/.fx/.usf/.ush` 的 VS Code 语言服务扩展，提供补全、悬停、签名帮助、定义跳转、引用、重命名、语义高亮与诊断能力。

扩展同时为上述文件提供基础编辑器能力，如注释切换、括号/引号自动配对、保守的选词规则、注释续写、`#region/#endregion` 折叠与最小 snippets。

## 安装

使用 VS Code 的 `Install from VSIX` 安装打包产物即可。

普通用户不需要手动指定 `nsf_lsp.exe`，插件会优先使用内置的 C++ server。

## 常用设置

普通用户通常只需要这些设置：

- `nsf.intellisionPath`
  - 工作路径列表（用于 `#include` 搜索、索引扫描与 include-context 分析）
- `nsf.include.validUnderline`
  - 可解析 include 是否显示下划线
- `nsf.shaderFileExtensions`
  - 参与索引和 include 解析的扩展名，默认包含 `.nsf/.hlsl/.hlsli/.fx/.usf/.ush`
- `nsf.defines`
  - 预处理宏定义
- `nsf.inlayHints.enabled`
  - 是否启用参数名提示
- `nsf.inlayHints.parameterNames`
  - 是否显示参数名提示
- `nsf.semanticTokens.enabled`
  - 是否启用语义高亮
- `nsf.diagnostics.mode`
  - 诊断强度，推荐使用 `basic / balanced / full`

## 高级设置

`nsf.serverPath` 是高级 override，只用于开发和调试时覆盖插件内置的 C++ server。普通用户应保持为空。

`nsf.overloadResolver.enabled` 是开发/验证用的实验开关，普通用户通常不需要修改。

## 常用命令

- `NSF: Restart LSP Server`
  - 重启语言服务进程，适合处理启动异常、通信异常或服务端卡住
- `NSF: Rebuild Index (Clear Cache)`
  - 清除当前工作区的 NSF 索引缓存并执行一次完整重建
  - 适合处理索引残留、历史缓存污染或怀疑旧文件仍被索引的场景

## 当前预处理诊断规则

- `#if/#elif` 条件求值会先纳入当前 active `#include` 链路里的 `#define/#undef` 宏状态；若仍出现未定义裸宏则报 `error`，并按 `0` 继续求值
- 同一组条件分支只会选择一个 active branch；inactive branch 默认不参与后续 diagnostics 与 `#include` 校验
- hover / definition / 基于局部类型的成员 completion 会优先解析当前 active branch 上的声明与类型
- immediate syntax / full diagnostics 在判断当前文档 `#if/#elif` active branch 时，也会纳入可加载 include 链中的宏状态；active include-controlled branch 里的缺分号等语法问题不应被当成 inactive branch 跳过
- missing semicolon diagnostics 现在会忽略仍处于 active 多行 `(`/`[` 分组内部的续行；像多行构造/函数调用参数行在后续 `);` / `]` 收束前不应再被误报为缺分号
- struct member hover 会显示字段类型；如果字段声明带有前置注释或行尾注释，也会一并显示
- struct hover 在可解析时也会列出 struct body 中 active inline `#include` 片段带来的成员与字段类型
- function / symbol / struct member hover 中的 `Defined at` 位置现在会渲染成可点击文件链接；重复的 overload 位置项会按签名与位置去重
- `#define` 对象宏与函数式宏的 hover 现在会按 macro 渲染，不再误显示成 `(HLSL function)`；只有 macro-generated function 这类展开后产出真实函数声明的模式仍按函数 hover 显示
- current-doc parameter hover 现在也会显示参数类型与当前文件内的 `Defined at` 位置；对同文件 parameter / function / symbol hover 结果不再附带 include-context 摘要
- function hover 的 overload 列表现在只来自 current-doc / shared semantic snapshot / workspace summary 等既有共享语义来源，不再额外拼接 request 层的 per-definition fallback 签名
- 对同文件内互斥条件分支中的同名局部声明，`references` / `rename` 会按条件符号族聚合该组 branch 变体
- 对 include 文件，在没有 active unit 且存在多个候选 `.nsf` root unit 时：
  - hover 只会在 candidate unit 最终收敛到多个不同定义位置时显示 include context ambiguous 提示，并按 candidate definitions 分组显示候选摘要
  - 如果所有 candidate units 最终收敛到同一个定义位置，hover 不会额外附带 include-context 摘要
  - 如果只有部分 candidate units 能收敛到定义，hover 会显示 partial-resolution 提示，但不会平铺 candidate definitions 列表
  - definition 可能返回多个候选位置
  - references 会按候选 unit 聚合结果
  - rename 会拒绝执行，避免跨不明确上下文批量改名
  - candidate units 的定义摘要现在基于 workspace summary 的 indexed include closure 过滤生成，不再走 include-graph 直扫

## 当前编辑态反馈分层

- diagnostics 现已分成两层：
  - Immediate syntax：优先发布缺分号、括号不配对、块注释未闭合、预处理配对等低成本语法反馈
  - Deferred semantic：随后补齐 include、类型、调用参数等较重语义诊断
- completion / hover / signature help / 当前文档短路径 definition / member completion 会优先命中由 `didOpen/didChange` 与分析上下文刷新预热的 current-doc interactive runtime，并在连续编辑期间保留 last-good snapshot 兜底
- 对小范围括号/分号一类 syntax-only 编辑，以及纯注释编辑，`didChange` 现在会先让 immediate syntax diagnostics 抢占热路径；interactive snapshot 仍会保持当前版本可发布
- 对纯注释编辑，当前还会跳过本次 `didChange` 上的 deferred-doc 重建与 full diagnostics 立即重排，优先保住下一次 interactive 请求的热路径时延
- 普通 completion 现在会优先合并 current-doc locals / params / top-level functions / globals / structs，再回落到 workspace summary 与静态资源候选
- `base.` 一类成员 completion 现在会优先返回带字段类型的 struct 成员项；workspace summary 回退路径也会过滤 `#if/#endif` 等预处理指令行，避免把宏名误当成 struct 成员
- 对纯注释/空白编辑，interactive runtime 会基于 changed ranges 优先做 incremental promote（last-good 直升当前版本），降低无语义编辑下的重建抖动
- definition / hover / signature help 的编辑热路径现在只按 current-doc runtime -> deferred doc snapshot -> workspace summary 逐层查询，不再做 include-graph 直扫兜底
- references / rename 现在会基于 workspace summary 的 indexed include closure 收集 active occurrences；include-context ambiguous 时仍按 candidate units 分支处理
- semantic tokens full / document symbols 会通过 deferred doc runtime 预热并复用 current-doc AST / semantic snapshot
- deferred doc runtime 现在还会缓存 full diagnostics 与 full-document inlay hints，并在 range/request 层做复用与切片
- deferred doc runtime 后台任务现在也会预热 full diagnostics；full-document inlay hints 的构建逻辑已从请求编排层抽到独立 runtime
- Lane C 后台请求已统一 latest-only + cancellation 策略，覆盖 inlay / semantic tokens / document symbols / references / prepareRename / rename / workspace symbol
- semantic tokens 不再产出 `comment` / `string` 结果；注释和字符串着色继续由 TextMate grammar / 编辑器壳层负责

## 开发验证

- `npm run test:client:repo`
  - 仓库模式集成测试，验证当前事实行为
- `npm run test:client:repo:m4`
  - 定向运行 `M4` shared analysis context repo 验收，按子场景顺序执行
- `npm run test:client:perf`
  - perf 模式基线测试，采集 wall-clock 与最新 `nsf/metrics` 快照，并输出到 `out/test/perf-reports/`
  - 当前覆盖 `M0-M5` 的主线 smoke，包括调度隔离、immediate syntax、current-doc interactive、shared analysis context / deferred-doc，以及 workspace summary / reverse-include 场景

## 开发文档

开发、调试、测试、打包与高级设置说明请看：

- `docs/client-editor-features.md`
- `docs/development.md`
- `docs/architecture.md`
- `docs/type-method-interface-contract.md`
- `docs/human-ai/realtime-feedback-design.md`
- `docs/resources.md`
- `docs/testing.md`

当前 client/server 入口边界与拆分后的主要模块落点，以 `docs/architecture.md` 为准；当前 `client/src/extension.ts` 主要承担 activate / lifecycle wiring 与高层装配，`server_cpp/src/app/main.cpp` 主要承担 server 入口、消息循环与顶层协调。

其中 `docs/human-ai/` 用于沉淀人类与 AI 协作过程中的设计稿、背景、方案权衡与共享 skill，不属于当前事实文档；当前事实仍以 `README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md` 为准。
