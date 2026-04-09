# Editor-First Runtime P1 Visibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the first executable slice of the approved editor-first runtime upgrade by making `P1 Interactive` a true hot path and adding a shared interactive visibility layer for current-context-visible cross-file symbols.

**Architecture:** This plan intentionally covers only the first subsystem from the approved design: `P1 foundations + shared interactive visibility`. It keeps the current `document_owner.*` / `document_runtime.*` backbone, adds a new `interactive_visibility_runtime.*` module, and enforces the contract `current -> last-good -> shared-visible -> deferred -> workspace` for variable/function/`.` completion, hover, and signature help.

**Tech Stack:** C++17 server (`server_cpp/src`), VS Code client/test harness (`client/src`, `src/test/suite`), CMake, npm-based repo integration tests.

---

## Scope Check

The approved spec spans multiple independent subsystems. This plan covers only the first one:

- In scope:
  - `P1 Interactive` source-order observability
  - `InteractiveVisibilityKey` in document runtime
  - new `interactive_visibility_runtime.*` module
  - current-context-visible cross-file symbol shards
  - variable/function completion using shared visibility
  - `.` member completion, hover, and signature help using the same order
  - doc/test updates for the new `P1` contract
- Out of scope:
  - deferred artifact/range cache redesign
  - workspace references/rename boundary cleanup
  - full diagnostics / semantic tokens / inlay hints refactor

## File Structure

- Create: `server_cpp/src/interactive_visibility_runtime.hpp`
- Create: `server_cpp/src/interactive_visibility_runtime.cpp`
- Create: `src/test/suite/integration/interactive-visibility.ts`
- Create: `src/test/suite/client.interactive-visibility.test.ts`
- Create: `test_files/visibility_root.nsf`
- Create: `test_files/visibility_shared.hlsli`
- Create: `test_files/visibility_member_root.nsf`
- Create: `test_files/visibility_member_types.hlsli`
- Modify: `server_cpp/CMakeLists.txt`
- Modify: `server_cpp/src/document_runtime.hpp`
- Modify: `server_cpp/src/document_runtime.cpp`
- Modify: `server_cpp/src/document_owner.hpp`
- Modify: `server_cpp/src/document_owner.cpp`
- Modify: `server_cpp/src/interactive_semantic_runtime.hpp`
- Modify: `server_cpp/src/interactive_semantic_runtime.cpp`
- Modify: `server_cpp/src/workspace_summary_runtime.hpp`
- Modify: `server_cpp/src/workspace_summary_runtime.cpp`
- Modify: `server_cpp/src/requests/server_request_handler_completion.cpp`
- Modify: `server_cpp/src/requests/server_request_handler_hover.cpp`
- Modify: `server_cpp/src/requests/server_request_handler_signature.cpp`
- Modify: `server_cpp/src/app/main.cpp`
- Modify: `client/src/client_internal_commands.ts`
- Modify: `src/test/suite/test_helpers.ts`
- Modify: `src/test/suite/client.integration.groups.ts`
- Modify: `docs/architecture.md`
- Modify: `docs/testing.md`

## Follow-Up Plans

Do not silently extend this plan into the whole upgrade. After this plan lands, write separate plans for:

1. `P2 DeferredDoc` semantic-core/artifact split
2. `P3 Workspace` boundary cleanup
3. final performance hardening and release gate updates

### Task 1: Add P1 Resolution Debug Surfaces

