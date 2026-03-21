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
- struct member hover 会显示字段类型；如果字段声明带有前置注释或行尾注释，也会一并显示
- struct hover 在可解析时也会列出 struct body 中 active inline `#include` 片段带来的成员与字段类型
- 对同文件内互斥条件分支中的同名局部声明，`references` / `rename` 会按条件符号族聚合该组 branch 变体
- 对 include 文件，在没有 active unit 且存在多个候选 `.nsf` root unit 时：
  - hover 会显示 include context ambiguous 提示，并附带 candidate units 定义摘要列表
  - definition 可能返回多个候选位置
  - references 会按候选 unit 聚合结果
  - rename 会拒绝执行，避免跨不明确上下文批量改名

## 开发文档

开发、调试、测试、打包与高级设置说明请看：

- `docs/client-editor-features.md`
- `docs/development.md`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`
