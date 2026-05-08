# Client 编辑器能力

本文档描述 VS Code client / manifest / 静态配置层的编辑器壳层能力。它是当前事实文档，但不负责 LSP 语义行为。

## 适用范围

先读本文的场景：

- 修改 `package.json` 的语言注册、grammar、snippets 或 editor 相关设置描述
- 修改 `syntaxes/nsf.language-configuration.json`
- 修改 `snippets/nsf.code-snippets`
- 排查注释切换、自动配对、选词、回车续写、folding 或 snippet 问题

不属于本文职责：

- hover、completion、signature help、diagnostics 的语义正确性
- server 侧资源 bundle、schema 和共享查询模块

## 单一事实来源

- `package.json`
  - `contributes.languages`
  - `contributes.grammars`
  - `contributes.snippets`
  - `contributes.configuration`
- `syntaxes/nsf.language-configuration.json`
  - 注释、配对、`wordPattern`、回车规则、folding markers
- `snippets/nsf.code-snippets`
  - 代码片段内容
- `client/src/extension.ts`
  - client 运行时对 `nsf.shaderFileExtensions` 的默认值兜底
- `server_cpp/src/app/main.cpp`
  - server 启动时的默认 shader 扩展名兜底

结论：

- 表现为 editor 行为不触发时，先检查 manifest 挂接和文件路径。
- 表现为触发后行为不理想时，先检查 language configuration。
- 扩展名表现不一致时，同时检查语言注册、client 默认值、server 默认值和 README。

## 当前能力

- 语言模式注册：`.nsf/.hlsl/.hlsli/.fx/.usf/.ush`
- grammar 挂接
- 行注释 `//`
- 块注释 `/* */`
- `{}`、`[]`、`()`、`""` 自动配对和选中包裹
- 基础缩进
- 保守版 `wordPattern`
- `///` 与 `/** */` 注释续写
- `#region/#endregion` 折叠
- 最小 snippets 集

当前边界：

- 不对 `<` / `>` 做自动配对，避免误伤 `Texture2D<float4>` 和比较表达式。
- `wordPattern` 优先保证 `SV_Position`、宏名、常见标识符和大写开头泛型对象类型；复杂 swizzle 或模板样式只承诺不明显退化。
- 还没有固定注释块折叠的正式规则。

## 语言扩展名一致性

需要保持一致的文件：

- `package.json`
- `client/src/extension.ts`
- `server_cpp/src/app/main.cpp`
- `README.md`

当前事实：

- `.nsf/.hlsl/.hlsli/.fx/.usf/.ush` 都归到 `nsf` 语言。
- `nsf.shaderFileExtensions` 默认值包含这些扩展名。
- client 运行时兜底默认值和 server 启动默认值应保持一致。

新增或删除扩展名时，必须同步检查上述文件和本节。

## Snippets 约束

当前最小片段集：

- `VS_INPUT`
- `PS_INPUT`
- `cbuffer`
- `vs_main`
- `ps_main`
- `technique/pass`
- `#if / #else / #endif`

维护规则：

- snippet 文件新增后，必须在 `package.json` 注册。
- prefix 应保持统一风格，避免 completion 面板混乱。
- 首版优先可直接改写的骨架，不引入过多占位符。

## 验证

验证说明以 `docs/testing.md` 为准。最小手工 smoke 覆盖：

- `.nsf`
- `.hlsl`
- `.hlsli`

重点检查：

- 语言模式归属
- 注释切换
- 自动配对与包裹
- 基础缩进
- `wordPattern`
- 注释续写
- `#region/#endregion`
- snippets

## 更新本文档

以下变化必须同步更新本文档：

- `package.json` 中语言注册、snippets 注册或 editor 相关设置描述变化
- `syntaxes/nsf.language-configuration.json` 规则变化
- `snippets/nsf.code-snippets` 片段集合或命名策略变化
- client 或 server 默认 shader 扩展名变化
- editor 壳层能力公开行为变化
