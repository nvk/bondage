# Bondage Core Spec

Canonical local design summary:

- C, not Rust
- simple config grammar
- exact absolute paths
- open first, verify, then execute
- fail closed
- shell aliases are convenience only

Target execution chain:

```text
alias -> bondage -> envchain-xtra -> nono -> exact target
```

Planned phases:

1. MVP launcher
2. JS script + interpreter verification
3. package tree hashing
4. wrapper-parity launch config
5. bundle promotion pipeline

Key rules:

- no PATH lookups after verification starts
- no `/bin/sh -c`
- no security-critical shell glue
- no mutable global npm installs in the trusted path
- exact argv must support:
  - `envchain <namespace> <nono> wrap --profile <name> -- <target...>`
  - `envchain <namespace> <target...>`
  - `<nono> wrap --profile <name> -- <target...>`
  - direct `<target...>`
- repeatable `target_arg` values must be inserted after the verified target
  entrypoint and before user passthrough args
- config defaults must be explicit:
  - `[defaults "name"]` defines reusable profile keys
  - `inherits = name` opt-in applies defaults to a profile
  - profile-local scalar keys override inherited values
  - list keys append inherited values first, profile-local values second
  - no implicit global inheritance, includes, wildcards, or shell expansion
- `repin` and `doctor` must report the owning section for inherited pins

Implemented now:

- direct file hashing
- package tree hashing
- config rewrite + `repin` for versioned tool moves
- named config defaults with explicit profile inheritance
- ownership-aware `repin`/`doctor` for inherited target, interpreter, and package pins
- non-mutating `doctor` drift detection
- `repin-globals` for shared helper maintenance
- exact argv construction
- optional `envchain`
- optional `nono`
- global nono profile-root expansion for explicit JSON profile paths
- `nono` flag injection
- target argument injection for stable tool startup policy
- static env injection and command-derived env vars
- Touch ID / local auth launch policy
- prelaunch directory creation
- `execve()` launch for native and script targets

Still intentionally deferred:

- thin-wrapper cutover
- bundle promotion pipeline
