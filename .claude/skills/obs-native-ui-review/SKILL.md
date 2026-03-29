---
name: obs-native-ui-review
description: Review and optionally improve OBS Studio native plugin (C/C++) properties/settings UI code when the task involves get_properties, obs_properties, obs_property callbacks, obs_data settings, source/filter defaults, update paths, or UI state bugs such as redraw loops, lost values, hidden stale settings, and callback reentrancy. Do not use for browser-source HTML/JS or Qt frontend-only UIs.
allowed-tools: Read, Edit, MultiEdit, Write, Grep, Glob, LS, Bash
---

# OBS native plugin UI review and remediation

Use this skill for **OBS Studio native plugin UI code only**:
- C/C++ plugins using `obs_properties_t`, `obs_property_t`, `obs_data_t`
- source/filter settings, defaults, modified callbacks, update flows
- bugs involving redraw, visibility/enabled toggles, value persistence, dynamic lists, hidden fields, or state desynchronization

Do **not** use this skill for:
- browser-source HTML/JS UIs
- Qt standalone frontend dialogs/docks unless they are only thin wrappers around native OBS properties APIs
- generic C++ style review unrelated to OBS UI/state management

## Goal

Evaluate whether the implementation follows the repository's OBS UI design rules, produce a concise report with severity and concrete fixes, and if the user asks for changes, implement them in a controlled way.

## Canonical rules to evaluate

Treat the following as the default rule set unless the repository defines a stricter local convention:

1. **Single source of truth**: persistent user-facing settings must live in `obs_data_t`.
2. **Pure-ish properties builder**: `get_properties()` should define UI structure, not own persistent state.
3. **No long-lived property pointers**: do not cache `obs_property_t*` or `obs_properties_t*` across builds.
4. **Minimal modified callbacks**: property callbacks should primarily control visibility/enabled state and lightweight dependent UI updates.
5. **Return redraw only when needed**: return `true` from modified callbacks only when layout/UI structure must refresh.
6. **No heavy work in UI callbacks**: avoid device scans, network access, expensive file IO, or expensive graph updates in modified callbacks.
7. **Defaults are explicit**: every persistent setting should have a declared default when practical.
8. **UI-only state separated**: non-behavioral UI state should use private settings or another non-persistent/per-UI mechanism, not core runtime config.
9. **Do not persist derived state**: save user intent, not recomputable presentation data.
10. **Stable identifiers for dynamic lists**: save durable IDs, not display labels.
11. **Update path owns runtime state**: apply runtime behavior in `update()` / creation / reset paths, not ad hoc from UI callbacks.
12. **Hidden-field semantics are intentional**: if a field becomes hidden or irrelevant, either preserve it deliberately with rationale or clear/migrate it deliberately.
13. **Null-safe assumptions**: do not assume `data` passed to `get_properties()` is always non-null.
14. **Version-safe migrations**: when settings schema changes, preserve backward compatibility or add explicit migration.

## What to inspect

Search for patterns including but not limited to:
- `get_properties`, `properties`, `obs_properties_create`
- `obs_properties_add_*`
- `obs_property_set_modified_callback`, `obs_property_set_modified_callback2`
- `obs_property_set_visible`, `obs_property_set_enabled`
- `obs_data_get_*`, `obs_data_set_*`, `obs_data_set_default_*`
- `get_defaults`, `update`, `create`, `destroy`
- `obs_source_get_private_settings`
- dynamic list builders and refresh helpers
- any static/global cache touching property objects or UI state

## Review procedure

1. Identify the plugin objects and files involved in settings UI construction.
2. Build a settings lifecycle map:
   - where defaults are defined
   - where UI is built
   - where callbacks mutate behavior or settings
   - where runtime state is updated from settings
3. Compare implementation against every canonical rule above.
4. Record findings with:
   - severity: critical / high / medium / low / note
   - rule violated or partially satisfied
   - evidence: file and function names, with brief quoted snippets only when necessary
   - impact: what bug can occur
   - fix: concrete recommended change
5. Distinguish clearly between:
   - confirmed issues
   - acceptable deviations with rationale
   - uncertain cases that need runtime validation

## Output format for review-only tasks

Use this structure unless the user asked for another format:

### Summary
- overall judgment in 3-6 bullets

### Findings
For each finding:
- ID: `OBSUI-<n>`
- Severity
- Rule
- Location
- Problem
- Why it matters
- Recommended fix

### Scorecard
- Rule-by-rule pass / partial / fail / not-applicable

### Next actions
- smallest safe remediation order
- tests or manual checks to run

## If the user asks for code changes

Before editing:
1. Restate the intended remediation plan briefly.
2. Prefer **minimal, behavior-preserving** changes.
3. Do not refactor unrelated code.
4. Preserve public behavior unless the issue itself requires a behavior fix.

When editing, prefer these transformations:
- move persistent values into `obs_data_t`
- move runtime application into `update()` or clearly owned apply helpers
- replace cached property pointers with on-demand lookup inside current properties object
- narrow modified callbacks to visibility/enabled/list refresh duties
- add explicit defaults
- switch saved list values from labels to stable IDs
- add comments only where the ownership boundary is otherwise unclear

After editing, report:
- files changed
- what rules are now satisfied
- any unresolved risk
- recommended verification steps

## Guardrails

- Do not claim compliance without checking the actual code.
- Do not invent OBS APIs.
- If repository conventions conflict with the canonical rules, prefer local conventions only when they are explicit and technically sound; mention the deviation.
- If there is insufficient context, say so and perform the narrowest defensible review.
- For broad repositories, focus first on files directly tied to the user’s requested plugin or changed diff.

## Suggested commands the agent may run

Use repository-appropriate equivalents:
- search for UI construction and callbacks
- inspect defaults/update/create/destroy functions
- inspect recent diffs when the user asks for regression review
- run build/tests only if already configured and safe

## Deliverable quality bar

A good result is not just “this looks fine.” It must explain whether the code follows the rule set, where it does not, and exactly how to fix it with minimal risk.
