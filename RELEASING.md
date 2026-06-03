# Releasing

The public source repository is:

- `https://github.com/nvk/bondage`

The Homebrew tap formula is:

- `agent-bondage` in `https://github.com/nvk/homebrew-tap`
- `touchid-check` in `https://github.com/nvk/homebrew-tap`

## Release Flow

1. Commit the desired `bondage` changes.
2. Create and push a version tag.
3. Update `Formula/agent-bondage.rb` and, when the helper changed,
   `Formula/touchid-check.rb` in `nvk/homebrew-tap`:
   - `tag`
   - `revision`
   - `version`
4. Commit and push the tap change on `main`.
5. Verify install from Homebrew.

## Example

From the local repo:

```zsh
git tag -a v0.2.6 -m 'bondage 0.2.6'
git push origin main --follow-tags
```

Then update the tap formula and push it:

```zsh
TAP_DIR="${TAP_DIR:-$HOME/src/homebrew-tap}"
git -C "$TAP_DIR" add Formula/agent-bondage.rb README.md RELEASING.md
git -C "$TAP_DIR" commit -m 'agent-bondage 0.2.6'
git -C "$TAP_DIR" push origin main
```

## Verify

```zsh
brew update
brew install nvk/tap/agent-bondage
brew install nvk/tap/touchid-check
```