**Files:**
- Modify: `server_cpp/src/interactive_semantic_runtime.hpp`
- Modify: `server_cpp/src/interactive_semantic_runtime.cpp`
- Modify: `server_cpp/src/app/main.cpp`
- Modify: `client/src/client_internal_commands.ts`
- Modify: `src/test/suite/test_helpers.ts`
- Modify: `src/test/suite/client.integration.groups.ts`
- Test: `src/test/suite/integration/interactive-visibility.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('reports which P1 layer answered completion requests', async function () {
	this.timeout(90000);

	const document = await openFixture('module_completion_current_doc.nsf');
	const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				document.uri,
				completionPosition
			),
		(value) => getCompletionItems(value).some((item) => item.label.toString() === 'CompletionDocHelper'),
		'current-doc completion before debug query'
	);

	const debug = await waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInteractiveRuntimeDebug', { uri: document.uri.toString() }),
		(value) => value?.uri === document.uri.toString(),
		'interactive runtime debug'
	);

	assert.strictEqual(debug.lastQueryKind, 'completion');
	assert.strictEqual(debug.lastResolvedLayer, 'current');
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because `nsf._getInteractiveRuntimeDebug` does not exist yet.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/interactive_semantic_runtime.hpp
struct InteractiveResolutionDebugSnapshot {
  std::string uri;
  std::string lastQueryKind;
  std::string lastResolvedLayer;
  std::string lastSymbol;
};

void recordInteractiveResolutionDebug(const std::string &uri,
                                      const std::string &queryKind,
                                      const std::string &layer,
                                      const std::string &symbol);
InteractiveResolutionDebugSnapshot
getInteractiveResolutionDebugSnapshot(const std::string &uri);
```

```cpp
// server_cpp/src/interactive_semantic_runtime.cpp
namespace {
std::mutex gInteractiveResolutionDebugMutex;
std::unordered_map<std::string, InteractiveResolutionDebugSnapshot>
    gInteractiveResolutionDebugByUri;
}

void recordInteractiveResolutionDebug(const std::string &uri,
                                      const std::string &queryKind,
                                      const std::string &layer,
                                      const std::string &symbol) {
  std::lock_guard<std::mutex> lock(gInteractiveResolutionDebugMutex);
  gInteractiveResolutionDebugByUri[uri] =
      InteractiveResolutionDebugSnapshot{uri, queryKind, layer, symbol};
}

InteractiveResolutionDebugSnapshot
getInteractiveResolutionDebugSnapshot(const std::string &uri) {
  std::lock_guard<std::mutex> lock(gInteractiveResolutionDebugMutex);
  auto it = gInteractiveResolutionDebugByUri.find(uri);
  return it == gInteractiveResolutionDebugByUri.end()
             ? InteractiveResolutionDebugSnapshot{}
             : it->second;
}
```

```cpp
// server_cpp/src/app/main.cpp
if (method == "nsf/_getInteractiveRuntimeDebug") {
  Json result = makeObject();
  if (params) {
    const Json *uriValue = getObjectValue(*params, "uri");
    if (uriValue && uriValue->type == Json::Type::String) {
      const auto snapshot = getInteractiveResolutionDebugSnapshot(uriValue->s);
      result.o["uri"] = makeString(snapshot.uri);
      result.o["lastQueryKind"] = makeString(snapshot.lastQueryKind);
      result.o["lastResolvedLayer"] = makeString(snapshot.lastResolvedLayer);
      result.o["lastSymbol"] = makeString(snapshot.lastSymbol);
    }
  }
  if (id.type != Json::Type::Null)
    writeResponse(id, result);
  continue;
}
```

```ts
// client/src/client_internal_commands.ts
commands.registerCommand('nsf._getInteractiveRuntimeDebug', async (args?: { uri?: string }) => {
	return deps.sendServerRequest('nsf/_getInteractiveRuntimeDebug', args ?? {});
});
```

```ts
// src/test/suite/test_helpers.ts
export async function getInteractiveRuntimeDebug(uri: string): Promise<any> {
	return waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInteractiveRuntimeDebug', { uri }),
		(value) => value?.uri === uri,
		'interactive runtime debug'
	);
}

export type DocumentRuntimeDebugEntry = {
	uri: string;
	exists: boolean;
	interactiveVisibilityFingerprint?: string;
};
```

