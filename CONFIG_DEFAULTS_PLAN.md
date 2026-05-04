# Config Defaults / Inheritance Plan

Implementation status: landed in this working tree on 2026-04-30. The plan is
kept as the design record and acceptance checklist.

## Problem

`bondage.conf` currently repeats the same launch-policy fragments across many
profiles:

- target paths and fingerprints for each agent family
- `nono` tty/device grants
- GitHub token injection
- sandbox-safe Git config override
- Node interpreter and package-root pins for script agents

That repetition is not just cosmetic. It makes upgrades brittle because one
profile can be repinned or edited while a sibling profile silently stays stale.
The goal is to reduce copy-paste while preserving the core security model:
explicit pinned tools, explicit secret release, explicit sandbox profile, and
deterministic launch argv.

## Gray-Beard Principles

- Prefer boring syntax over clever syntax.
- Make inheritance opt-in and visible at the profile.
- Preserve old configs without migration.
- Keep the parser small and auditable.
- Make the rendered effective profile inspectable.
- Let profile-local values win over shared defaults.
- Never widen sandbox or secret access implicitly.
- Fail loudly on ambiguous ownership, stale pins, missing defaults, or invalid
  security combinations.
- Keep machine-specific paths, hashes, and policy local; public docs should
  teach the pattern, not publish a pretend-universal local config.
- Write tests before migrating the local config.
- Keep repin/doctor truthful when values come from shared defaults.

## Anti-Goals

- Do not add implicit global inheritance. Every profile that consumes defaults
  must say so with `inherits = ...`.
- Do not introduce wildcards, conditionals, includes, environment expansion, or
  a second policy language in `bondage.conf`.
- Do not let defaults inherit defaults in the first version.
- Do not silently drop inherited `nono_*`, `env_*`, or secret-release settings
  when a profile disables the layer that would consume them.
- Do not make shell wrappers smarter to compensate for config duplication. The
  launcher remains the policy boundary.
- Do not hide stable target startup flags in shell wrappers when they can live
  in audited launcher config.

## Proposed Syntax

Add named defaults sections:

```ini
[defaults "agent-nono"]
nono_allow_cwd = true
nono_allow_dir = /Users/you/Library/Mobile Documents/com~apple~CloudDocs/claude-sandbox
nono_allow_file = /dev/tty
nono_allow_file = /dev/null
nono_read_file = /dev/urandom
env_set = GIT_CONFIG_GLOBAL=/Users/you/.config/bondage/gitconfig-sandbox
env_command = GH_TOKEN=/opt/homebrew/bin/gh auth token
env_command = GITHUB_TOKEN=/opt/homebrew/bin/gh auth token

[defaults "codex-target"]
target_kind = native
target = /opt/homebrew/Caskroom/codex/0.128.0/codex-aarch64-apple-darwin
target_fp = sha256:...

[profile "codex"]
inherits = agent-nono,codex-target
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
```

## Semantics

- `[defaults "name"]` accepts the same keys as `[profile "..."]`, except profile
  identity keys such as `name` are not present.
- `inherits = a,b,c` is profile-local and explicit.
- Defaults are applied in listed order.
- The profile body is applied last.
- Scalar keys use last-writer-wins.
- List keys append in order: inherited defaults first, profile-local entries
  last.
- `target_arg` is a list key. Values are inserted after the verified target
  entrypoint and before user passthrough args.
- Later `env_set` values already override earlier values during env assembly,
  so list append order keeps existing behavior.
- Missing default names are hard errors.
- Duplicate default names are hard errors.
- Cycles are impossible in the first version because defaults do not inherit
  other defaults.

## Data Model Changes

Add a `bondage_defaults` structure that reuses the same fields as
`bondage_profile`, minus the profile name and runtime-only identity.

Add presence bits for booleans:

- `use_envchain_set`
- `use_nono_set`
- `nono_allow_cwd_set`

This is needed because `false` is meaningful and must not be confused with
"unset".

Profiles should keep the current effective fields. Parsing should build raw
profiles/defaults, then resolve them into effective profiles before validation.

## Parser Plan

1. Extend section parsing to recognize `[defaults "name"]`.
2. Add `inherits` support only in profile sections.
3. Parse defaults with the same assignment function used for profiles.
4. After file read, resolve each profile:
   - start with built-in profile defaults (`use_envchain=true`, `use_nono=true`)
   - apply each named defaults block in order
   - apply profile-local keys
   - validate the resulting effective profile
5. Keep old configs working by treating profiles without `inherits` exactly as
   they work today.

## Repin And Doctor Plan

`repin` and `doctor` must understand value ownership.

For each effective field, record where it came from:

- built-in default
- named defaults section
- profile section

