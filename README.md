# bondage-core

Local C launcher/policy engine for:

```text
alias -> bondage -> envchain-xtra -> nono -> exact target
```

This repository is intentionally small and local-first. The design goal is a
FreeBSD/Capsicum-style trusted launcher:

- small C codebase
- explicit path and fd handling
- fail-closed behavior
- exact absolute execution paths
- no shell logic in the security boundary

## Build

```sh
make
```

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
The local `./bondage.conf` in this checkout is gitignored and can pin directly to
the envchain-xtra `touch-id` PR worktree plus the live agent artifacts on this
machine.

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

Not implemented yet:

- thin-wrapper cutover from the existing shell functions
- explicit `nono` profile directory configuration for environments where `nono`
  cannot discover `~/.config/nono/profiles` normally

Notes:

- local pinned configs should use the real installed `nono` profile names from this setup,
  like `custom-claude`
- the local config now carries the real tier matrix:
  `base`, `-mid`, `-plugin`, `-unsafe`, `-dotfiles`, and `-rawdog`