```ts
// src/test/suite/integration/interactive-visibility.ts
import * as assert from 'assert';
import * as vscode from 'vscode';

import { getCompletionItems, getInteractiveRuntimeDebug, openFixture, positionOf, repoDescribe, waitFor } from '../test_helpers';

export function registerInteractiveVisibilityTests(): void {
	repoDescribe('NSF client integration: Interactive Visibility', () => {
		it('reports which P1 layer answered completion requests', async function () {
			this.timeout(90000);
			const document = await openFixture('module_completion_current_doc.nsf');
			const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);
			await waitFor(
				() => vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>('vscode.executeCompletionItemProvider', document.uri, completionPosition),
				(value) => getCompletionItems(value).some((item) => item.label.toString() === 'CompletionDocHelper'),
				'current-doc completion before debug query'
			);
			const debug = await getInteractiveRuntimeDebug(document.uri.toString());
			assert.strictEqual(debug.lastQueryKind, 'completion');
			assert.strictEqual(debug.lastResolvedLayer, 'current');
		});
	});
}
```

```ts
// src/test/suite/client.interactive-visibility.test.ts
import { registerInteractiveVisibilityTests } from './client.integration.groups';

registerInteractiveVisibilityTests();
```

```ts
// src/test/suite/client.integration.groups.ts
export { registerInteractiveVisibilityTests } from './integration/interactive-visibility';
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: PASS for the debug-surface test.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/interactive_semantic_runtime.hpp server_cpp/src/interactive_semantic_runtime.cpp server_cpp/src/app/main.cpp client/src/client_internal_commands.ts src/test/suite/test_helpers.ts src/test/suite/client.integration.groups.ts src/test/suite/integration/interactive-visibility.ts src/test/suite/client.interactive-visibility.test.ts
git commit -m "test: add p1 resolution debug surface"
```

### Task 2: Add Interactive Visibility Keys And Runtime Skeleton

**Files:**
- Create: `server_cpp/src/interactive_visibility_runtime.hpp`
- Create: `server_cpp/src/interactive_visibility_runtime.cpp`
- Modify: `server_cpp/CMakeLists.txt`
- Modify: `server_cpp/src/document_runtime.hpp`
- Modify: `server_cpp/src/document_runtime.cpp`
- Modify: `server_cpp/src/document_owner.hpp`
- Modify: `server_cpp/src/document_owner.cpp`
- Test: `src/test/suite/integration/interactive-visibility.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('tracks an interactive visibility key alongside the analysis key', async function () {
	this.timeout(90000);

	const document = await openFixture('module_completion_current_doc.nsf');
	await waitForClientReady();

	const runtime = (await getDocumentRuntimeDebug([document.uri.toString()]))[0];
	assert.ok(runtime?.interactiveVisibilityFingerprint);
	assert.ok((runtime?.interactiveVisibilityFingerprint ?? '').length > 0);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because `interactiveVisibilityFingerprint` is not stored or surfaced.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/document_runtime.hpp
struct InteractiveVisibilityKey {
  std::string activeUnitPath;
  std::string includeClosureFingerprint;
  std::string activeBranchFingerprint;
  std::string definesFingerprint;
  uint64_t workspaceSummaryVersion = 0;
  std::string fullFingerprint;
};

struct DocumentRuntime {
  std::string uri;
  std::string text;
  int version = 0;
  uint64_t epoch = 0;
  AnalysisSnapshotKey analysisSnapshotKey;
  InteractiveVisibilityKey interactiveVisibilityKey;
  ActiveUnitSnapshot activeUnitSnapshot;
  // existing fields unchanged
};
```

