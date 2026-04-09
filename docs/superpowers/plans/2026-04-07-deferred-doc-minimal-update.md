# Deferred Doc Minimal Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let small edits and visible-range background requests reuse a current-version deferred semantic core and invalidate only overlapping deferred range caches, so `semanticTokens/range` and `textDocument/inlayHint` stop forcing whole-document deferred rebuilds.

**Architecture:** Keep `document_owner.*`, `document_runtime.*`, and `AnalysisSnapshotKey` as the single owner and invalidation boundary. Split `DeferredDocSnapshot` materialization into a semantic core (`astDocument` + `semanticSnapshot`) plus lazy artifacts, then add per-range caches for semantic tokens and inlay hints keyed by requested line windows. Use existing `changedRanges` to invalidate only overlapping range buckets while full-document artifacts remain latest-only background work.

**Tech Stack:** C++17 LSP server, deferred/current-doc runtime modules under `server_cpp/src/`, VS Code integration tests in TypeScript/Mocha, internal debug command `nsf._getDocumentRuntimeDebug`, Markdown facts docs.

---

## Scope Note

This plan intentionally covers only the first executable sub-project of the broader “cache supports minimal update” effort.

- In scope:
  - Split deferred runtime into semantic core vs lazy artifacts
  - Add semantic-token range cache
  - Add inlay-hint range cache
  - Invalidate only overlapping range caches on `didChange`
  - Add test-visible debug fields and update facts docs
- Out of scope:
  - Interactive function-slice cache
  - Workspace summary / reverse-include behavior changes
  - Incremental AST or incremental preprocessor parser

## File Map

- `server_cpp/src/document_runtime.hpp`
  - Extend `DeferredDocSnapshot` state with range-cache containers that survive stale-eligible reuse across edits.
- `server_cpp/src/document_runtime.cpp`
  - When reusing a stale-eligible deferred snapshot after `didChange`, invalidate only overlapping range-cache entries and clear any full-document artifacts that are no longer current.
- `server_cpp/src/deferred_doc_runtime.hpp`
  - Declare semantic-core builders, range-cache entry structs, and lazy artifact helpers.
- `server_cpp/src/deferred_doc_runtime.cpp`
  - Split eager whole-snapshot build into semantic core + on-demand artifact builders; store/reuse range caches; preserve copied caches when cloning snapshots.
- `server_cpp/src/inlay_hints_runtime.cpp`
  - Reuse deferred inlay range cache first, then build/store a range miss; clear both full and range inlay caches when slow parameter resolution changes.
- `server_cpp/src/requests/server_request_handler_background.cpp`
  - Keep handlers thin, but update any helper names if the deferred APIs change.
- `server_cpp/src/app/main.cpp`
  - Extend `nsf._getDocumentRuntimeDebug` to expose deferred artifact/range-cache state for deterministic tests.
- `src/test/suite/test_helpers.ts`
  - Add typed access to new debug fields returned by `nsf._getDocumentRuntimeDebug`.
- `src/test/suite/integration/deferred-doc.ts`
  - Add repo-mode tests for lazy semantic-core materialization, semantic-token range cache invalidation, inlay range cache invalidation, and coexistence with full-document artifacts.
- `docs/architecture.md`
  - Record deferred semantic-core vs lazy-artifact runtime boundaries and overlap-only range invalidation.
- `docs/testing.md`
  - Record which targeted suites verify deferred range caches and how to assert overlap vs non-overlap behavior.

### Task 1: Add Deferred Runtime Observability

