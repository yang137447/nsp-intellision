# NSF LSP

NSF LSP 是面向 `.nsf/.hlsl` 的 VS Code 语言服务扩展，提供补全、悬停、签名帮助、定义跳转、引用、重命名、语义高亮、诊断和参数名提示。

扩展也提供基础编辑器壳层能力，包括注释切换、自动配对、保守的 `wordPattern`、注释续写、`// #region` / `// #endregion` 折叠和最小 snippets。

## 安装

使用 VS Code 的 `Install from VSIX` 安装打包产物即可。

普通用户不需要手动指定 `nsf_lsp.exe`。扩展默认使用内置 C++ server；`nsf.serverPath` 仅用于开发和调试覆盖。

## 常用设置

- `nsf.intellisionPath`: 工作路径列表，用于 `#include` 搜索、索引扫描和 include-context 分析
- `nsf.shaderCompilerPath`: shadercompiler 根目录或可执行路径；server 只读消费其导出的宏/profile 数据，不在编辑热路径执行完整编译
- `nsf.include.validUnderline`: 可解析的 `#include` 路径是否显示下划线
- `nsf.shaderFileExtensions`: 参与索引和 include 解析的扩展名，默认 `.nsf/.hlsl`
- `nsf.preprocessorMacros`: 预处理宏 replacement 配置；扩展首次会把内置 preset 写入工作区设置，旧版完整 preset 缺少新版内置项时会一次性补齐缺失项，之后用户可直接编辑、删除或补充
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
- 预处理宏 preset 会首次写入 `nsf.preprocessorMacros` 工作区设置；旧版完整 preset 缺少当前默认 preset 新增 key 时，client 会一次性补齐缺失项并保留已有值。设置里看到的宏表就是实际 preset。默认 preset 包含 shadercompiler builtin/context 宏，也保留少量已确认按 legacy `#if` undefined-as-zero 语义工作的 profile / project 宏，以及已验证无冲突的 source enum-like 常量；其中 project 宏的 `0` 只表示没有 source/profile/provider 输入时对齐 shadercompiler 的 undefined-in-`#if` 求值，不代表 material-family 枚举真值。分析时 server 还会按 active unit 尝试接入 shadercompiler 导出的 compile profile 宏，当前会从 `gimlocalvariants.json` 和 `used_shader_variants.csv` 提取对该 unit 稳定且显式的数值宏；并可从 `active_unit_variant_selection.csv` 读取该 unit 的显式 row 选择提示（只在 profile 源已出现该宏且存在匹配值行时生效，不猜默认）。这些 profile source 会从 workspace/include roots 以及可选 `nsf.shaderCompilerPath` 下发现。workspace summary 还会索引 Neox `#art NAME "..." "BOOL"` / `"INT"` 美术宏声明；当 profile、配置和源码都没有给值时，这类宏按默认 `0` 进入预处理环境。索引会保存同一参数块中紧邻 `#art` 的整数 companion 常量，但 server 只在该参数文件属于当前 active unit include closure 且 companion 值无冲突时注入这些常量；未进入 closure 的 material-family 参数常量不会全局注入。PreprocessorView 还会在当前 active unit include closure 内收集稳定、无冲突、最终仍有可见定义的 object-like 单整数 `#define`，作为 shadercompiler private numeric constant 初始输入；同时由 C++ `compiler_macro_snapshot_provider.*` 从 active closure 源码中收集稳定、被实际使用的 object-like 单 token alias 和 `#ifndef` default alias，作为 compiler macro snapshot 初始输入。这两层收集时 `#undef` 会清空当前候选，后续稳定 `#define` 可以重新建立候选；冲突、非单 token / 非整数或最终没有后续定义的候选仍排除。它们用于对齐 shadercompiler 先收集宏快照再求值的 `#if/#elif` 行为，不执行 shadercompiler 或 Python helper，不扫描全 workspace。最终仍由 `nsf.preprocessorMacros`、compiler macro snapshot、profile / `nsf.defines` 和源码 `#define/#undef` 按优先级覆盖。
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