```cpp
// server_cpp/src/document_runtime.cpp
static InteractiveVisibilityKey buildInteractiveVisibilityKey(
    const ActiveUnitSnapshot &activeUnitSnapshot) {
  InteractiveVisibilityKey key;
  key.activeUnitPath = activeUnitSnapshot.path;
  key.includeClosureFingerprint = activeUnitSnapshot.includeClosureFingerprint;
  key.activeBranchFingerprint = activeUnitSnapshot.activeBranchFingerprint;
  key.definesFingerprint = activeUnitSnapshot.definesFingerprint;
  key.workspaceSummaryVersion = activeUnitSnapshot.workspaceSummaryVersion;
  key.fullFingerprint = key.activeUnitPath + "|" + key.includeClosureFingerprint +
                        "|" + key.activeBranchFingerprint + "|" +
                        key.definesFingerprint + "|" +
                        std::to_string(key.workspaceSummaryVersion);
  return key;
}

updated.interactiveVisibilityKey =
    buildInteractiveVisibilityKey(updated.activeUnitSnapshot);
```

```cpp
// server_cpp/src/app/main.cpp (inside nsf/_getDocumentRuntimeDebug)
item.o["interactiveVisibilityFingerprint"] =
    makeString(runtime.interactiveVisibilityKey.fullFingerprint);
```

```cpp
// server_cpp/src/interactive_visibility_runtime.hpp
struct InteractiveVisibleSymbolShard {
  InteractiveVisibilityKey key;
  std::vector<IndexedDefinition> functions;
  std::vector<IndexedDefinition> globals;
  std::vector<IndexedDefinition> types;
};

bool interactiveVisibilityRuntimeGet(const InteractiveVisibilityKey &key,
                                     InteractiveVisibleSymbolShard &out);
void interactiveVisibilityRuntimeInvalidateAll();
```

```cmake
# server_cpp/CMakeLists.txt
  src/interactive_visibility_runtime.cpp
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: PASS and the debug payload now includes `interactiveVisibilityFingerprint`.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/CMakeLists.txt server_cpp/src/document_runtime.hpp server_cpp/src/document_runtime.cpp server_cpp/src/document_owner.hpp server_cpp/src/document_owner.cpp server_cpp/src/interactive_visibility_runtime.hpp server_cpp/src/interactive_visibility_runtime.cpp src/test/suite/integration/interactive-visibility.ts src/test/suite/client.interactive-visibility.test.ts
git commit -m "feat: add interactive visibility runtime skeleton"
```

### Task 3: Build Shared Visible Symbol Shards From Active Unit Context

**Files:**
- Modify: `server_cpp/src/interactive_visibility_runtime.hpp`
- Modify: `server_cpp/src/interactive_visibility_runtime.cpp`
- Modify: `server_cpp/src/document_owner.cpp`
- Modify: `server_cpp/src/workspace_summary_runtime.hpp`
- Modify: `server_cpp/src/workspace_summary_runtime.cpp`
- Modify: `src/test/suite/client.integration.groups.ts`
- Create: `test_files/visibility_root.nsf`
- Create: `test_files/visibility_shared.hlsli`
- Test: `src/test/suite/integration/interactive-visibility.ts`

Controller note:
- Task 3 is allowed to add the smallest possible shared-visible completion consumer hook needed to make the prewarmed shard observable in the completion-driven acceptance test.
- Task 4 still owns the full ordered merge contract (`current -> last-good -> shared-visible -> deferred -> workspace`) and broader shared-visible integration policy.
- If the runtime wrapper for per-URI definition lookup proves to be a fake global-symbol scan, Task 3 is allowed to add the narrowest `workspace_index` helper needed to fetch `FileMeta.defs` by normalized path/URI and rewire the runtime wrapper to use it.
- Task 3 is also allowed to replace `interactiveVisibilityRuntimeInvalidateAll()` on document close with keyed invalidation, as long as broader analysis-context refresh paths keep the full invalidation entrypoint.

- [ ] **Step 1: Write the failing test**