**Files:**
- Modify: `server_cpp/src/document_runtime.hpp`
- Modify: `server_cpp/src/app/main.cpp`
- Modify: `src/test/suite/test_helpers.ts`
- Test: `src/test/suite/integration/deferred-doc.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('reports deferred artifact state in document runtime debug', async function () {
	this.timeout(120000);
	await vscode.commands.executeCommand('nsf.restartServer');
	const document = await openFixture('module_semantic_tokens.nsf');

	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				document.uri
			),
		(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
		'semantic tokens for deferred debug surface'
	);

	const [runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.ok(runtime?.hasDeferredDocSnapshot, 'Expected a deferred snapshot to exist.');
	assert.strictEqual(typeof runtime?.deferredHasSemanticSnapshot, 'boolean');
	assert.strictEqual(typeof runtime?.deferredHasSemanticTokensFull, 'boolean');
	assert.strictEqual(typeof runtime?.deferredHasDocumentSymbols, 'boolean');
	assert.strictEqual(typeof runtime?.deferredHasFullDiagnostics, 'boolean');
	assert.strictEqual(typeof runtime?.deferredHasInlayHintsFull, 'boolean');
	assert.strictEqual(typeof runtime?.deferredSemanticTokensRangeCacheCount, 'number');
	assert.strictEqual(typeof runtime?.deferredInlayRangeCacheCount, 'number');
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because `DocumentRuntimeDebugEntry` and `nsf._getDocumentRuntimeDebug` do not yet expose the deferred artifact fields.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/document_runtime.hpp
struct DeferredRangeCacheEntry {
  int startLine = 0;
  int endLine = 0;
  Json value;
};

struct DeferredDocSnapshot {
  AnalysisSnapshotKey key;
  uint64_t documentEpoch = 0;
  int documentVersion = 0;
  std::shared_ptr<const HlslAstDocument> astDocument;
  std::shared_ptr<const SemanticSnapshot> semanticSnapshot;
  Json fullDiagnostics;
  bool hasFullDiagnostics = false;
  std::string fullDiagnosticsFingerprint;
  Json semanticTokensFull;
  bool hasSemanticTokensFull = false;
  Json inlayHintsFull;
  bool hasInlayHintsFull = false;
  Json documentSymbols;
  bool hasDocumentSymbols = false;
  std::vector<DeferredRangeCacheEntry> semanticTokensRangeCache;
  std::vector<DeferredRangeCacheEntry> inlayHintsRangeCache;
  uint64_t builtAtMs = 0;
};
```

```cpp
// server_cpp/src/app/main.cpp
if (runtime.deferredDocSnapshot) {
  item.o["deferredAnalysisFullFingerprint"] =
      makeString(runtime.deferredDocSnapshot->key.fullFingerprint);
  item.o["deferredAnalysisStableFingerprint"] =
      makeString(runtime.deferredDocSnapshot->key.stableContextFingerprint);
  item.o["deferredHasSemanticSnapshot"] =
      makeBool(static_cast<bool>(runtime.deferredDocSnapshot->semanticSnapshot));
  item.o["deferredHasSemanticTokensFull"] =
      makeBool(runtime.deferredDocSnapshot->hasSemanticTokensFull);
  item.o["deferredHasDocumentSymbols"] =
      makeBool(runtime.deferredDocSnapshot->hasDocumentSymbols);
  item.o["deferredHasFullDiagnostics"] =
      makeBool(runtime.deferredDocSnapshot->hasFullDiagnostics);
  item.o["deferredHasInlayHintsFull"] =
      makeBool(runtime.deferredDocSnapshot->hasInlayHintsFull);
  item.o["deferredSemanticTokensRangeCacheCount"] = makeNumber(
      static_cast<double>(runtime.deferredDocSnapshot->semanticTokensRangeCache.size()));
  item.o["deferredInlayRangeCacheCount"] = makeNumber(
      static_cast<double>(runtime.deferredDocSnapshot->inlayHintsRangeCache.size()));
}
```

