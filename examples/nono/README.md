# Example nono Profiles

These starter profiles are the public `nono` side of the same stack shown in
`bondage.conf.example`.

They are intentionally minimal:

- deny mounted volumes with `/Volumes`
- allow project workdir read/write access
- add only the extra state paths a given client actually needs

Treat them as patterns to adapt, not files to copy unchanged. In particular:

- replace profile names if your local setup uses different names
- add only the extra paths your own workflow needs
- keep auth and storage caveats in mind, especially for macOS Keychain-backed tools

The launcher side belongs in [`../../bondage.conf.example`](../../bondage.conf.example).