```ts
it('prewarms current-context-visible cross-file function symbols from the active unit include closure', async function () {
	this.timeout(120000);

	await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
		const root = await openFixture('visibility_root.nsf');
		await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());

		const position = positionOf(root, 'VisibleInc', 1, 'VisibleInc'.length);
		const items = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					root.uri,
					position
				),
			(value) => getCompletionItems(value).some((item) => item.label.toString() === 'VisibleIncludeHelper'),
			'cross-file visible completion'
		);

		assert.ok(getCompletionItems(items).some((item) => item.label.toString() === 'VisibleIncludeHelper'));
		const debug = await getInteractiveRuntimeDebug(root.uri.toString());
		assert.strictEqual(debug.lastResolvedLayer, 'shared-visible');
	});
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because the shared visible shard is not built yet.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/interactive_visibility_runtime.hpp
void interactiveVisibilityRuntimePrewarm(const DocumentRuntime &runtime);
bool interactiveVisibilityRuntimeCollectFunctions(
    const InteractiveVisibilityKey &key,
    std::vector<IndexedDefinition> &functionsOut);
```

```cpp
// server_cpp/src/workspace_summary_runtime.hpp
void workspaceSummaryRuntimeQueryDefinitionsByUri(
    const std::string &uri, std::vector<IndexedDefinition> &out);
```

```cpp
// server_cpp/src/workspace_summary_runtime.cpp
void workspaceSummaryRuntimeQueryDefinitionsByUri(
    const std::string &uri, std::vector<IndexedDefinition> &out) {
  out.clear();
  if (uri.empty())
    return;
  std::vector<IndexedDefinition> allDefs;
  workspaceSummaryRuntimeQuerySymbols("", allDefs, 4096);
  for (const auto &def : allDefs) {
    if (def.uri == uri)
      out.push_back(def);
  }
}
```

```cpp
// server_cpp/src/interactive_visibility_runtime.cpp
namespace {
std::mutex gInteractiveVisibilityMutex;
std::unordered_map<std::string, InteractiveVisibleSymbolShard> gShardByFingerprint;
}

void interactiveVisibilityRuntimePrewarm(const DocumentRuntime &runtime) {
  const std::string &fingerprint = runtime.interactiveVisibilityKey.fullFingerprint;
  if (fingerprint.empty())
    return;

  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  if (gShardByFingerprint.find(fingerprint) != gShardByFingerprint.end())
    return;

  InteractiveVisibleSymbolShard shard;
  shard.key = runtime.interactiveVisibilityKey;
  for (const auto &uri : runtime.activeUnitSnapshot.includeClosureUris) {
    std::vector<IndexedDefinition> defs;
    workspaceSummaryRuntimeQueryDefinitionsByUri(uri, defs);
    for (const auto &def : defs) {
      if (def.kind == 12)
        shard.functions.push_back(def);
      else if (def.kind == 13)
        shard.globals.push_back(def);
      else if (def.kind == 23)
        shard.types.push_back(def);
    }
  }
  gShardByFingerprint[fingerprint] = std::move(shard);
}

bool interactiveVisibilityRuntimeCollectFunctions(
    const InteractiveVisibilityKey &key,
    std::vector<IndexedDefinition> &functionsOut) {
  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  auto it = gShardByFingerprint.find(key.fullFingerprint);
  if (it == gShardByFingerprint.end())
    return false;
  functionsOut = it->second.functions;
  return !functionsOut.empty();
}
```

```cpp
// server_cpp/src/document_owner.cpp
if (shouldPrewarm)
  interactiveSemanticRuntimePrewarm(document.uri, document, ctx);

DocumentRuntime runtime;
if (documentRuntimeGet(document.uri, runtime))
  interactiveVisibilityRuntimePrewarm(runtime);
```

```hlsl
// test_files/visibility_shared.hlsli
float4 VisibleIncludeHelper(float3 normal, float amount)
{
    return float4(normal * amount, 1.0);
}
```

