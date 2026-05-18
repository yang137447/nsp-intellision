# NSF LSP

NSF LSP 是面向 `.nsf/.hlsl` 的 VS Code 语言服务扩展，提供补全、悬停、签名帮助、定义跳转、引用、重命名、语义高亮、诊断和参数名提示。

扩展也提供基础编辑器壳层能力，包括注释切换、自动配对、保守的 `wordPattern`、注释续写、`// #region` / `// #endregion` 折叠和最小 snippets。

## 安装

使用 VS Code 的 `Install from VSIX` 安装打包产物即可。

普通用户不需要手动指定 `nsf_lsp.exe`。扩展默认使用内置 C++ server；`nsf.serverPath` 仅用于开发和调试覆盖。

## 常用设置

- `nsf.intellisionPath`: 工作路径列表，用于 `#include` 搜索、索引扫描和 include-context 分析
- `nsf.include.validUnderline`: 可解析的 `#include` 路径是否显示下划线
- `nsf.shaderFileExtensions`: 参与索引和 include 解析的扩展名，默认 `.nsf/.hlsl`
- `nsf.preprocessorMacros`: 预处理宏 replacement 配置；扩展首次会把内置 preset 写入工作区设置，之后用户可直接编辑、删除或补充
- `nsf.defines`: 预处理宏定义；会覆盖 active unit compile profile 提供的数值宏
- `nsf.inlayHints.enabled`: 是否启用参数名提示
- `nsf.inlayHints.parameterNames`: 是否显示参数名提示
- `nsf.semanticTokens.enabled`: 是否启用语义高亮
- `nsf.diagnostics.mode`: 诊断强度，推荐使用 `basic`、`balanced` 或 `full`；默认 `balanced` 会隐藏合法但高噪的隐式转换风险 warning，`full` 会显示这类转换风险以便源码审核

高级设置：

- `nsf.serverPath`: 覆盖内置 C++ server 路径，仅用于开发和调试
- `nsf.overloadResolver.enabled`: 实验开关，普通用户通常不需要修改

## 常用命令

- `NSF: Restart LSP Server`: 重启语言服务进程
- `NSF: Rebuild Index (Clear Cache)`: 清除当前工作区索引缓存并执行完整重建

## 当前能力边界

- LSP 语义能力由 C++ server 和共享 registry 提供；语言知识不应在 client 侧复制。
- 编辑器壳层能力由 VS Code manifest、language configuration 和 snippets 提供；这类行为不是 server 语义真相。
- 资源只支持 bundle 布局，详见 `docs/resources.md`。
- 预处理宏 preset 会首次写入 `nsf.preprocessorMacros` 工作区设置；设置里看到的宏表就是实际 preset。分析时 server 还会按 active unit 尝试接入 shadercompiler 导出的 compile profile 宏，随后再由 `nsf.defines` 和源码 `#define/#undef` 覆盖。
- 对象类型、对象方法和参数标签共享契约详见 `docs/type-method-interface-contract.md`。

## 开发验证

常用验证入口：

- `npm run json:validate`
- `npm run compile`
- `cmake --build .\server_cpp\build`
- `npm run test:client:repo`
- `npm run gate:d3`
- `npm run package:vsix`

不同改动该跑哪一层，以 `docs/testing.md` 为准。开发、调试和打包细节以 `docs/development.md` 为准。

## 文档入口

当前事实文档：

- `README.md`: 用户入口、能力摘要和文档导航
- `docs/architecture.md`: 模块边界、请求链路和单一事实来源
- `docs/resources.md`: 资源 bundle 布局、加载和维护规则
- `docs/testing.md`: 验证命令、测试入口和测试约束

专题事实文档：

- `docs/client-editor-features.md`: client 编辑器壳层能力
- `docs/type-method-interface-contract.md`: 对象类型 / 对象方法共享契约
- `docs/development.md`: 本地开发、调试与打包流程

协作沉淀：

- `docs/human-ai/`: 设计稿、任务背景、方案权衡和共享 skill，默认不作为当前事实来源
- `docs/superpowers/`: 计划和规格沉淀，按具体文档声明判断是否仍是当前事实
