#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

make >/dev/null

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

root="$(pwd)"

# env_command splits its command on spaces, so copy fixtures into the
# space-free mktemp dir; the repo path itself may contain spaces.
bindir="$tmpdir/bin"
mkdir -p "$bindir"
# macOS /var -> /private/var: bondage requires canonical absolute paths.
bindir="$(cd "$bindir" && pwd -P)"
cp "$root/fixtures/fake-nono" "$bindir/fake-nono"
cp "$root/fixtures/fake-codex" "$bindir/fake-codex"
cp "$root/fixtures/fake-profile-context" "$bindir/fake-helper"
chmod +x "$bindir/fake-nono" "$bindir/fake-codex" "$bindir/fake-helper"

nono_fp="$(./bondage hash-file "$bindir/fake-nono")"
codex_fp="$(./bondage hash-file "$bindir/fake-codex")"
helper_fp="$(./bondage hash-file "$bindir/fake-helper")"

# 1. Matching env_command_fp pin lets the helper run and export its value.
pinned_conf="$tmpdir/pinned.conf"
cat >"$pinned_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "pinned"]
use_envchain = false
use_nono = false
touch_policy = none
target_kind = native
target = $bindir/fake-codex
target_fp = $codex_fp
env_command = PINNED_VALUE=$bindir/fake-helper
env_command_fp = PINNED_VALUE=$helper_fp
CONF

BONDAGE_CONF="$pinned_conf" BONDAGE_QUIET=1 \
  ./bondage exec pinned -- smoke >"$tmpdir/pinned.out" 2>"$tmpdir/pinned.err"
grep -F 'fake codex args=smoke' "$tmpdir/pinned.out" >/dev/null

# 2. Mismatched pin fails closed before exec.
mismatch_conf="$tmpdir/mismatch.conf"
cat >"$mismatch_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "mismatch"]
use_envchain = false
use_nono = false
touch_policy = none
target_kind = native
target = $bindir/fake-codex
target_fp = $codex_fp
env_command = PINNED_VALUE=$bindir/fake-helper
env_command_fp = PINNED_VALUE=sha256:0000000000000000000000000000000000000000000000000000000000000000
CONF

if BONDAGE_CONF="$mismatch_conf" BONDAGE_QUIET=1 \
  ./bondage exec mismatch -- smoke >"$tmpdir/mismatch.out" 2>"$tmpdir/mismatch.err"; then
  echo "expected exec to fail on env_command fingerprint mismatch" >&2
  exit 1
fi
grep -F 'env_command PINNED_VALUE fingerprint mismatch' "$tmpdir/mismatch.err" >/dev/null
grep -F "for $bindir/fake-helper" "$tmpdir/mismatch.err" >/dev/null
if grep -F 'fake codex args' "$tmpdir/mismatch.out" >/dev/null 2>&1; then
  echo "target must not run after env_command fingerprint mismatch" >&2
  exit 1
fi

# 3. Count mismatch between env_command and env_command_fp is a config error.
count_conf="$tmpdir/count.conf"
cat >"$count_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "countmismatch"]
use_envchain = false
use_nono = false
touch_policy = none
target_kind = native
target = $bindir/fake-codex
target_fp = $codex_fp
env_command = FIRST=$bindir/fake-helper
env_command = SECOND=$bindir/fake-helper
env_command_fp = FIRST=$helper_fp
CONF

if ./bondage verify countmismatch "$count_conf" >"$tmpdir/count.out" 2>"$tmpdir/count.err"; then
  echo "expected config load to fail on env_command_fp count mismatch" >&2
  exit 1
fi
grep -F "env_command_fp count must match env_command count" "$tmpdir/count.err" >/dev/null

# 4. env_command_fp entries must be NAME=value shaped.
shape_conf="$tmpdir/shape.conf"
cat >"$shape_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "shape"]
use_envchain = false
use_nono = false
touch_policy = none
target_kind = native
target = $bindir/fake-codex
target_fp = $codex_fp
env_command = PINNED_VALUE=$bindir/fake-helper
env_command_fp = not-an-assignment
CONF

if ./bondage verify shape "$shape_conf" >"$tmpdir/shape.out" 2>"$tmpdir/shape.err"; then
  echo "expected config load to fail on malformed env_command_fp" >&2
  exit 1
fi
grep -F "env_command_fp requires NAME=sha256:..." "$tmpdir/shape.err" >/dev/null

# 5. Unpinned env_command still runs (pinning is opt-in).
unpinned_conf="$tmpdir/unpinned.conf"
cat >"$unpinned_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "unpinned"]
use_envchain = false
use_nono = false
touch_policy = none
target_kind = native
target = $bindir/fake-codex
target_fp = $codex_fp
env_command = UNPINNED_VALUE=$bindir/fake-helper
CONF

BONDAGE_CONF="$unpinned_conf" BONDAGE_QUIET=1 \
  ./bondage exec unpinned -- smoke >"$tmpdir/unpinned.out" 2>"$tmpdir/unpinned.err"
grep -F 'fake codex args=smoke' "$tmpdir/unpinned.out" >/dev/null

# 6. env_command_fp pins survive defaults inheritance.
inherited_conf="$tmpdir/inherited.conf"
cat >"$inherited_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[defaults "pinned-env"]
env_command = INHERITED_VALUE=$bindir/fake-helper
env_command_fp = INHERITED_VALUE=sha256:0000000000000000000000000000000000000000000000000000000000000000

[profile "inherits-env"]
inherits = pinned-env
use_envchain = false
use_nono = false
touch_policy = none
target_kind = native
target = $bindir/fake-codex
target_fp = $codex_fp
CONF

if BONDAGE_CONF="$inherited_conf" BONDAGE_QUIET=1 \
  ./bondage exec inherits-env -- smoke >"$tmpdir/inherited.out" 2>"$tmpdir/inherited.err"; then
  echo "expected exec to fail on inherited env_command fingerprint mismatch" >&2
  exit 1
fi
grep -F 'env_command INHERITED_VALUE fingerprint mismatch' "$tmpdir/inherited.err" >/dev/null

echo "test-env-command-pinning: ok"