```ts
// src/test/suite/test_helpers.ts
export type DocumentRuntimeDebugEntry = {
	uri: string;
	exists: boolean;
	version?: number;
	epoch?: number;
	analysisFullFingerprint?: string;
	analysisStableFingerprint?: string;
	workspaceSummaryVersion?: number;
	activeUnitPath?: string;
	activeUnitIncludeClosureFingerprint?: string;
	activeUnitBranchFingerprint?: string;
	activeUnitWorkspaceSummaryVersion?: number;
	hasInteractiveSnapshot?: boolean;
	hasLastGoodInteractiveSnapshot?: boolean;
	hasDeferredDocSnapshot?: boolean;
	interactiveAnalysisFullFingerprint?: string;
	interactiveAnalysisStableFingerprint?: string;
	lastGoodAnalysisFullFingerprint?: string;
	deferredAnalysisFullFingerprint?: string;
	deferredAnalysisStableFingerprint?: string;
	deferredHasSemanticSnapshot?: boolean;
	deferredHasSemanticTokensFull?: boolean;
	deferredHasDocumentSymbols?: boolean;
	deferredHasFullDiagnostics?: boolean;
	deferredHasInlayHintsFull?: boolean;
	deferredSemanticTokensRangeCacheCount?: number;
	deferredInlayRangeCacheCount?: number;
	changedRangesCount?: number;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: PASS for the new debug-surface test.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/document_runtime.hpp server_cpp/src/app/main.cpp src/test/suite/test_helpers.ts src/test/suite/integration/deferred-doc.ts
git commit -m "test: expose deferred runtime artifact state"
```

### Task 2: Split Deferred Semantic Core From Lazy Artifacts

**Files:**
- Modify: `server_cpp/src/deferred_doc_runtime.hpp`
- Modify: `server_cpp/src/deferred_doc_runtime.cpp`
- Modify: `server_cpp/src/requests/server_request_handler_background.cpp`
- Test: `src/test/suite/integration/deferred-doc.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('keeps range semantic token requests lazy and avoids eager full deferred artifacts', async function () {
	this.timeout(120000);
	await vscode.commands.executeCommand('nsf.restartServer');
	const document = await openFixture('module_semantic_tokens.nsf');
	const range = new vscode.Range(new vscode.Position(0, 0), new vscode.Position(6, 0));

	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentRangeSemanticTokens',
				document.uri,
				range
			),
		(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
		'range semantic tokens without eager deferred full build'
	);

	const [runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.ok(runtime?.hasDeferredDocSnapshot, 'Expected deferred snapshot after range request.');
	assert.strictEqual(runtime?.deferredHasSemanticSnapshot, true);
	assert.strictEqual(runtime?.deferredHasSemanticTokensFull, false);
	assert.strictEqual(runtime?.deferredHasDocumentSymbols, false);
	assert.strictEqual(runtime?.deferredHasFullDiagnostics, false);
	assert.strictEqual(runtime?.deferredSemanticTokensRangeCacheCount, 0);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because `buildDeferredSemanticTokensRange(...)` still reaches the eager whole-snapshot builder.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/deferred_doc_runtime.hpp
std::shared_ptr<const DeferredDocSnapshot> ensureDeferredSemanticCore(
    const std::string &uri, const Document &doc, const ServerRequestContext &ctx);
```

