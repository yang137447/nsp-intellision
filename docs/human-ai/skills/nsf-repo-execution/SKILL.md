---
name: nsf-repo-execution
description: Evidence-first workflow for diagnosing, reviewing, or implementing changes in the NSF LSP repository. Use when Codex is working in this repo and needs repo-specific guardrails: read current fact docs first, gather code and test evidence before editing, keep scope to the user's explicit request, reuse shared registries and single sources of truth, stop for confirmation before changing public behavior or shared boundaries, and run only the smallest matching validation.
---

# NSF Repo Execution

## Start Here

Read the current fact docs before making assumptions:

- [`README.md`](../../../../README.md)
- [`AGENTS.md`](../../../../AGENTS.md)
- [`docs/architecture.md`](../../../architecture.md)
- [`docs/resources.md`](../../../resources.md)
- [`docs/testing.md`](../../../testing.md)

Add topic docs only when the task touches that area:

- [`docs/client-editor-features.md`](../../../client-editor-features.md) for comment toggling, auto-pairs, `wordPattern`, snippets, folding, and file-extension ownership
- [`docs/type-method-interface-contract.md`](../../../type-method-interface-contract.md) for `type_model.*`, `methods/object_methods`, array texture coordinate rules, and labeled parameter syntax

Treat `docs/human-ai/` material as collaboration context, not current fact, unless a file explicitly says it has been promoted.

## Diagnose Before Editing

- Start from the user-visible symptom or requested behavior, then inspect the owning code path, fixture, and current fact doc.
- Form one root-cause hypothesis and one smallest change that would prove or disprove it.
- Gather evidence before editing. Prefer authoritative modules, tests, and request paths over guesswork.
- If a first attempt fails, write down which assumption broke before trying another path.
- Do not keep mutating the same surface fix without new evidence.

## Keep the Change Narrow

- Solve only the problem the user explicitly asked about.
- Prefer the smallest change inside the existing architecture over broader cleanup or abstraction.
- Reuse shared registries, runtimes, and single sources of truth instead of copying rules into feature code.
- Do not add fallback, compat layer, adapter, shim, feature flag, dual path, silent downgrade, retry, or guess-based repair unless the user explicitly asks for that tradeoff.
- Do not change business logic just to satisfy one suspicious test failure before confirming the root cause.

## Ask Only For Real Decisions

- Do the discoverable work yourself. Search the repo, read the relevant implementation, and run the smallest useful validation before asking the user anything.
- Ask only when the missing information is genuinely external, or when the next step changes scope, architecture, or public behavior in a non-obvious way.
- When you must ask, present the trigger, the concrete risk, and the smallest viable options.

## Stop And Confirm At Boundaries

- Stop before changing a shared entry point, a single source of truth, or a key shared module.
- Stop before changing hover, completion, signature help, diagnostics, semantic tokens, or other user-visible behavior in a way the user did not ask for explicitly.
- Stop before changing resource bundle layout, bundle naming, bundle loading rules, or reintroducing a deprecated layout.
- Stop before adding compatibility layers or keeping old and new logic alive together.
- Stop if the only workable solution materially expands the scope beyond the current request.

## Validate Proportionally

- Resource or schema changes: `npm run json:validate`
- TypeScript client or test harness changes: `npm run compile`
- C++ server changes, or resource changes that must reach the build output: `cmake --build .\\server_cpp\\build`
- Behavior changes to completion, hover, signature help, diagnostics, semantic tokens, or other repo-mode behavior: `npm run test:client:repo`
- Perf, full gate, and packaging only when the task explicitly justifies them
- If a single integration test fails once and looks timing-related, rerun it before changing code.

## Sync Docs In The Same Task

- Update docs when commands, paths, resource rules, architecture boundaries, public behavior, testing strategy, AI collaboration rules, or header-level contracts change.
- If no docs changed, say `No doc updates needed` and explain why.
- Keep collaboration drafts and skills out of the current-fact set unless a file explicitly promotes itself.

## Close Out Clearly

- Include the root cause, the actual change, why it fits the current architecture, what validation ran, and whether docs were updated.
- If you intentionally avoided extra abstraction or fallback logic, say so as part of the minimal-change rationale.
