# NSF LSP

NSF LSP 是面向 `.nsf/.hlsl/.hlsli/.fx/.usf/.ush` 的 VS Code 语言服务扩展，提供补全、悬停、签名帮助、定义跳转、引用、重命名、语义高亮、诊断和参数名提示。

扩展同时提供基础编辑器壳层能力，如注释切换、自动配对、保守的 `wordPattern`、注释续写、`#region/#endregion` 折叠和最小 snippets。

## 安装

使用 VS Code 的 `Install from VSIX` 安装打包产物即可。

普通用户不需要手动指定 `nsf_lsp.exe`。扩展默认使用内置的 C++ server，`nsf.serverPath` 仅用于开发和调试覆盖。

## 常用设置

- `nsf.intellisionPath`
  - 工作路径列表，用于 `#include` 搜索、索引扫描和 include-context 分析
- `nsf.include.validUnderline`
  - 可解析的 `#include` 路径是否显示下划线
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

高级设置：

- `nsf.serverPath`
  - 覆盖内置 C++ server 路径，仅用于开发和调试
- `nsf.overloadResolver.enabled`
  - 实验开关，普通用户通常不需要修改

## 常用命令

- `NSF: Restart LSP Server`
  - 重启语言服务进程
- `NSF: Rebuild Index (Clear Cache)`
  - 清除当前工作区索引缓存并执行完整重建

## 当前公开行为摘要

- `#if/#elif` 条件求值会纳入当前 active `#include` 链路中的宏状态；inactive branch 默认不参与后续 diagnostics 和大多数局部语义查询。
- diagnostics 分成两层：先发布低成本的 immediate syntax，再补齐较重的 deferred semantic；当 full diagnostics 仍在等待时，fast publish 会继续保留上一份 last-good full 结果，避免无关 semantic 波浪线被整份清空。
- completion、hover、signature help 和当前文档短路径 definition 会优先命中 current-doc runtime，再按既有共享语义链路回退。
- completion 对 `.` / `[` / `#` 一类特定语法字符仍保留专用触发；client 侧同时通过 `[nsf]` / `[hlsl]` 语言级 editor defaults 和活跃编辑器里的标识符输入自动 suggest fallback，保持普通函数输入过程能主动拉起候选。
- dirty doc 上的普通标识符 completion 与 signature help，在 client 发起请求前会先等待挂起的 `didChange` 同步落到 server，避免编辑态出现“慢一拍”的旧文本视图。
- inlay hints 属于 deferred 路径；当 full-document inlay snapshot 尚未就绪时，请求当前可见范围会优先走 visible-range first 构建，full-document cache 仍在后台补齐。对 indexing 抖动、请求取消或瞬态 RPC 错误，client 会优先续用最近一次成功的 last-good hints，而不是把瞬态不可用直接翻译成空 UI。
- completion 在普通标识符前缀场景下，会先保留 current-doc / include-context 命中的候选，并对 server 侧静态 catalog 候选做同前缀过滤；编辑器自身的 word completion 仍可能追加额外词条。
- `base.` 一类成员 completion 在当前文档局部声明可直接判定 owner type 时，会优先走 current-doc declaration / lexical fallback，而不是先等待更重的 snapshot 重建。
- 顶层 `<>` metadata block 声明会继续按全局变量参与语义查询；对常见 `SasUi*` / `TextureFile` / `ThumbnailEnable` 字段，hover 会显示已知 UI metadata 摘要。
- Neox `technique / pass` 结构头当前不会被当成缺分号语句；`pass` block 内的 `StencilRef / StencilReadMask / StencilWriteMask` 整数字面量会补二进制 inlay hints。
- include 文件在没有唯一 active unit 时，definition 可能返回多个候选；rename 会在 include-context ambiguous 时拒绝执行。对于 current `.nsf` unit 的 indexed include closure，如果同名 helper 定义不唯一，definition 会返回多候选，hover / signature help 会明确标记 current-unit include closure ambiguous。
- 项目专用预处理指令 `#art`、`#expression`、`#excludefromtemptech` 当前会参与 directive hover / completion。
- semantic tokens 只负责语义项；注释和字符串着色继续由 TextMate grammar / 编辑器壳层负责。

## 开发验证

- `npm run test:client:repo`
  - 默认的仓库模式集成回归
- `npm run test:client:perf`
  - 性能基线与 `nsf/metrics` 快照采集
- `npm run test:client:real:perf`
  - real workspace 性能补充层，针对固定 real workspace 采集 `Idle / Load-BG` 交互基线与 `nsf/metrics`

更完整的验证分层和测试约定以 `docs/testing.md` 为准。

## 文档入口

当前事实文档：

- `README.md`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`

按主题补充阅读：

- `docs/client-editor-features.md`
  - client 编辑器壳层能力
- `docs/type-method-interface-contract.md`
  - 对象类型 / 对象方法共享契约
- `docs/development.md`
  - 开发、调试与打包说明

`docs/human-ai/` 用于沉淀设计稿、任务背景、方案权衡和协作记录，默认不属于当前事实文档。
