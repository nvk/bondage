```text
    ____                  __
   / __ )____  ____  ____/ /___ _____ ____
  / __  / __ \/ __ \/ __  / __ `/ __ `/ _ \
 / /_/ / /_/ / / / / /_/ / /_/ / /_/ /  __/
/_____/\____/_/ /_/\__,_/\__,_/\__, /\___/
                              /____/
```

# Bondage. Agent Bondage.

`bondage-core` is the local C launcher/policy engine.

Runtime shape:

```text
alias -> bondage -> envchain-xtra -> nono -> exact target
```

Why bother:

- you cannot let coding agents run loose with live keys
- you cannot assume a familiar command implies a trustworthy dependency tree
- you should not grant broad ambient environment access by default

`bondage` exists to narrow that trust boundary at launch time. It does not make
bad dependencies good. It makes the launch decision explicit: exact paths,
exact hashes, explicit secret release, explicit sandbox profile, then exact
target.

It also assumes the OS should do the parts the OS is good at. On macOS that
means leveraging code-signing identity where available for approvals, and
letting Keychain remain the underlying secret store instead of trying to
replace it inside the launcher.

That matters in practice for tools like Claude Code on macOS: if you use the
normal Claude.ai/OAuth login flow, your default Claude profile should keep
Keychain access or the login may fail to persist across sessions.

This repository is intentionally small and local-first. The design goal is a
FreeBSD/Capsicum-style trusted launcher:

- small C codebase
- explicit path and fd handling
- fail-closed behavior
- exact absolute execution paths
- no shell logic in the security boundary

For the trust model and deployment shape, see
[`TRUST_MODEL.md`](TRUST_MODEL.md).

Start with [`GETTING_STARTED.md`](GETTING_STARTED.md) if you want to install
and use `bondage` on a new machine.

## Operational lessons

The launcher architecture is only half the story. The operational glue matters
too. A stack like this gets brittle when shell startup, upgrade paths, or
helper scripts quietly become part of the trust boundary.

Public lessons worth keeping:

- keep home-shell startup files tiny and stable; let them source the real repo
  config when readable, instead of making the login shell depend directly on a
  fragile repo symlink
- keep shell wrappers thin; if real policy lives in shell again, the design has
  drifted
- treat package-manager upgrades as launch-policy events: repin, verify, then
  promote
- keep a named repair tier for fixing the launcher stack itself
- treat helper scripts and denial hooks as testable code, not disposable glue

In practice that means:

```text
stable home bootstrap stub
  -> readable wrapper file
  -> bondage
  -> [envchain-xtra]
  -> [nono]
  -> exact pinned tool
```

The point is not ceremony. It is to avoid a situation where one unreadable
startup file or one stale package-manager path silently drops you onto the raw
binary you were trying not to trust.

## Build

```sh
make
```

## Install

Homebrew tap install:

```sh
brew tap nvk/tap
brew install nvk/tap/agent-bondage
```

The formula name is `agent-bondage`, but it installs the `bondage` executable.

Source build for development:

```sh
git clone https://github.com/nvk/bondage.git
cd bondage
make
```

The Homebrew formula installs only the binary. Your local config, pinned tool
artifacts, and thin shell wrappers remain your responsibility.

## Current commands

```sh
./bondage status [config]
./bondage doctor [config]
./bondage verify <profile> [config]
./bondage repin <profile> [config]
./bondage repin-globals [config]
./bondage argv <profile> [config] [-- args...]
./bondage exec <profile> [config] [-- args...]
./bondage hash-file <absolute-path>
./bondage hash-tree <absolute-path>
```

If `config` is omitted, `bondage` resolves it in this order:

1. explicit CLI config argument
2. `BONDAGE_CONF`
3. `~/.config/bondage/bondage.conf`
4. `./bondage.conf`

An example config lives at [`bondage.conf.example`](bondage.conf.example).
It is intentionally a small schema/sample file, not the full local profile matrix.
The local `./bondage.conf` in this checkout is gitignored and can pin directly to
the live agent artifacts on this machine.

Minimal starter `nono` profiles live in [`examples/nono/`](examples/nono/).
Those are the sandbox-side companions to the sample launcher config.

Important:

- paths in the sample config are literal absolute paths
- `bondage` does not expand shell variables inside the config
- named `[defaults "..."]` blocks are opt-in; a profile only consumes them when
  it declares `inherits = ...`
- `bondage` itself now prefers the standard config location
  `~/.config/bondage/bondage.conf` when no explicit config is provided
- the sample config is a pattern to adapt, not a file to use unchanged
- the `examples/nono/` profiles are starter patterns, not a complete local tier matrix

## Config defaults

Use named defaults to remove repeated launch-policy fragments without moving
policy back into shell wrappers:

