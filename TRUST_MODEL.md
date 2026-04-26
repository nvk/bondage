# Bondage. Agent Bondage.

## Trust Model

`bondage-core` is the trusted launcher for the local AI agent stack.

Its intended runtime shape is:

```text
shell name -> bondage -> [envchain] -> [nono] -> exact pinned tool
```

## Design Goals

- small C codebase
- explicit path handling
- explicit argv construction
- fail-closed verification
- no `PATH` lookups in the trusted chain
- no shell logic in the security boundary

This is deliberately closer to a FreeBSD/Capsicum-style launcher mentality than to a
general shell wrapper.

## Layer Responsibilities

### shell

The shell should do convenience only:

- short command names
- tab colors
- prompt shaping
- wrapper aliases like `codex-wiki`

It should not decide:

- which binary is trusted
- whether Touch ID is required
- whether secrets are released
- which sandbox profile is used

### bondage

`bondage` owns launch-policy decisions:

- exact target selection
- exact `nono` profile selection
- optional `envchain` namespace selection
- optional Touch ID gate
- exact environment construction
- exact argv construction
- direct artifact verification
- interpreter verification for script targets
- package-tree verification for JS tools

### envchain

`envchain` owns secret release:

- fetch secrets from Keychain-backed namespaces
- export them only to the launched child process
- approve the direct binary it executes

### nono

`nono` owns sandbox enforcement:

- filesystem policy
- process policy
- network policy
- profile-based capability layout

## Threat Model

The core threats this launcher is meant to reduce are:

- accidental drift from `PATH`-based resolution
- silent binary replacement after tool upgrades
- mutable JS toolchains where only a top-level shim is checked
- shell wrappers becoming the real security boundary
- secrets being released to an unexpected launcher chain

It is not a full supply-chain solution by itself.

If you bless a compromised tool tree, `bondage` will faithfully protect the
compromised tree. That is why JS tools should be treated as pinned immutable
artifact sets, not just “whatever npm installed globally today.”

## Verification Model

### Native targets

For native binaries, `bondage` verifies:

- exact absolute target path
- exact target fingerprint

### Script / JS targets

For script-backed tools, `bondage` verifies:

- exact entrypoint path
- entrypoint fingerprint
- exact interpreter path
- interpreter fingerprint
- deterministic package-tree fingerprint

That means the trust object is the launch artifact set, not just a shell command name.

## Rawdog

`rawdog` is an explicit policy choice:

- `use_nono = false`

That bypasses the sandbox, but still keeps the verified launcher path intact when
configured that way. It is the deliberate escape hatch. It should never be confused
with `unsafe`, which is still sandboxed.

## Current Local Shape

The current local configuration uses:

- `custom-*` `nono` profiles from the dotfiles repo
- a patched `envchain` binary
- exact pins for:
  - Claude
  - Codex
  - OpenCode
  - Pi
- Touch ID on selected envchain-backed profiles

The local shell has already been reduced to convenience-only wrappers. The launcher
and trust decisions now live in `bondage.conf`, not in `shell.zsh`.

## Operational Rule

If a new feature requires adding security-critical logic to shell, that is probably the
wrong layer.

The preferred path is:

1. add the capability to `bondage`
2. express policy in `bondage.conf`
3. keep shell wrappers thin
