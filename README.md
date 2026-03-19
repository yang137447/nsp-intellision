# NSF LSP

NSF LSP 是面向 `.nsf/.hlsl/.fx/.usf/.ush` 的 VS Code 语言服务扩展，提供补全、悬停、签名帮助、定义跳转、引用、重命名、语义高亮与诊断能力。

## 安装

使用 VS Code 的 `Install from VSIX` 安装打包产物即可。

普通用户不需要手动指定 `nsf_lsp.exe`，插件会优先使用内置的 C++ server。

## 常用设置

普通用户通常只需要这些设置：

- `nsf.includePaths`
  - 额外的 `#include` 搜索路径
- `nsf.include.validUnderline`
  - 可解析 include 是否显示下划线
- `nsf.shaderFileExtensions`
  - 参与索引和 include 解析的扩展名
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

## 开发文档

开发、调试、测试、打包与高级设置说明请看：

- `docs/development.md`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`