```hlsl
// test_files/visibility_root.nsf
#include "visibility_shared.hlsli"

float4 main_ps(float3 normal : NORMAL) : SV_Target
{
    float4 visibilityLocalColor = 1.0;
    return Vis
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: PASS and the debug layer for the include-helper completion is `shared-visible`.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/interactive_visibility_runtime.hpp server_cpp/src/interactive_visibility_runtime.cpp server_cpp/src/document_owner.cpp server_cpp/src/workspace_summary_runtime.hpp server_cpp/src/workspace_summary_runtime.cpp src/test/suite/client.integration.groups.ts src/test/suite/integration/interactive-visibility.ts test_files/visibility_shared.hlsli test_files/visibility_root.nsf
git commit -m "feat: prewarm shared visible symbol shards"
```

### Task 4: Enforce Completion Order For Current + Shared Visible Symbols

**Files:**
- Modify: `server_cpp/src/interactive_semantic_runtime.hpp`
- Modify: `server_cpp/src/interactive_semantic_runtime.cpp`
- Modify: `server_cpp/src/requests/server_request_handler_completion.cpp`
- Test: `src/test/suite/integration/interactive-visibility.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('orders completion results as current, last-good, shared-visible, then workspace fallback', async function () {
	this.timeout(120000);

	await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
		const root = await openFixture('visibility_root.nsf');
		await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());

		const position = positionOf(root, 'return Vis', 1, 'return Vis'.length);
		const result = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					root.uri,
					position
				),
			(value) => getCompletionItems(value).some((item) => item.label.toString() === 'VisibleIncludeHelper'),
			'ordered current/shared-visible completion'
		);

		const labels = getCompletionItems(result).map((item) => item.label.toString());
		assert.ok(labels.indexOf('visibilityLocalColor') >= 0);
		assert.ok(labels.indexOf('VisibleIncludeHelper') >= 0);
		assert.ok(labels.indexOf('VisibleIncludeHelper') > labels.indexOf('visibilityLocalColor'));
	});
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because shared-visible symbols are not yet merged as a formal layer.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/interactive_semantic_runtime.cpp
static void appendCompletionItemsFromVisibleShard(
    const std::vector<IndexedDefinition> &defs,
    std::vector<InteractiveCompletionItem> &outItems) {
  for (const auto &def : defs) {
    if (def.name.empty())
      continue;
    outItems.push_back(
        InteractiveCompletionItem{def.name, def.type, completionKindForWorkspaceDefinition(def)});
  }
}

void interactiveCollectCompletionItems(
    const std::string &uri, const Document &doc, size_t cursorOffset,
    const std::string &prefix, const ServerRequestContext &ctx,
    std::vector<InteractiveCompletionItem> &outItems) {
  DocumentRuntime runtime;
  if (!documentOwnerGetRuntime(uri, runtime))
    return;

  bool usedLastGood = false;
  auto current = getOrBuildInteractiveSnapshot(uri, doc, ctx, &usedLastGood);
  std::shared_ptr<const InteractiveSnapshot> lastGood;
  std::shared_ptr<const DeferredDocSnapshot> deferred;
  collectEligibleSnapshots(runtime, lastGood, deferred);

  if (current && current->semanticSnapshot &&
      appendCompletionItemsFromSnapshot(*current->semanticSnapshot, doc.text, cursorOffset,
                                       prefix, outItems)) {
    recordInteractiveResolutionDebug(uri, "completion", "current", prefix);
  }

  if (lastGood && lastGood->semanticSnapshot &&
      appendCompletionItemsFromSnapshot(*lastGood->semanticSnapshot, doc.text, cursorOffset,
                                       prefix, outItems)) {
    recordInteractiveResolutionDebug(uri, "completion", "last-good", prefix);
  }

  std::vector<IndexedDefinition> visibleFns;
  if (interactiveVisibilityRuntimeCollectFunctions(runtime.interactiveVisibilityKey, visibleFns)) {
    appendCompletionItemsFromVisibleShard(visibleFns, outItems);
    recordInteractiveResolutionDebug(uri, "completion", "shared-visible", prefix);
  }

  if (deferred && deferred->semanticSnapshot) {
    appendCompletionItemsFromSnapshot(*deferred->semanticSnapshot, doc.text, cursorOffset,
                                      prefix, outItems);
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: PASS and the current-doc local symbol appears before the shared-visible include helper.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/interactive_semantic_runtime.hpp server_cpp/src/interactive_semantic_runtime.cpp server_cpp/src/requests/server_request_handler_completion.cpp src/test/suite/integration/interactive-visibility.ts
git commit -m "feat: merge shared visible symbols into p1 completion"
```

