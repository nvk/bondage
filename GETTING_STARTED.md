# Getting Started

This guide is the public, portable setup path for `bondage`.

The intended trust chain is:

```text
shell name -> bondage -> [envchain] -> [nono] -> exact pinned tool
```

`bondage` is just the launcher. It does not install your tools, your
`nono` profiles, or your local `envchain` secrets for you.

## Why You Need It

The point is not aesthetic purity. The point is that coding agents are useful
but too dangerous to run loose with:

- live API keys and other secrets
- mutable dependency trees you did not meaningfully audit
- broad ambient environment access from your normal shell session

`bondage` does not solve acquisition-time trust. It does not make a bad npm
tree good. What it does do is force the launch boundary to become explicit:
exact artifact set, optional secret release, optional sandbox, exact argv.

This setup also leans on the OS where it makes sense. On macOS, that means
using Keychain as the real secret store and using OS-level signing identity
where available as part of approval and drift detection.

## What You Need

- `bondage`
- `nono`
- optionally `envchain-xtra` if a profile needs secret injection
- optionally `touchid-check` if a profile uses `touch_policy = prompt`
- your actual agent/tool artifacts in immutable versioned directories

## Install

Install the launcher:

```sh
brew tap nvk/tap
brew install nvk/tap/agent-bondage
```

That installs the `bondage` executable.

Install `nono` and any other local dependencies using your own preferred path.

## Recommended Layout

Keep three things separate:

1. tool artifacts
2. launcher config
3. shell convenience wrappers

One workable layout is:

```text
~/.config/bondage/bondage.conf
~/.bondage/tools/
~/.config/nono/profiles/
~/bin/
```

For shell startup itself, prefer tiny stable files in `$HOME` that source the
real repo-backed shell config when readable. Do not make your login shell depend
directly on a fragile repo symlink. A simple pattern is:

```sh
# ~/.zshrc
if [ -r "$HOME/src-repo/.dotfiles/zsh/.zshrc" ]; then
  . "$HOME/src-repo/.dotfiles/zsh/.zshrc"
elif [ -r "$HOME/src-repo/.dotfiles/ai/shell.zsh" ]; then
  . "$HOME/src-repo/.dotfiles/ai/shell.zsh"
fi
```

The launcher stack should survive a temporarily unreadable repo path without
dropping you onto raw binaries by accident.

Example artifact layout:

```text
~/.bondage/tools/codex/0.125.0/codex-aarch64-apple-darwin
~/.bondage/tools/pi/1.2.3/dist/cli.js
~/.bondage/tools/runtimes/node/22.12.0/bin/node
```

The point is to pin exact absolute paths, not PATH lookups or mutable global
installs.

## Config

Start from [`bondage.conf.example`](bondage.conf.example), then make your own
local config file.

Recommended location:

```sh
mkdir -p ~/.config/bondage
```

Then create `~/.config/bondage/bondage.conf` from the example in this
repository and edit it so it matches your machine.

For the sandbox side, start from the matching JSON examples in
[`examples/nono/`](examples/nono/), then copy and rename only the profiles you
actually use into `~/.config/nono/profiles/`.

Important:

- use absolute paths
- fill in real fingerprints
- point `nono_profile_root` at your local profile directory
- point `tool_root` at your own immutable tool tree
- remove any profile you do not actually use
- replace the sample `nono` profile names if your local setup uses different names

The sample config is not meant to be copied unchanged.

## Machine-Specific vs Reusable

Reusable ideas:

- the config shape
- profile concepts like `base`, `-mid`, `-unsafe`, `-rawdog`
- pinning interpreter + entrypoint + package tree for JS tools
- using `envchain` only for profiles that actually need secret release
- using `nono` only as the sandbox layer

Machine-specific values:

- absolute paths
- fingerprints
- envchain namespaces
- Touch ID policy
- `nono` profile names
- local environment variables like `GH_TOKEN`

Treat the config as a local policy file, not a portable dotfile blob.

## Generate Fingerprints

Use `bondage` itself to generate the hashes you need:

```sh
bondage hash-file /absolute/path/to/tool
bondage hash-tree /absolute/path/to/package-root
```

Typical cases:

- native binary: `hash-file`
- script entrypoint: `hash-file`
- interpreter: `hash-file`
- JS package directory: `hash-tree`

## Verify Before Exec

Before wiring up shell wrappers, verify each profile explicitly:

```sh
bondage verify codex ~/.config/bondage/bondage.conf
bondage verify pi ~/.config/bondage/bondage.conf
```

Then inspect the exact argv:

```sh
bondage argv codex ~/.config/bondage/bondage.conf -- --help
bondage argv pi ~/.config/bondage/bondage.conf -- --help
```

Only after that should you use:

```sh
bondage exec codex ~/.config/bondage/bondage.conf -- --help
```

## Thin Shell Wrappers

Keep shell logic thin.

Good:

```sh
codex() { bondage exec codex ~/.config/bondage/bondage.conf -- "$@"; }
```

Bad:

- shell decides which binary is trusted
- shell decides whether Touch ID is required
- shell performs PATH-sensitive target resolution inside the trust boundary

If the shell starts growing real security logic again, the design is drifting.

If you keep multiple tiers, use explicit names that describe policy rather than
vibes. A practical split is:

- normal tier
- broader but still sandboxed tier like `*-unsafe`
- explicit no-sandbox tier like `*-rawdog`
- optional repair tier like `*-fix` for launcher and config maintenance

The repair tier is worth naming if you maintain a more complex local stack.

## Optional Pieces

### envchain

Use `envchain` only for profiles that need secret injection.

If a profile does not need secret release, prefer:

- `use_envchain = false`

### macOS Claude Code auth

If you use Claude Code on macOS with the normal Claude.ai web/OAuth login,
do not make your default Claude profile a no-Keychain profile.

Claude Code persists OAuth/API credentials in the macOS Keychain. If you
remove Keychain access from the normal Claude profile, you can end up
re-authenticating every new session.

Practical rule:

- normal Claude.ai/OAuth profile on macOS -> allow Keychain
- no-Keychain Claude profile -> reserve for API-key-only or experimental use

### nono

Use `nono` for the sandboxed tiers.

If you need an explicit escape hatch, make it obvious:

- `use_nono = false`
- separate profile name like `*-rawdog`

Do not confuse that with a looser-but-still-sandboxed profile.

### Touch ID

Use Touch ID only where you actually want an interactive local approval gate.

If a profile does not need it, keep:

- `touch_policy = none`

## First Real Smoke Test

After the config is in place, a good first test is:

```sh
bondage status ~/.config/bondage/bondage.conf
bondage verify codex ~/.config/bondage/bondage.conf
bondage exec codex ~/.config/bondage/bondage.conf -- --help
```

If those work, then add your shell wrappers.

## After upgrades

Treat package-manager upgrades as launcher changes, not just tool updates.

Minimum post-upgrade checks:

```sh
bondage verify codex ~/.config/bondage/bondage.conf
bondage verify claude ~/.config/bondage/bondage.conf
bondage argv codex ~/.config/bondage/bondage.conf -- --help
```

Then open a fresh login shell and confirm your wrapper names still resolve to
the expected shell functions.

If you use helper scripts for denial diagnostics or restart suggestions, test
those too. Paths with spaces, quotes, and punctuation are normal on macOS and
should be handled deliberately, not with naive whitespace splitting.
