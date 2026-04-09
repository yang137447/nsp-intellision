---
name: plan-execution-orchestrator
description: Use when the user explicitly asks to execute an approved plan or spec Markdown file and the task benefits from value-gated multi-subagent orchestration for implementation, architecture review, QA verification, progress handoffs, and hard stops on boundary changes or low-value retry loops.
---

# Plan Execution Orchestrator

## Overview

Execute approved plan Markdown files with a controller-led loop:

1. `Entry Gate`
2. `Plan Intake`
3. `Execution Round`
4. `Parallel Review Round`
5. `Work-Value Gate`
6. `Escalate Or Continue`
7. `Progress/Handoff Sink`
8. `Completion Gate`

Core principle:

`Automatic loops exist to improve convergence, not to maximize agent activity.`

## Plan Source

This skill realizes the approved plan captured in `docs/human-ai/plan-execution-orchestrator-skill-design.md`. Always treat that design doc as the orchestrator's playbook, while still deferring to the current fact docs (`README.md`, `AGENTS.md`, `docs/architecture.md`, `docs/resources.md`, `docs/testing.md`) whenever they contradict the plan text.

## Preconditions

Only use this skill when all of the following are true:

- The user explicitly asks to execute a plan, approved spec, or equivalent Markdown plan file
- A concrete plan/spec Markdown path exists
- The work is no longer in brainstorming mode
- There is at least one executable task that can begin without immediately violating repo boundaries

Do not use this skill when:

- The user is still shaping requirements or asking for design help
- No plan/spec Markdown file exists
- The "plan" is actually a design note, brainstorm, or vague idea dump
- The only way forward immediately changes module boundaries, shared truth sources, public behavior, or compatibility strategy without approval

## Repo Guardrails First

Before trusting the plan, read the repo guardrails and current fact docs:

- [`README.md`](../../../../README.md)
- [`AGENTS.md`](../../../../AGENTS.md)
- [`docs/architecture.md`](../../../architecture.md)
- [`docs/resources.md`](../../../resources.md)
- [`docs/testing.md`](../../../testing.md)
- [`../nsf-repo-execution/SKILL.md`](../nsf-repo-execution/SKILL.md)

Load topic docs only when the plan touches that area.

Treat `docs/human-ai/` material as collaboration context unless the file explicitly says it has been promoted to current fact.

## Entry Gate

Before dispatching any subagent:

1. Confirm the user explicitly asked to execute a plan
2. Confirm the plan/spec Markdown path exists
3. Confirm the file is actually a plan/spec, not a brainstorm note
4. Check for obvious `AGENTS.md` hard-stop conditions
5. Decide whether the plan contains at least one safe executable slice

If any of these fail, stop and tell the user what blocked execution.

## Plan Intake

Read the plan and extract:

- Task list
- Files each task expects to touch
- Validation commands
- Required doc sync points
- Explicit scope boundaries

Then tag each task as:

- `parallel-safe`
- `serial-only`
- `high-risk-review`
- `doc-sensitive`

Never dispatch parallel implementers with overlapping write sets.

## Execution Round

The controller decides whether to run one or more implementers.

- Use parallel implementers only for disjoint write sets
- Keep tightly coupled work serial
- Give each implementer clear ownership, file scope, validation scope, and explicit instructions not to revert other edits

When dispatching implementers, load [`references/agent-contracts.md`](./references/agent-contracts.md) and use the implementer contract.

## Parallel Review Round

After implementation, dispatch two read-only reviewers in parallel:

- `architecture-reviewer`
- `qa-verifier`

The reviewers do not fix code. They gather evidence and return one of the approved statuses from [`references/agent-contracts.md`](./references/agent-contracts.md).

## Work-Value Gate

Never continue just because an issue exists. Continue only after scoring the value of another loop.

### Hard Stops

Immediately stop automatic looping and return to the user if any of these are true:

- `AGENTS.md` says the next change requires confirmation
- The next step changes module boundaries, single sources of truth, or key shared entry points
- The next step changes public behavior
- The only path forward needs fallback, compat, adapter, dual-path, or silent downgrade logic
- Implementer, architecture reviewer, and QA verifier disagree in a way you cannot resolve from current evidence
- Two rounds in a row fail to produce a new testable hypothesis

### Score The Next Round

If no hard stop triggered, score three dimensions from `0` to `2`:

- `New Evidence`
- `Risk Reduction`
- `Benefit / Cost`

Decision rule:

- `5-6`: continue automatically
- `3-4`: allow only one more code-level repair round
- `0-2`: stop and write progress/handoff instead of looping

### Retry Guardrails

- Maximum 2 automatic repair rounds for the same code problem
- Maximum 2 attempts at the same failure pattern
- If the second attempt still lacks new evidence, stop

## Escalate Or Continue

Continue automatically only for code-level, evidence-backed fixes.

Stop and ask the user when:

- a reviewer returns `NEEDS_USER_CONFIRMATION`
- the next step expands scope beyond the plan
- the next step changes public behavior or a shared boundary
- the work-value score says continuing is no longer worth it

## Progress/Handoff Sink

Write progress/handoff whenever:

- a round ends and you need to decide whether to continue
- a hard stop triggers
- the work-value gate says stop
- context is getting tight
- the user interrupts or a new thread will need to resume

Prefer appending an `Execution Progress` section to the active plan/spec Markdown.

If that is awkward, create a task-specific handoff note under `docs/human-ai/`.

Use [`references/progress-handoff-template.md`](./references/progress-handoff-template.md) for the exact structure.

## Completion Gate

Do not claim completion until:

- fresh verification evidence exists
- doc-update obligations have been checked
- no unresolved hard-stop issues remain

If no doc changes were needed, say `No doc updates needed` and explain why.

## Red Flags

Stop and reconsider if you catch yourself thinking:

- "The user said keep going, so I can ignore stop rules."
- "QA found problems, so I should just retry until green."
- "This is only a small boundary change."
- "I can add a fallback for now and clean it up later."
- "I do not need to write progress because I still have context."

These are signs the orchestrator is drifting out of controlled execution.

## Integration Notes

- Use repo fact docs and `AGENTS.md` as higher priority than any plan wording
- Prefer existing skills and repo-specific guardrails over inventing parallel process rules
- Keep the orchestrator focused on control flow and value gating, not on replacing implementation or review expertise
