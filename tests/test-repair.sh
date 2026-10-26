#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

make >/dev/null

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

root="$(pwd)"

# Space-free canonical fixture copies (repo path may contain spaces).
bindir="$tmpdir/bin"
mkdir -p "$bindir"
bindir="$(cd "$bindir" && pwd -P)"
cp "$root/fixtures/fake-nono" "$bindir/fake-nono"
cp "$root/fixtures/fake-codex" "$bindir/fake-codex"
chmod +x "$bindir/fake-nono" "$bindir/fake-codex"

nono_fp="$(./bondage hash-file "$bindir/fake-nono")"
codex_fp="$(./bondage hash-file "$bindir/fake-codex")"

# ---------------------------------------------------------------------------
# 1. Clean config: repair reports nothing to do.
# ---------------------------------------------------------------------------
clean_conf="$tmpdir/clean.conf"
cat >"$clean_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "codex"]
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
target_kind = native
target = $bindir/fake-codex
target_fp = $codex_fp
CONF

out="$((sleep 0.2; printf 'yes\n') | script -q /dev/null ./bondage repair "$clean_conf" 2>&1 || true)"
grep -F "nothing to do" <<<"$out" >/dev/null
grep -F "status: clean" <<<"$out" >/dev/null

# ---------------------------------------------------------------------------
# 3. Routine upgrade: a moved+rehashed target is repaired after confirmation.
# ---------------------------------------------------------------------------
v1="$tmpdir/v1"
v2="$tmpdir/v2"
mkdir -p "$v1" "$v2"
cp "$root/fixtures/fake-codex" "$v1/fake-codex"
cp "$root/fixtures/fake-codex" "$v2/fake-codex"
chmod +x "$v1/fake-codex" "$v2/fake-codex"
v1="$(cd "$v1" && pwd -P)"
v2="$(cd "$v2" && pwd -P)"
v1_fp="$(./bondage hash-file "$v1/fake-codex")"

moved_conf="$tmpdir/moved.conf"
cat >"$moved_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "codex"]
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
target_kind = native
target = $v1/fake-codex
target_fp = $v1_fp
CONF

# Simulate "package moved": remove v1 so repin cannot resolve it.
# brew-style resolution only works under Cellar/Caskroom, so this profile
# must be skipped by repair, not blessed.
rm "$v1/fake-codex"
out="$((sleep 0.2; printf 'yes\n') | script -q /dev/null ./bondage repair "$moved_conf" 2>&1 || true)"
grep -F "skip profile codex" <<<"$out" >/dev/null
grep -F "nothing to do" <<<"$out" >/dev/null

# ---------------------------------------------------------------------------
# 4. Same-version fingerprint change is refused even with confirmation.
# ---------------------------------------------------------------------------
tamper_conf="$tmpdir/tamper.conf"
cp "$root/fixtures/fake-codex" "$bindir/tamper-target"
chmod +x "$bindir/tamper-target"
tamper_fp="$(./bondage hash-file "$bindir/tamper-target")"
cat >"$tamper_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "codex"]
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
target_kind = native
target = $bindir/tamper-target
target_fp = $tamper_fp
CONF

# Modify the binary in place without changing its path: a tamper signal.
printf '# tampered\n' >> "$bindir/tamper-target"

# Non-interactive use is refused, even with piped "yes", when a routine plan
# exists. Build a Cellar-style layout so repin resolves a version move.
cellar="$tmpdir/Cellar/fake-tool"
mkdir -p "$cellar/1.0/bin" "$cellar/2.0/bin"
cp "$root/fixtures/fake-codex" "$cellar/2.0/bin/fake-tool"
chmod +x "$cellar/2.0/bin/fake-tool"
cellar="$(cd "$cellar" && pwd -P)"
cellar_old_fp="sha256:0000000000000000000000000000000000000000000000000000000000000000"

routine_conf="$tmpdir/routine.conf"
cat >"$routine_conf" <<CONF
[global]
nono = $bindir/fake-nono
nono_fp = $nono_fp

[profile "codex"]
use_envchain = false
use_nono = true
nono_profile = custom-codex
touch_policy = none
target_kind = native
target = $cellar/1.0/bin/fake-tool
target_fp = $cellar_old_fp
CONF

set +e
printf 'yes\n' | ./bondage repair "$routine_conf" >"$tmpdir/notty.out" 2>"$tmpdir/notty.err"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
  echo "expected repair to refuse non-interactive stdin" >&2
  exit 1
fi
grep -F "requires an interactive terminal" "$tmpdir/notty.err" >/dev/null
grep -F "target_fp = $cellar_old_fp" "$routine_conf" >/dev/null

# With a pty, the routine plan is shown and confirmed, and the config is
# rewritten to the new version path with a verified rehash.
cp "$routine_conf" "$routine_conf.orig"
out="$((sleep 0.2; printf 'yes\n') | script -q /dev/null ./bondage repair "$routine_conf" 2>&1 || true)"
grep -F "bless target" <<<"$out" >/dev/null
grep -F "configured: $cellar/1.0/bin/fake-tool" <<<"$out" >/dev/null
grep -F "current:    $cellar/2.0/bin/fake-tool" <<<"$out" >/dev/null
grep -F "status: repaired" <<<"$out" >/dev/null
grep -F "target = $cellar/2.0/bin/fake-tool" "$routine_conf" >/dev/null
new_fp="$(./bondage hash-file "$cellar/2.0/bin/fake-tool")"
grep -F "target_fp = $new_fp" "$routine_conf" >/dev/null
./bondage verify codex "$routine_conf" >/dev/null

# A second repair run on the healed config reports clean.
out="$((sleep 0.2; printf 'yes\n') | script -q /dev/null ./bondage repair "$routine_conf" 2>&1 || true)"
grep -F "status: clean" <<<"$out" >/dev/null

out="$( (sleep 0.2; printf 'yes\n') | script -q /dev/null ./bondage repair "$tamper_conf" 2>&1 || true)"
if grep -F "status: repaired" <<<"$out" >/dev/null 2>&1; then
  echo "expected repair to refuse same-version fingerprint change" >&2
  exit 1
fi
grep -F "fingerprint changed but path $bindir/tamper-target is unchanged" <<<"$out" >/dev/null
grep -F "repair refuses to" <<<"$out" >/dev/null
# Config must be untouched.
grep -F "target_fp = $tamper_fp" "$tamper_conf" >/dev/null

# ---------------------------------------------------------------------------
# 5. Confirmation other than "yes" aborts without writing. Use the routine
# config (tamper_conf is refused before ever reaching the prompt).
# ---------------------------------------------------------------------------
cp "$routine_conf.orig" "$routine_conf"
out="$( (sleep 0.2; printf 'y\n') | script -q /dev/null ./bondage repair "$routine_conf" 2>&1 || true)"
if grep -F "status: repaired" <<<"$out" >/dev/null 2>&1; then
  echo "expected repair to abort on wrong confirmation" >&2
  exit 1
fi
grep -F "repair aborted" <<<"$out" >/dev/null
grep -F "target_fp = $cellar_old_fp" "$routine_conf" >/dev/null

# ---------------------------------------------------------------------------
# 6. repin --help / repair --help exist.
# ---------------------------------------------------------------------------
./bondage repair --help 2>&1 | grep -F "Interactive batch repair" >/dev/null

echo "test-repair: ok"