### Task 5: Extend `.` / Hover / Signature Help To Shared Visibility And Update Docs

**Files:**
- Modify: `server_cpp/src/interactive_semantic_runtime.cpp`
- Modify: `server_cpp/src/requests/server_request_handler_hover.cpp`
- Modify: `server_cpp/src/requests/server_request_handler_signature.cpp`
- Modify: `docs/architecture.md`
- Modify: `docs/testing.md`
- Create: `test_files/visibility_member_root.nsf`
- Create: `test_files/visibility_member_types.hlsli`
- Test: `src/test/suite/integration/interactive-visibility.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('uses shared-visible cross-file type information for member completion, hover, and signature help', async function () {
	this.timeout(120000);

	await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
		const root = await openFixture('visibility_member_root.nsf');
		await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());

		const memberPos = positionOf(root, 'visibleStruct.', 1, 'visibleStruct.'.length);
		const memberItems = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					root.uri,
					memberPos
				),
			(value) => getCompletionItems(value).some((item) => item.label.toString() === 'SharedVisibleField'),
			'shared-visible member completion'
		);
		assert.ok(getCompletionItems(memberItems).some((item) => item.label.toString() === 'SharedVisibleField'));

		const hoverPos = positionOf(root, 'SharedVisibleHelper', 1, 2);
		const hoverText = hoverToText(
			await waitForHoverText(
				root,
				hoverPos,
				(text) => text.includes('SharedVisibleHelper') && text.includes('float4'),
				'shared-visible hover'
			)
		);
		assert.ok(hoverText.includes('SharedVisibleHelper'));

		const sigPos = positionOf(root, 'SharedVisibleHelper(', 1, 'SharedVisibleHelper('.length);
		const sig = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					root.uri,
					sigPos
				),
			(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
			'shared-visible signature help'
		);
		assert.ok(sig.signatures.some((item) => item.label.includes('SharedVisibleHelper')));
	});
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because `.` / hover / signature help do not yet consult shared-visible shards.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/interactive_visibility_runtime.hpp
bool interactiveVisibilityRuntimeCollectTypes(
    const InteractiveVisibilityKey &key,
    std::vector<IndexedDefinition> &typesOut);
bool interactiveVisibilityRuntimeCollectFunctions(
    const InteractiveVisibilityKey &key,
    std::vector<IndexedDefinition> &functionsOut);
```

```cpp
// server_cpp/src/interactive_visibility_runtime.cpp
bool interactiveVisibilityRuntimeCollectTypes(
    const InteractiveVisibilityKey &key,
    std::vector<IndexedDefinition> &typesOut) {
  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  auto it = gShardByFingerprint.find(key.fullFingerprint);
  if (it == gShardByFingerprint.end())
    return false;
  typesOut = it->second.types;
  return !typesOut.empty();
}
```