```cpp
// server_cpp/src/deferred_doc_runtime.cpp
static std::shared_ptr<const DeferredDocSnapshot> buildDeferredSemanticCoreFromInputs(
    const Document &doc, const DeferredDocBuildContext &context,
    const AnalysisSnapshotKey &analysisKey) {
  auto deferred = std::make_shared<DeferredDocSnapshot>();
  deferred->key = analysisKey;
  deferred->documentEpoch = doc.epoch;
  deferred->documentVersion = doc.version;
  deferred->astDocument = std::make_shared<HlslAstDocument>(
      buildHlslAstDocument(buildLinePreservingExpandedSource(doc.text, context.defines)));
  deferred->semanticSnapshot = getSemanticSnapshotView(
      doc.uri, doc.text, doc.epoch, context.workspaceFolders,
      context.includePaths, context.shaderExtensions, context.defines);
  deferred->builtAtMs = currentTimeMs();
  return deferred;
}

std::shared_ptr<const DeferredDocSnapshot> ensureDeferredSemanticCore(
    const std::string &uri, const Document &doc, const ServerRequestContext &ctx) {
  DocumentRuntime runtime;
  if (!documentOwnerGetRuntime(uri, runtime))
    return nullptr;
  if (runtime.deferredDocSnapshot &&
      runtime.deferredDocSnapshot->key.fullFingerprint ==
          runtime.analysisSnapshotKey.fullFingerprint &&
      runtime.deferredDocSnapshot->documentEpoch == doc.epoch &&
      runtime.deferredDocSnapshot->documentVersion == doc.version &&
      runtime.deferredDocSnapshot->astDocument &&
      runtime.deferredDocSnapshot->semanticSnapshot) {
    return runtime.deferredDocSnapshot;
  }

  DeferredDocBuildContext buildContext;
  buildContext.workspaceFolders = runtime.activeUnitSnapshot.workspaceFolders;
  buildContext.includePaths = runtime.activeUnitSnapshot.includePaths;
  buildContext.shaderExtensions = runtime.activeUnitSnapshot.shaderExtensions;
  buildContext.defines = runtime.activeUnitSnapshot.defines;
  if (buildContext.workspaceFolders.empty() && buildContext.includePaths.empty() &&
      buildContext.shaderExtensions.empty() && buildContext.defines.empty()) {
    buildContext.workspaceFolders = ctx.workspaceFolders;
    buildContext.includePaths = ctx.includePaths;
    buildContext.shaderExtensions = ctx.shaderExtensions;
    buildContext.defines = ctx.preprocessorDefines;
  }

  auto deferred =
      buildDeferredSemanticCoreFromInputs(doc, buildContext, runtime.analysisSnapshotKey);
  documentOwnerStoreDeferredSnapshot(uri, deferred);
  return deferred;
}
```

```cpp
// server_cpp/src/deferred_doc_runtime.cpp
Json buildDeferredSemanticTokensRange(const std::string &uri, const Document &doc,
                                      int startLine, int startCharacter,
                                      int endLine, int endCharacter,
                                      const ServerRequestContext &ctx) {
  ensureDeferredSemanticCore(uri, doc, ctx);
  return buildSemanticTokensRange(doc.text, startLine, startCharacter, endLine,
                                  endCharacter, ctx.semanticLegend);
}

Json buildDeferredDocumentSymbols(const std::string &uri, const Document &doc,
                                  const ServerRequestContext &ctx) {
  auto deferred = ensureDeferredSemanticCore(uri, doc, ctx);
  if (!deferred)
    return makeArray();
  if (deferred->hasDocumentSymbols)
    return deferred->documentSymbols;

  auto writable = std::make_shared<DeferredDocSnapshot>(*deferred);
  writable->documentSymbols =
      buildDocumentSymbolsFromAst(doc.text, writable->astDocument.get(),
                                  writable->semanticSnapshot.get());
  writable->hasDocumentSymbols = true;
  writable->builtAtMs = currentTimeMs();
  documentOwnerStoreDeferredSnapshot(uri, writable);
  return writable->documentSymbols;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: PASS for the lazy semantic-core test; `deferredHasSemanticTokensFull` and `deferredHasDocumentSymbols` stay `false` after a range semantic-token request.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/deferred_doc_runtime.hpp server_cpp/src/deferred_doc_runtime.cpp server_cpp/src/requests/server_request_handler_background.cpp src/test/suite/integration/deferred-doc.ts
git commit -m "refactor: split deferred semantic core from lazy artifacts"
```

### Task 3: Cache Semantic Token Ranges And Invalidate Only Overlap

