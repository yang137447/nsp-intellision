# 资源规范

本文档描述 `server_cpp/resources/` 的当前事实布局、加载规则和维护流程。资源字段语义不在本文展开；对象类型和对象方法契约见 `docs/type-method-interface-contract.md`。

## 布局规则

所有运行时资源都使用 bundle 布局，不允许重新引入 flat 文件命名。

每个 bundle 目录统一包含：

```text
<bundle>/
  base.json
  override.json
  schema.json
```

当前 bundle：

```text
server_cpp/resources/
  builtins/intrinsics/
  language/keywords/
  language/directives/
  language/semantics/
  language/preprocessor_macros/
  methods/object_methods/
  types/object_types/
  types/object_families/
  types/type_overrides/
```

## Bundle 职责

- `builtins/intrinsics`: HLSL builtin 函数名、签名和文档
- `language/keywords`: 关键字名和说明
- `language/directives`: 预处理指令名、说明和语法
- `language/semantics`: 系统语义名、说明和典型类型
- `language/preprocessor_macros`: shadercompiler 风格默认预处理宏 preset，用于首次填充用户可见的 `nsf.preprocessorMacros` 工作区设置；包含 `builtin_macros.py` 的 builtin 宏以及 `hlsl_process.py` 编译上下文宏名
- `methods/object_methods`: 对象方法签名、适用对象族和文档
- `types/object_types`: 对象类型定义
- `types/object_families`: 对象族与兼容关系
- `types/type_overrides`: 类型和对象族覆盖层

## 加载与合并

当前加载入口是 `server_cpp/src/resource_registry.*`：

1. 通过 bundle key 解析路径。
2. 读取 `schema.json`。
3. 读取 `base.json` 和 `override.json`。
4. 分别执行 schema 校验。
5. 按 `override > base` 合并同名条目。

约定：

- `override.json` 可以覆盖同名 `base.json` 条目。
- `disabled: true` 表示显式禁用条目。
- 即使没有覆盖项，也保留有效的空 `override.json`。
- schema 同时被 `scripts/json/validate_resources.js` 和运行时 `resource_registry.*` 使用。

## 修改流程

修改 `server_cpp/resources/` 下任意 bundle 时：

1. 修改对应 `base.json` 或 `override.json`。
2. 如果结构变化，同步修改 `schema.json`。
3. 运行 `npm run json:validate`。
4. 如果需要本地构建产物拿到最新资源，运行 `cmake --build .\server_cpp\build`。
5. 如果影响 completion、hover、diagnostics 或 semantic tokens，运行 `npm run test:client:repo`。
6. 如果路径、命名、加载规则或字段契约变化，同步更新相关事实文档。

## 用户配置填充

`language/preprocessor_macros` 是随扩展发布的默认 preset。client 首次发现当前工作区没有显式 `nsf.preprocessorMacros` 设置时，会通过 server 共享 registry 读取该 preset，并写入工作区设置。之后这份设置就是普通用户配置；用户删掉某个 key，就表示该宏不再属于有效 preset。

preset 包含两类事实：

- `shadercompiler/data/builtin_macros.py` 中的默认 builtin 宏和质量等级派生宏。
- `shadercompiler/hlsl_process.py` 中的编译上下文宏名，包括 system / device / API support / platform quality 宏；这些宏在默认 preset 中使用保守值 `0`，真实 target 或 compile mode 值应由 active unit compile profile、workspace `nsf.preprocessorMacros` 或 `nsf.defines` 覆盖。

server 构建预处理环境时按以下顺序合并：

1. `nsf.preprocessorMacros` 完整有效 preset 表。
2. active unit compile profile 提供的显式数值宏；当前会先尝试 `gimlocalvariants.json` 中对该 shader key 所有 local variants 都一致的宏值，再回退到 `used_shader_variants.csv` 中对该 unit stem 所有 used-variant rows 都一致的宏值。如果存在 `active_unit_variant_selection.csv`，会先按 unit stem 聚合 row 选择提示，再由 workspace `nsf.preprocessorMacros` / `nsf.defines` 中可解析为整数的显式值覆盖同名 hint；仍然只在 profile source 已出现该宏且存在匹配值行时收敛，不猜默认。
3. `nsf.defines` 数字宏。
4. active unit / include / 当前文件里的 `#define` 和 `#undef`。

因此资源 bundle 只负责提供初始填充值，分析时不再隐藏叠加 bundle 默认值；用户配置应被视为预处理宏 preset 层，而不是新的资源 bundle 或 diagnostics 特判。

## 生成与拷贝

资源相关脚本：

- `scripts/builtins/update_hlsl_intrinsics_manifest.js`: 生成 `server_cpp/resources/builtins/intrinsics/base.json`
- `scripts/builtins/update_preprocessor_macros.py`: 从 shadercompiler `builtin_macros.py` 生成 `server_cpp/resources/language/preprocessor_macros/base.json`；可传入 `--const-macros` 补齐 builtin 表达式依赖的枚举 / 常量宏，可传入 `--compiler-context` 指向 `hlsl_process.py` 以补齐编译上下文宏名
- `scripts/json/validate_resources.js`: 校验所有资源 bundle

构建规则：

- `server_cpp/CMakeLists.txt` 的 `nsf_lsp_resources` 目标会在每次 build 时把整个 `server_cpp/resources/` 拷贝到可执行文件旁边的 `resources/` 目录，即使 C++ 可执行文件本身没有重新链接。
- 生成脚本输出必须直接对准当前 bundle 路径。

## 禁止项

- 不要新增 `manifest.json`、`*_overrides.json`、`resources/schemas/*.schema.json` 等旧布局。
- 不要让 feature 代码绕过 registry 直接读取任意 JSON 文件。
- 不要把临时或实验性资源放进正式 bundle 路径而不配 `schema.json`。

## 更新本文档

以下变化必须同步更新本文档：

- 资源目录、bundle 名称或 bundle key 变化
- bundle 加载、校验、合并或构建拷贝规则变化
- 资源生成或资源校验脚本入口变化
