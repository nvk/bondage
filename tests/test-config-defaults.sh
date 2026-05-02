#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

make >/dev/null

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

root="$(pwd)"
nono_fp="$(./bondage hash-file "$root/fixtures/fake-nono")"
codex_fp="$(./bondage hash-file "$root/fixtures/fake-codex")"

old_conf="$tmpdir/old.conf"
cat >"$old_conf" <<EOF
[global]
nono = $root/fixtures/fake-nono
nono_fp = $nono_fp

[profile "codex"]
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
target_kind = native
target = $root/fixtures/fake-codex
target_fp = $codex_fp
nono_allow_cwd = true
EOF

./bondage verify codex "$old_conf" >/dev/null

conf="$tmpdir/defaults.conf"
cat >"$conf" <<EOF
[global]
nono = $root/fixtures/fake-nono
nono_fp = $nono_fp

[defaults "agent-nono"]
nono_allow_cwd = true
nono_allow_dir = $root
nono_read_dir = /tmp
nono_allow_file = /dev/tty
nono_read_file = /dev/urandom

[defaults "codex-target"]
target_kind = native
target = $root/fixtures/fake-codex
target_fp = $codex_fp

[defaults "codex-auto"]
target_arg = --sandbox
target_arg = danger-full-access

[profile "codex"]
inherits = agent-nono,codex-target,codex-auto
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
nono_allow_file = /dev/null
target_arg = --test-profile-local
EOF

verify_out="$(./bondage verify codex "$conf")"
grep -F "nono_allow_dir: $root" <<<"$verify_out" >/dev/null
grep -F "nono_read_dir: /tmp" <<<"$verify_out" >/dev/null
grep -F "nono_allow_file: /dev/tty" <<<"$verify_out" >/dev/null
grep -F "nono_allow_file: /dev/null" <<<"$verify_out" >/dev/null
grep -F "nono_read_file: /dev/urandom" <<<"$verify_out" >/dev/null

status_out="$(./bondage status "$conf")"
grep -F "  nono_allow_dir: $root" <<<"$status_out" >/dev/null
grep -F "  nono_read_dir: /tmp" <<<"$status_out" >/dev/null
grep -F "  nono_allow_file: /dev/tty" <<<"$status_out" >/dev/null
grep -F "  nono_allow_file: /dev/null" <<<"$status_out" >/dev/null
grep -F "  nono_read_file: /dev/urandom" <<<"$status_out" >/dev/null

argv="$(./bondage argv codex "$conf" -- ping)"

grep -F 'argv[4] = --workdir' <<<"$argv" >/dev/null
grep -F "argv[5] = $root" <<<"$argv" >/dev/null
grep -F 'argv[6] = --allow-cwd' <<<"$argv" >/dev/null
grep -F 'argv[7] = --allow' <<<"$argv" >/dev/null
grep -F "argv[8] = $root" <<<"$argv" >/dev/null
grep -F 'argv[9] = --read' <<<"$argv" >/dev/null
grep -F 'argv[10] = /tmp' <<<"$argv" >/dev/null
grep -F 'argv[11] = --allow-file' <<<"$argv" >/dev/null
grep -F 'argv[12] = /dev/tty' <<<"$argv" >/dev/null
grep -F 'argv[13] = --allow-file' <<<"$argv" >/dev/null
grep -F 'argv[14] = /dev/null' <<<"$argv" >/dev/null
grep -F 'argv[15] = --read-file' <<<"$argv" >/dev/null
grep -F 'argv[16] = /dev/urandom' <<<"$argv" >/dev/null
grep -F "argv[18] = $root/fixtures/fake-codex" <<<"$argv" >/dev/null
grep -F 'argv[19] = --sandbox' <<<"$argv" >/dev/null
grep -F 'argv[20] = danger-full-access' <<<"$argv" >/dev/null
grep -F 'argv[21] = --test-profile-local' <<<"$argv" >/dev/null
grep -F 'argv[22] = ping' <<<"$argv" >/dev/null

unknown="$tmpdir/unknown.conf"
cat >"$unknown" <<EOF
[global]
nono = $root/fixtures/fake-nono
nono_fp = $nono_fp

[profile "codex"]
inherits = missing-defaults
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
target_kind = native
target = $root/fixtures/fake-codex
target_fp = $codex_fp
EOF

if ./bondage status "$unknown" >"$tmpdir/unknown.out" 2>&1; then
  echo "expected unknown inherited defaults to fail" >&2
  exit 1