**Files:**
- Modify: `server_cpp/src/document_runtime.cpp`
- Modify: `server_cpp/src/deferred_doc_runtime.cpp`
- Modify: `server_cpp/src/deferred_doc_runtime.hpp`
- Test: `src/test/suite/integration/deferred-doc.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('retains non-overlapping semantic token range caches and drops overlapping ones on edit', async function () {
	this.timeout(120000);
	await vscode.commands.executeCommand('nsf.restartServer');
	let document = await openFixture('module_perf_large_current_doc.nsf');
	const range = new vscode.Range(new vscode.Position(40, 0), new vscode.Position(70, 0));

	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentRangeSemanticTokens',
				document.uri,
				range
			),
		(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
		'seed semantic token range cache'
	);

	let [runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.strictEqual(runtime?.deferredSemanticTokensRangeCacheCount, 1);

	const farInsert = new vscode.WorkspaceEdit();
	farInsert.insert(document.uri, new vscode.Position(180, 0), " \n");
	assert.ok(await vscode.workspace.applyEdit(farInsert));
	document = await vscode.workspace.openTextDocument(document.uri);

	[runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.strictEqual(runtime?.deferredSemanticTokensRangeCacheCount, 1);

	const nearInsert = new vscode.WorkspaceEdit();
	nearInsert.insert(document.uri, new vscode.Position(50, 0), " \n");
	assert.ok(await vscode.workspace.applyEdit(nearInsert));
	document = await vscode.workspace.openTextDocument(document.uri);

	[runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.strictEqual(runtime?.deferredSemanticTokensRangeCacheCount, 0);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because the range request is not cached and the reused deferred snapshot does not invalidate by overlap.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/deferred_doc_runtime.cpp
static bool tryFindRangeCacheEntry(const std::vector<DeferredRangeCacheEntry> &entries,
                                   int startLine, int endLine, Json &valueOut) {
  for (const auto &entry : entries) {
    if (entry.startLine == startLine && entry.endLine == endLine) {
      valueOut = entry.value;
      return true;
    }
  }
  return false;
}

static void storeRangeCacheEntry(std::vector<DeferredRangeCacheEntry> &entries,
                                 int startLine, int endLine, const Json &value) {
  for (auto &entry : entries) {
    if (entry.startLine == startLine && entry.endLine == endLine) {
      entry.value = value;
      return;
    }
  }
  entries.push_back(DeferredRangeCacheEntry{startLine, endLine, value});
}

Json buildDeferredSemanticTokensRange(const std::string &uri, const Document &doc,
                                      int startLine, int startCharacter,
                                      int endLine, int endCharacter,
                                      const ServerRequestContext &ctx) {
  auto deferred = ensureDeferredSemanticCore(uri, doc, ctx);
  if (!deferred)
    return buildSemanticTokensRange(doc.text, startLine, startCharacter, endLine,
                                    endCharacter, ctx.semanticLegend);

  Json cached;
  const int normalizedStart = std::min(startLine, endLine);
  const int normalizedEnd = std::max(startLine, endLine);
  if (tryFindRangeCacheEntry(deferred->semanticTokensRangeCache, normalizedStart,
                             normalizedEnd, cached)) {
    return cached;
  }

  Json tokens = buildSemanticTokensRange(doc.text, startLine, startCharacter, endLine,
                                         endCharacter, ctx.semanticLegend);
  auto writable = std::make_shared<DeferredDocSnapshot>(*deferred);
  storeRangeCacheEntry(writable->semanticTokensRangeCache, normalizedStart,
                       normalizedEnd, tokens);
  writable->builtAtMs = currentTimeMs();
  documentOwnerStoreDeferredSnapshot(uri, writable);
  return tokens;
}
```