```ini
[defaults "agent-nono"]
nono_allow_cwd = true
nono_allow_file = /dev/tty
nono_read_file = /dev/urandom

[defaults "codex-target"]
target_kind = native
target = /Users/you/.bondage/tools/codex/0.128.0/codex-aarch64-apple-darwin
target_fp = sha256:replace-me

[defaults "codex-external-sandbox"]
target_arg = --dangerously-bypass-approvals-and-sandbox

[profile "codex"]
inherits = agent-nono,codex-target,codex-external-sandbox
use_envchain = false
use_nono = true
nono_profile = codex
touch_policy = none
```

Rules:

- inheritance is explicit and profile-local
- defaults are applied in order, then profile-local keys override them
- list keys append in order, so inherited `nono_allow_file` entries come before
  profile-local entries
- repeatable `target_arg` entries are appended after the verified target and
  before user passthrough args; this is where tool policy flags belong
- old configs without defaults still work
- invalid combinations fail closed, for example inheriting `nono_*` settings
  while setting `use_nono = false`

For agent permission modes, prefer target args in config over shell-wrapper
flags:

```ini
[defaults "claude-auto"]
target_arg = --permission-mode
target_arg = auto

[defaults "codex-external-sandbox"]
target_arg = --dangerously-bypass-approvals-and-sandbox
```

Only inherit those defaults into profiles that are still protected by the
outer sandbox layer. Do not inherit dangerous target-permission flags into a
rawdog/no-`nono` profile unless that is the explicit purpose of the profile.

`status`, `verify`, `doctor`, and `repin` report where inherited pin fields
come from. If `repin codex` refreshes `defaults "codex-target"`, every profile
that inherits that defaults block gets the new pin. That blast radius is the
point, but it should be visible in the command output.

## Upgrade discipline

Treat upgrades to `bondage`, `nono`, agent binaries, interpreters, or package
trees as explicit change events.

Minimum checklist:

```sh
bondage doctor ~/.config/bondage/bondage.conf
bondage repin-globals ~/.config/bondage/bondage.conf
bondage repin codex ~/.config/bondage/bondage.conf
bondage repin claude ~/.config/bondage/bondage.conf
bondage verify codex ~/.config/bondage/bondage.conf
bondage verify claude ~/.config/bondage/bondage.conf
bondage argv codex ~/.config/bondage/bondage.conf -- --help
```

`repin` is the command that removes the dumb manual step. It rewrites the
selected profile family in place, refreshes fingerprints, canonicalizes live
symlinked tool paths, and follows Homebrew version moves under `Cellar/` and
`Caskroom/` before re-verifying the result.

In practice:

- `bondage repin codex ...` updates every Codex-tier profile sharing the same
  pinned target, or the shared defaults block if the target is inherited
- `bondage repin opencode ...` can also refresh the pinned interpreter and
  package tree for script-based tools, including inherited script defaults
- global helpers like `nono`, `envchain`, and `touchid-check` are repinned too
  when that profile type depends on them

`doctor` is the non-mutating pass. It checks the whole config, exits nonzero on
stale or broken pins, and tells you which repair command to run next.

`repin-globals` is the narrow maintenance command for shared helpers only. Use
it when the drift is in `nono`, `envchain`, or `touchid-check`, not in a tool
family itself.

`verify` still fails closed, but it now tries to explain common Homebrew drift
in human terms. If a pinned path moved from one installed version to another,
the error tells you what changed and which `repin` command to run next.

Then open a fresh shell and confirm the wrapper names still resolve to shell
functions rather than silently falling through to raw binaries.

## Current status

Implemented now:

- hand-written INI-ish config parser
- `status`
- `doctor`
- `verify`
- `repin`
- `repin-globals`
- `argv`
- `exec`
- `hash-file`
- `hash-tree`
- exact-path checks via `realpath()`
- fd-based SHA-256 hashing for direct artifacts
- deterministic package-tree hashing for script profiles
- named defaults and explicit profile inheritance for repeated launch policy
- optional `envchain` per profile
- optional `nono` per profile, including rawdog/no-`nono` launches
- profile-driven `nono` flags like `--allow-cwd`, `--allow-file`, and `--read-file`
- profile-driven target args for stable tool policy flags
- global `nono_profile_root` injection so short profile names expand to explicit JSON paths
- profile-driven static env injection and command-derived env vars
- Touch ID launch policy via pinned `touchid-check`
- prelaunch directory creation for profiles that need state dirs before sandbox start
- exact argv construction for:
  - `envchain -> nono wrap --profile <name> -- target`
  - `envchain -> target`
  - `nono wrap --profile <name> -- target`
  - direct target exec
- end-to-end fake execution for:
  - native target
  - script target + pinned interpreter + package tree
- explicit `nono_profile_root` support so short profile names expand to real JSON profile paths
- real thin-wrapper cutover from the shell functions for the local agent stack

Notes:

- local pinned configs should use the real installed `nono` profile names from this setup,
  like `custom-claude`
- the local config now carries the real tier matrix:
  `base`, `-mid`, `-plugin`, `-unsafe`, `-dotfiles`, and `-rawdog`
- the intended shell shape is now thin convenience only:
  names, tab colors, and small prompt-shaping helpers

## Release Notes

For release/tag and tap update steps, see
[`RELEASING.md`](RELEASING.md).
