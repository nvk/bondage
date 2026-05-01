#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

make >/dev/null

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

root="$(pwd)"
nono_fp="$(./bondage hash-file "$root/fixtures/fake-nono")"
codex_fp="$(./bondage hash-file "$root/fixtures/fake-codex")"

conf="$tmpdir/bondage.conf"
cat >"$conf" <<EOF
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
EOF

./bondage --help >"$tmpdir/help.out"
grep -F -- '--config <path>' "$tmpdir/help.out" >/dev/null

./bondage --config "$conf" status >"$tmpdir/status.out"
grep -F 'config:' "$tmpdir/status.out" >/dev/null
grep -F -- '- codex' "$tmpdir/status.out" >/dev/null

./bondage -c "$conf" verify codex >/dev/null
./bondage --config="$conf" verify codex >/dev/null
BONDAGE_CONF="$conf" ./bondage verify codex >/dev/null

legacy_argv="$(./bondage argv codex "$conf" -- --help)"
global_argv="$(./bondage --config "$conf" argv codex -- --help)"
grep -F 'argv[6] = --help' <<<"$legacy_argv" >/dev/null
grep -F 'argv[6] = --help' <<<"$global_argv" >/dev/null

if ./bondage --bogus status >"$tmpdir/bogus.out" 2>&1; then
  echo "expected unknown global option to fail" >&2
  exit 1
fi
grep -F 'unknown option' "$tmpdir/bogus.out" >/dev/null

if ./bondage --config "$conf" verify codex "$conf" >"$tmpdir/double-config.out" 2>&1; then
  echo "expected duplicate config source to fail" >&2
  exit 1
fi
grep -F 'config supplied both with --config and positional argument' \
  "$tmpdir/double-config.out" >/dev/null

echo "test-cli-options: ok"