```cpp
// server_cpp/src/interactive_semantic_runtime.cpp
MemberAccessBaseTypeResult interactiveResolveMemberAccessBaseType(
    const std::string &uri, const Document &doc, const std::string &base,
    size_t cursorOffset, const ServerRequestContext &ctx,
    const MemberAccessBaseTypeOptions &options) {
  DocumentRuntime runtime;
  if (!documentOwnerGetRuntime(uri, runtime))
    return MemberAccessBaseTypeResult{};

  // existing current / last-good / deferred checks stay first

  std::vector<IndexedDefinition> visibleTypes;
  if (interactiveVisibilityRuntimeCollectTypes(runtime.interactiveVisibilityKey, visibleTypes)) {
    for (const auto &def : visibleTypes) {
      if (def.name == base || def.type == base) {
        recordInteractiveResolutionDebug(uri, "member-base", "shared-visible", base);
        return MemberAccessBaseTypeResult{def.type, true, ""};
      }
    }
  }

  return MemberAccessBaseTypeResult{};
}

bool interactiveResolveFunctionOverloads(
    const std::string &uri, const Document &doc, const std::string &name,
    const ServerRequestContext &ctx,
    std::vector<SemanticSnapshotFunctionOverloadInfo> &overloadsOut) {
  // existing current / last-good / deferred checks stay first
  DocumentRuntime runtime;
  if (documentOwnerGetRuntime(uri, runtime)) {
    std::vector<IndexedDefinition> visibleFns;
    if (interactiveVisibilityRuntimeCollectFunctions(runtime.interactiveVisibilityKey, visibleFns)) {
      for (const auto &def : visibleFns) {
        if (def.name == name) {
          overloadsOut.push_back(SemanticSnapshotFunctionOverloadInfo{
              def.name, def.type, {}, def.uri, def.line, def.start});
        }
      }
      if (!overloadsOut.empty()) {
        recordInteractiveResolutionDebug(uri, "signature-help", "shared-visible", name);
        return true;
      }
    }
  }
  return false;
}
```

```md
<!-- docs/architecture.md -->
- `interactive_visibility_runtime.*` 作为 `P1 Interactive` 的共享可见性层，负责 active unit / include closure / branch / defines 约束下的 cross-file-visible symbols。
- completion、hover、signature help、`.` member 相关查询现在必须遵循 `current -> last-good -> shared-visible -> deferred -> workspace`。
```

```md
<!-- docs/testing.md -->
- `client.interactive-visibility.test.ts` 承担 `P1` shared-visible 验收，至少覆盖变量/函数补全、`.` member completion、hover、signature help 四类交互。
- 对这些用例优先使用 `nsf._getInteractiveRuntimeDebug` 断言实际命中的层级，而不是只根据结果文本间接推测。
```

```hlsl
// test_files/visibility_member_types.hlsli
struct SharedVisibleStruct {
    float4 SharedVisibleField;
};

float4 SharedVisibleHelper(float4 baseColor, float amount)
{
    return baseColor * amount;
}
```

```hlsl
// test_files/visibility_member_root.nsf
#include "visibility_member_types.hlsli"

float4 main_ps() : SV_Target
{
    SharedVisibleStruct visibleStruct;
    visibleStruct.
    return SharedVisibleHelper(float4(1, 1, 1, 1), 0.5);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.interactive-visibility'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
npm run test:client:repo
```

Expected: PASS for the new shared-visible interaction tests and PASS for the full repo regression suite.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/interactive_semantic_runtime.cpp server_cpp/src/requests/server_request_handler_hover.cpp server_cpp/src/requests/server_request_handler_signature.cpp docs/architecture.md docs/testing.md src/test/suite/integration/interactive-visibility.ts src/test/suite/client.interactive-visibility.test.ts src/test/suite/client.integration.groups.ts test_files/visibility_member_types.hlsli test_files/visibility_member_root.nsf
git commit -m "feat: extend p1 shared visibility across interactive queries"
```

## Self-Review

- Spec coverage:
  - `P1` source-order observability: Task 1
  - visibility key + runtime skeleton: Task 2
  - shared-visible shards from active unit context: Task 3
  - current + shared-visible completion merge: Task 4
  - member/hover/signature and docs/tests: Task 5
- Placeholder scan:
  - No `TODO`, `TBD`, “implement later”, or “similar to previous task” instructions remain.
  - Each task includes concrete file paths, code, commands, and expected outcomes.
- Type consistency:
  - `InteractiveVisibilityKey`, `InteractiveVisibleSymbolShard`, `interactiveVisibilityRuntimePrewarm`, and `nsf._getInteractiveRuntimeDebug` are used consistently across the plan.