```cpp
// server_cpp/src/document_runtime.cpp
static std::pair<int, int> computeDeferredChangedWindow(
    const std::string &text, const std::vector<ChangedRange> &changedRanges) {
  const int lineCount = std::max(1, static_cast<int>(splitLinesShared(text).size()));
  if (changedRanges.empty())
    return {0, lineCount - 1};

  int startLine = lineCount - 1;
  int endLine = 0;
  for (const auto &range : changedRanges) {
    startLine = std::min(startLine, std::max(0, range.startLine));
    endLine = std::max(endLine,
                       std::min(lineCount - 1,
                                std::max(range.endLine, range.startLine + range.newEndLine)));
  }
  if (endLine < startLine)
    endLine = startLine;
  return {startLine, endLine};
}

static void invalidateOverlappingDeferredRanges(
    std::vector<DeferredRangeCacheEntry> &entries, int changedStartLine,
    int changedEndLine) {
  entries.erase(
      std::remove_if(entries.begin(), entries.end(),
                     [&](const DeferredRangeCacheEntry &entry) {
                       return !(entry.endLine < changedStartLine ||
                                entry.startLine > changedEndLine);
                     }),
      entries.end());
}

if (existing.deferredDocSnapshot &&
    isSnapshotStaleEligible(existing.deferredDocSnapshot->key,
                            updated.analysisSnapshotKey)) {
  auto writable = std::make_shared<DeferredDocSnapshot>(*existing.deferredDocSnapshot);
  const auto [changedStartLine, changedEndLine] =
      computeDeferredChangedWindow(document.text, changedRanges);
  invalidateOverlappingDeferredRanges(writable->semanticTokensRangeCache,
                                      changedStartLine, changedEndLine);
  writable->semanticTokensFull = makeArray();
  writable->hasSemanticTokensFull = false;
  writable->fullDiagnostics = makeArray();
  writable->hasFullDiagnostics = false;
  updated.deferredDocSnapshot = writable;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: PASS; the far edit keeps `deferredSemanticTokensRangeCacheCount == 1`, and the overlapping edit drops it to `0`.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/document_runtime.cpp server_cpp/src/deferred_doc_runtime.cpp server_cpp/src/deferred_doc_runtime.hpp src/test/suite/integration/deferred-doc.ts
git commit -m "feat: cache semantic token ranges by overlap"
```

### Task 4: Cache Inlay Ranges And Clear Them Precisely

**Files:**
- Modify: `server_cpp/src/inlay_hints_runtime.cpp`
- Modify: `server_cpp/src/document_runtime.cpp`
- Modify: `server_cpp/src/deferred_doc_runtime.cpp`
- Test: `src/test/suite/integration/deferred-doc.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('retains non-overlapping inlay range caches and clears them on overlapping edits', async function () {
	this.timeout(120000);
	await vscode.commands.executeCommand('nsf.restartServer');
	let document = await openFixture('main.nsf');
	const range = new vscode.Range(new vscode.Position(168, 0), new vscode.Position(214, 0));

	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.InlayHint[]>(
				'vscode.executeInlayHintProvider',
				document.uri,
				range
			),
		(value) => Array.isArray(value) && value.length > 0,
		'seed inlay range cache'
	);

	let [runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.strictEqual(runtime?.deferredInlayRangeCacheCount, 1);

	const farInsert = new vscode.WorkspaceEdit();
	farInsert.insert(document.uri, new vscode.Position(20, 0), "// far edit\n");
	assert.ok(await vscode.workspace.applyEdit(farInsert));
	document = await vscode.workspace.openTextDocument(document.uri);

	[runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.strictEqual(runtime?.deferredInlayRangeCacheCount, 1);

	const nearInsert = new vscode.WorkspaceEdit();
	nearInsert.insert(document.uri, new vscode.Position(171, 0), "// near edit\n");
	assert.ok(await vscode.workspace.applyEdit(nearInsert));
	document = await vscode.workspace.openTextDocument(document.uri);

	[runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.strictEqual(runtime?.deferredInlayRangeCacheCount, 0);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL because the inlay request currently builds a range on demand but does not cache it for reuse or invalidate by overlap.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/inlay_hints_runtime.cpp
static bool tryGetDeferredInlayRangeCache(const DeferredDocSnapshot &snapshot,
                                          int startLine, int endLine,
                                          Json &valueOut) {
  for (const auto &entry : snapshot.inlayHintsRangeCache) {
    if (entry.startLine == startLine && entry.endLine == endLine) {
      valueOut = entry.value;
      return true;
    }
  }
  return false;
}

static void storeDeferredInlayRangeCache(DeferredDocSnapshot &snapshot,
                                         int startLine, int endLine,
                                         const Json &value) {
  for (auto &entry : snapshot.inlayHintsRangeCache) {
    if (entry.startLine == startLine && entry.endLine == endLine) {
      entry.value = value;
      return;
    }
  }
  snapshot.inlayHintsRangeCache.push_back(
      DeferredRangeCacheEntry{startLine, endLine, value});
}

Json inlayHintsRuntimeBuildRange(const std::string &uri, const Document &doc,
                                 ServerRequestContext &ctx, int startLine,
                                 int startChar, int endLine, int endChar) {
  if (auto deferred = ensureDeferredSemanticCore(uri, doc, ctx)) {
    Json cached;
    const int normalizedStart = std::min(startLine, endLine);
    const int normalizedEnd = std::max(startLine, endLine);
    if (tryGetDeferredInlayRangeCache(*deferred, normalizedStart, normalizedEnd,
                                      cached)) {
      return cached;
    }

    Json hints = buildInlayHintsForOffsets(
        uri, doc, ctx,
        positionToOffsetUtf16(doc.text, startLine, startChar),
        positionToOffsetUtf16(doc.text, endLine, endChar),
        normalizedStart, normalizedEnd, 160);
    auto writable = std::make_shared<DeferredDocSnapshot>(*deferred);
    storeDeferredInlayRangeCache(*writable, normalizedStart, normalizedEnd, hints);
    documentOwnerStoreDeferredSnapshot(uri, writable);
    return hints;
  }

  return makeArray();
}
```