fi
grep -F "inherits unknown defaults 'missing-defaults'" "$tmpdir/unknown.out" >/dev/null

duplicate="$tmpdir/duplicate.conf"
cat >"$duplicate" <<EOF
[global]
nono = $root/fixtures/fake-nono
nono_fp = $nono_fp

[defaults "same"]
target_kind = native

[defaults "same"]
target = $root/fixtures/fake-codex
EOF

if ./bondage status "$duplicate" >"$tmpdir/duplicate.out" 2>&1; then
  echo "expected duplicate defaults to fail" >&2
  exit 1
fi
grep -F "duplicate defaults 'same'" "$tmpdir/duplicate.out" >/dev/null

duplicate_inherit="$tmpdir/duplicate-inherit.conf"
cat >"$duplicate_inherit" <<EOF
[global]
nono = $root/fixtures/fake-nono
nono_fp = $nono_fp

[defaults "same"]
target_kind = native

[profile "codex"]
inherits = same,same
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
target = $root/fixtures/fake-codex
target_fp = $codex_fp
EOF

if ./bondage status "$duplicate_inherit" >"$tmpdir/duplicate-inherit.out" 2>&1; then
  echo "expected duplicate inherited defaults to fail" >&2
  exit 1
fi
grep -F "duplicate inherited defaults 'same'" "$tmpdir/duplicate-inherit.out" >/dev/null

invalid_layer="$tmpdir/invalid-layer.conf"
cat >"$invalid_layer" <<EOF
[global]
nono = $root/fixtures/fake-nono
nono_fp = $nono_fp

[defaults "agent-nono"]
nono_profile = custom-codex

[profile "codex"]
inherits = agent-nono
use_envchain = false
use_nono = false
touch_policy = none
target_kind = native
target = $root/fixtures/fake-codex
target_fp = $codex_fp
EOF

if ./bondage status "$invalid_layer" >"$tmpdir/invalid-layer.out" 2>&1; then
  echo "expected inherited nono settings with use_nono=false to fail" >&2
  exit 1
fi
grep -F "has nono settings but use_nono=false" "$tmpdir/invalid-layer.out" >/dev/null

invalid_dir="$tmpdir/invalid-dir.conf"
cat >"$invalid_dir" <<EOF
[global]
nono = $root/fixtures/fake-nono
nono_fp = $nono_fp

[profile "codex"]
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
target_kind = native
target = $root/fixtures/fake-codex
target_fp = $codex_fp
nono_allow_dir = relative/path
EOF

if ./bondage status "$invalid_dir" >"$tmpdir/invalid-dir.out" 2>&1; then
  echo "expected relative nono_allow_dir to fail" >&2
  exit 1
fi
grep -F "nono_allow_dir must be an absolute path" "$tmpdir/invalid-dir.out" >/dev/null

repin_conf="$tmpdir/repin-defaults.conf"
cat >"$repin_conf" <<EOF
[global]
nono = $root/fixtures/fake-nono
nono_fp = $nono_fp

[defaults "codex-target"]
target_kind = native
target = $root/fixtures/fake-codex
target_fp = sha256:0000000000000000000000000000000000000000000000000000000000000000

[profile "codex"]
inherits = codex-target
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
EOF

if ./bondage doctor "$repin_conf" >"$tmpdir/doctor.out" 2>&1; then
  echo "expected doctor to report stale inherited target_fp" >&2
  exit 1
fi
grep -F 'target via defaults "codex-target"' "$tmpdir/doctor.out" >/dev/null
grep -F 'suggest: bondage repin codex' "$tmpdir/doctor.out" >/dev/null

./bondage repin codex "$repin_conf" >"$tmpdir/repin.out"
grep -F 'updated target_fp in defaults "codex-target"' "$tmpdir/repin.out" >/dev/null
awk '
  /^\[defaults "codex-target"\]/ { in_defaults = 1; next }
  /^\[/ { in_defaults = 0 }
  in_defaults && $1 == "target_fp" && $3 == want { found = 1 }
  END { exit found ? 0 : 1 }
' want="$codex_fp" "$repin_conf"
awk '
  /^\[profile "codex"\]/ { in_profile = 1; next }
  /^\[/ { in_profile = 0 }
  in_profile && $1 == "target_fp" { exit 1 }
' "$repin_conf"
./bondage verify codex "$repin_conf" >/dev/null

echo "test-config-defaults: ok"
