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
./bondage verify <profile> [config]
./bondage argv <profile> [config] [-- args...]
./bondage exec <profile> [config] [-- args...]
./bondage hash-file <absolute-path>
./bondage hash-tree <absolute-path>
```

If `config` is omitted, `./bondage.conf` is used.

An example config lives at [`bondage.conf.example`](bondage.conf.example).
It is intentionally a small schema/sample file, not the full local profile matrix.
The local `./bondage.conf` in this checkout is gitignored and can pin directly to
the live agent artifacts on this machine.

Important:

- paths in the sample config are literal absolute paths
- `bondage` does not expand shell variables inside the config
- the sample config is a pattern to adapt, not a file to use unchanged

## Current status

Implemented now:

- hand-written INI-ish config parser
- `status`
- `verify`
- `argv`
- `exec`
- `hash-file`
- `hash-tree`
- exact-path checks via `realpath()`
- fd-based SHA-256 hashing for direct artifacts
- deterministic package-tree hashing for script profiles
- optional `envchain` per profile
- optional `nono` per profile, including rawdog/no-`nono` launches
- profile-driven `nono` flags like `--allow-cwd`, `--allow-file`, and `--read-file`
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