```cpp
// server_cpp/src/document_runtime.cpp
if (updated.deferredDocSnapshot) {
  auto writable = std::make_shared<DeferredDocSnapshot>(*updated.deferredDocSnapshot);
  const auto [changedStartLine, changedEndLine] =
      computeDeferredChangedWindow(document.text, changedRanges);
  invalidateOverlappingDeferredRanges(writable->inlayHintsRangeCache,
                                      changedStartLine, changedEndLine);
  writable->inlayHintsFull = makeArray();
  writable->hasInlayHintsFull = false;
  updated.deferredDocSnapshot = writable;
}
```

```cpp
// server_cpp/src/deferred_doc_runtime.cpp
void deferredDocRuntimeInvalidateInlayHints(const std::string &uri) {
  DocumentRuntime runtime;
  if (!documentOwnerGetRuntime(uri, runtime) || !runtime.deferredDocSnapshot)
    return;
  auto writable = std::make_shared<DeferredDocSnapshot>(*runtime.deferredDocSnapshot);
  writable->inlayHintsFull = makeArray();
  writable->hasInlayHintsFull = false;
  writable->inlayHintsRangeCache.clear();
  writable->builtAtMs = currentTimeMs();
  documentOwnerStoreDeferredSnapshot(uri, writable);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: PASS; non-overlapping edits leave the inlay range cache intact, and overlapping edits clear it.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/inlay_hints_runtime.cpp server_cpp/src/document_runtime.cpp server_cpp/src/deferred_doc_runtime.cpp src/test/suite/integration/deferred-doc.ts
git commit -m "feat: cache inlay ranges by overlap"
```

### Task 5: Preserve Full-Artifact Behavior And Update Facts Docs

**Files:**
- Modify: `server_cpp/src/deferred_doc_runtime.cpp`
- Modify: `docs/architecture.md`
- Modify: `docs/testing.md`
- Test: `src/test/suite/integration/deferred-doc.ts`

- [ ] **Step 1: Write the failing test**

```ts
it('materializes full deferred artifacts on demand without discarding range caches', async function () {
	this.timeout(120000);
	await vscode.commands.executeCommand('nsf.restartServer');
	const document = await openFixture('module_semantic_tokens.nsf');
	const range = new vscode.Range(new vscode.Position(0, 0), new vscode.Position(6, 0));

	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentRangeSemanticTokens',
				document.uri,
				range
			),
		(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
		'seed semantic token range cache before full artifact build'
	);

	let [runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.strictEqual(runtime?.deferredSemanticTokensRangeCacheCount, 1);
	assert.strictEqual(runtime?.deferredHasSemanticTokensFull, false);

	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				document.uri
			),
		(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
		'full semantic tokens after range cache seed'
	);
	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.DocumentSymbol[]>(
				'vscode.executeDocumentSymbolProvider',
				document.uri
			),
		(value) => Array.isArray(value) && value.length > 0,
		'document symbols after range cache seed'
	);

	[runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
	assert.strictEqual(runtime?.deferredHasSemanticTokensFull, true);
	assert.strictEqual(runtime?.deferredHasDocumentSymbols, true);
	assert.strictEqual(runtime?.deferredSemanticTokensRangeCacheCount, 1);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
```