When `repin <profile>` updates `target`, `target_fp`, `interpreter`,
`interpreter_fp`, `package_root`, or `package_tree_fp`:

- if the stale value came from a named defaults block, rewrite that defaults
  block
- if it came from the profile section, rewrite that profile section
- if multiple fields in one command come from different owners, rewrite each
  owner explicitly and report each rewritten section
- if ownership is ambiguous, fail with an explicit diagnostic rather than
  guessing

`doctor` should report inherited stale pins clearly:

```text
profile codex: stale via defaults "codex-target"
suggest: bondage repin codex ~/.config/bondage/bondage.conf
```

`argv` remains the final runtime truth. Add a machine-readable or stable
human-readable effective-profile view only if `doctor`/`status` cannot make
ownership clear enough during implementation.

## Test Plan

Add fixture configs and tests for:

- existing config without defaults still parses, verifies, and repins
- profile inherits target and common `nono` grants
- profile-local scalar overrides inherited scalar
- list keys preserve deterministic inherited-then-local ordering
- missing default name fails cleanly
- duplicate default name fails cleanly
- `use_nono=false` plus inherited `nono_*` settings fails loudly instead of
  silently discarding or applying them
- `use_envchain=false` plus inherited envchain namespace/secret-release
  settings fails loudly if those settings would otherwise be consumed
- `repin` updates a defaults section when inherited target pins are stale
- `repin` updates a profile section when profile-local target pins are stale
- `repin` reports all rewritten owners when target and interpreter pins live in
  different sections
- `doctor` names inherited stale defaults in its output
- `argv` output before and after local migration is byte-identical for the
  representative profiles

## Migration Plan

After the implementation and tests pass, migrate the local config into
defaults blocks:

- `agent-nono`: cwd, tty, null, urandom grants
- `agent-gh-env`: `GIT_CONFIG_GLOBAL`, `GH_TOKEN`, `GITHUB_TOKEN`
- `claude-target`: Claude native target and fingerprint
- `codex-target`: Codex native target and fingerprint
- `opencode-script-target`: OpenCode target, Node interpreter, package root,
  and fingerprints
- `pi-script-target`: Pi target, Node interpreter, package root, and
  fingerprints
- optional local-model blocks for localhost profiles

Then verify:

```sh
bondage verify claude ~/.config/bondage/bondage.conf
bondage verify codex ~/.config/bondage/bondage.conf
bondage verify opencode ~/.config/bondage/bondage.conf
bondage verify pi ~/.config/bondage/bondage.conf
```

For representative profiles, compare `bondage chain` before and after migration.
The launch chain should be byte-for-byte identical except for intentional config
deduplication.

## Rollback Plan

- Keep the old config format valid forever.
- Do not require migration for users who prefer explicit repeated profiles.
- If inherited repin has a bug, users can inline the affected defaults block
  into profiles and continue with the old behavior.
- Tag the release before public docs promote the new syntax.

## Wiki Audit

Reviewed against local architecture notes on 2026-04-30:

- `cli-ai-coding`: confirms the durable stack is `shell name -> bondage ->
  [envchain-xtra] -> [nono] -> exact pinned tool`. This plan keeps wrappers
  thin and moves no security-critical behavior back into shell glue.
- `cli-ai-coding/concepts/nono`: confirms per-tool profiles are mandatory,
  wrapper-injected files need explicit grants, and live effective nono policy
  beats historical notes. This plan does not merge nono profiles across tools;
  it only deduplicates bondage-side launch fragments.
- `cli-ai-coding/concepts/envchain-xtra`: confirms secrets are released only
  through the pinned launcher chain and that `GIT_CONFIG_GLOBAL`, `GH_TOKEN`,
  and `GITHUB_TOKEN` are deliberate launch-time environment controls. This plan
  keeps those controls explicit in named defaults rather than ambient shell env.
- `dotfiles`: confirms dotfiles can be the source of truth for managed local
  config, while machine-specific state, caches, and secrets stay out of public
  docs. Migration should update the managed `bondage.conf`, not a scratch copy
  or generated profile cache.
- `ai-coding-tool-privacy`: confirms client-side ignore files are not a
  security boundary and secrets should not sit in repo-visible config. This
  plan avoids plaintext secrets and treats inherited secret-release settings as
  security-sensitive capabilities.
- `tech-gray-beard`: confirms the design bias: simple data structures,
  inspectable text config, fail-noisily behavior, strict tests, and no clever
  shell expansion tricks.

Plan adjustments from the audit:

- Add the anti-goals above to prevent defaults from becoming hidden policy.
- Treat invalid inherited security combinations as hard errors, not best-effort
  normalization.
- Require repin/doctor ownership reporting before migrating the local config.
- Keep public documentation generic and non-doxing; local absolute paths and
  fingerprints stay in the local config examples only.
