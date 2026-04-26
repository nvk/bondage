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

Implemented now:

- direct file hashing
- package tree hashing
- exact argv construction
- optional `envchain`
- optional `nono`
- global nono profile-root expansion for explicit JSON profile paths
- `nono` flag injection
- static env injection and command-derived env vars
- Touch ID / local auth launch policy
- prelaunch directory creation
- `execve()` launch for native and script targets

Still intentionally deferred:

- thin-wrapper cutover
- bundle promotion pipeline