Expected: FAIL if any full-artifact builder replaces the snapshot without copying over the seeded range caches.

- [ ] **Step 3: Write minimal implementation**

```cpp
// server_cpp/src/deferred_doc_runtime.cpp
Json buildDeferredSemanticTokensFull(const std::string &uri, const Document &doc,
                                     const ServerRequestContext &ctx) {
  auto deferred = ensureDeferredSemanticCore(uri, doc, ctx);
  if (!deferred)
    return buildSemanticTokensFull(doc.text, ctx.semanticLegend);
  if (deferred->hasSemanticTokensFull)
    return deferred->semanticTokensFull;

  auto writable = std::make_shared<DeferredDocSnapshot>(*deferred);
  writable->semanticTokensFull = buildSemanticTokensFull(doc.text, ctx.semanticLegend);
  writable->hasSemanticTokensFull = true;
  writable->builtAtMs = currentTimeMs();
  documentOwnerStoreDeferredSnapshot(uri, writable);
  return writable->semanticTokensFull;
}
```

```md
<!-- docs/architecture.md -->
- `deferred_doc_runtime.*` 现在应区分 semantic core（`astDocument` + `semanticSnapshot`）与 lazy artifacts（`semanticTokens full/range`、`inlay hints full/range`、`document symbols`、`full diagnostics`）。
- `semanticTokens/range` 与 `textDocument/inlayHint` 应优先复用同分析 key 下的 deferred range cache；`didChange` 后仅失效 changed window 相交的 range bucket，full artifacts 继续按后台 latest-only 补齐。
```

```md
<!-- docs/testing.md -->
- 当改动涉及 deferred range cache 时，至少补跑 `npm run test:client:repo`，并新增或更新 `client.deferred-doc-runtime.test.ts` 对应的 integration 用例，显式断言“非重叠 edit 保留 cache、重叠 edit 清空 cache”。
- 对这类用例优先使用 `getDocumentRuntimeDebug(...)` 验证 deferred cache state，不要只靠 wall-clock 或“结果非空”间接猜测。
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```powershell
npm run compile
cmake --build .\server_cpp\build
$env:NSF_TEST_FILE_FILTER='client.deferred-doc-runtime'
npm run test:client:repo
Remove-Item Env:NSF_TEST_FILE_FILTER
npm run test:client:repo
```

Expected: PASS for the new coexistence test and PASS for the full repo-mode regression suite.

- [ ] **Step 5: Commit**

```bash
git add server_cpp/src/deferred_doc_runtime.cpp docs/architecture.md docs/testing.md src/test/suite/integration/deferred-doc.ts
git commit -m "docs: record deferred minimal-update runtime"
```

## Self-Review

- Spec coverage:
  - Deferred semantic core split: Task 2
  - Semantic-token range cache + overlap invalidation: Task 3
  - Inlay range cache + overlap invalidation: Task 4
  - Debug visibility for deterministic tests: Task 1
  - Facts docs and regression verification: Task 5
- Placeholder scan:
  - No `TODO`, `TBD`, or “similar to previous task” instructions remain.
  - Every code-changing step includes concrete snippets and exact commands.
- Type consistency:
  - `DeferredRangeCacheEntry`, `ensureDeferredSemanticCore(...)`, `deferredSemanticTokensRangeCacheCount`, and `deferredInlayRangeCacheCount` use one spelling throughout the plan.

## Follow-Up Plans

Do not expand this plan mid-flight. After this plan lands and repo tests are green, write separate plans for:

1. Interactive function-slice cache / local-symbol minimal rebuild
2. Document-symbol and diagnostics artifact-granular invalidation
3. Any future incremental AST or preprocessor work
