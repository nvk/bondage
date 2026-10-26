#!/usr/bin/env python3
"""Security regressions using disposable configs, helpers, and real PTYs."""

import errno
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("BONDAGE_TEST_BIN", ROOT / "bondage")).resolve()
ZERO_FP = "sha256:" + "0" * 64


class SecurityRegressions(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bondage-security-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.nono = self.executable("nono", "exit 0")
        self.target = self.executable("target", 'printf "target:%s:%s\\n" "$FIRST" "$SECOND"')
        self.marker = self.root / "helper-ran"
        self.helper = self.executable("helper", 'printf ran > "$1"; printf value')

    def executable(self, relative, body):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\n" + body + "\n")
        path.chmod(0o755)
        return path

    def run_bondage(self, *args):
        return subprocess.run(
            [str(BINARY), *map(str, args)], capture_output=True, text=True,
            env={**os.environ, "BONDAGE_QUIET": "1"}, timeout=15,
        )

    def fingerprint(self, path, tree=False):
        proc = self.run_bondage("hash-tree" if tree else "hash-file", path)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout.strip()

    def profile(self, name="test", target=None, fp=None, extra="", kind="native"):
        target = self.target if target is None else target
        fp = self.fingerprint(target) if fp is None else fp
        return (
            f'\n[profile "{name}"]\nuse_envchain = false\nuse_nono = false\n'
            f"touch_policy = none\ntarget_kind = {kind}\n"
            f"target = {target}\ntarget_fp = {fp}\n{extra}"
        )

    def config(self, sections, globals_extra=""):
        path = self.root / "test.conf"
        path.write_text(
            f"[global]\nnono = {self.nono}\nnono_fp = {self.fingerprint(self.nono)}\n"
            + globals_extra + sections
        )
        return path

    def helper_config(self, pin=None, extra=""):
        pin = self.fingerprint(self.helper) if pin is None else pin
        return self.config(self.profile(extra=(
            f"env_command = FIRST=\t {self.helper}\t{self.marker}\n"
            f"env_command_fp = FIRST={pin}\n" + extra
        )))

    def repair(self, conf):
        master, slave = pty.openpty()
        proc = subprocess.Popen(
            [str(BINARY), "repair", str(conf)], stdin=slave, stdout=slave, stderr=slave,
        )
        os.close(slave)
        output = b""
        confirmed = False
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.1)[0]:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError as exc:
                        if exc.errno != errno.EIO:
                            raise
                        break
                    if not chunk:
                        break
                    output += chunk
                    if not confirmed and b"Type 'yes'" in output:
                        os.write(master, b"yes\n")
                        confirmed = True
                elif proc.poll() is not None:
                    break
            proc.wait(timeout=1)
            return proc.returncode, output.decode()
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)

    def upgrade(self, name):
        new = self.executable(f"Cellar/{name}/2.0/bin/tool", f"printf '{name}\\n'")
        old = self.root / f"Cellar/{name}/1.0/bin/tool"
        return old, new

    def assert_repaired(self, conf):
        code, output = self.repair(conf)
        self.assertEqual(code, 0, output)
        self.assertIn("status: repaired", output)
        proc = self.run_bondage("doctor", conf)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return output

    def assert_refused_unchanged(self, conf, message):
        before = conf.read_bytes()
        code, output = self.repair(conf)
        self.assertNotEqual(code, 0, output)
        self.assertIn(message, output)
        self.assertEqual(conf.read_bytes(), before)

    def test_wrong_or_duplicate_pin_names_fail_closed(self):
        helper_fp = self.fingerprint(self.helper)
        first = f"env_command = FIRST={self.helper} {self.marker}\n"
        second = f"env_command = SECOND={self.helper} {self.marker}\n"
        pins = f"env_command_fp = FIRST={helper_fp}\n"
        cases = {
            "wrong name": first + f"env_command_fp = TYPO={helper_fp}\n",
            "duplicate pin": first + second + pins * 2,
            "duplicate command": first * 2 + pins + f"env_command_fp = SECOND={helper_fp}\n",
        }
        for name, entries in cases.items():
            with self.subTest(name=name):
                conf = self.config(self.profile(extra=entries))
                for command in ("verify", "exec"):
                    proc = self.run_bondage(command, "test", conf)
                    self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertFalse(self.marker.exists())

    def test_inherited_name_mismatch_is_rejected(self):
        conf = self.config(
            '[defaults "helpers"]\n'
            f"env_command = FIRST={self.helper} {self.marker}\n"
            f"env_command_fp = TYPO={self.fingerprint(self.helper)}\n"
            + self.profile(extra="inherits = helpers\n")
        )
        proc = self.run_bondage("verify", "test", conf)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("exactly one matching env_command_fp", proc.stderr)

    def test_pin_order_does_not_matter(self):
        second = self.executable("helper2", "printf second")
        conf = self.config(self.profile(extra=(
            f"env_command = FIRST={self.helper} {self.marker}\n"
            f"env_command = SECOND={second}\n"
            f"env_command_fp = SECOND={self.fingerprint(second)}\n"
            f"env_command_fp = FIRST={self.fingerprint(self.helper)}\n"
        )))
        proc = self.run_bondage("exec", "test", conf)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("target:value:second", proc.stdout)

    def test_helper_audits_do_not_execute_helpers(self):
        conf = self.helper_config()
        for args in (("verify", "test"), ("doctor",), ("repair",)):
            proc = self.run_bondage(*args, conf)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertFalse(self.marker.exists())
        self.assertIn("ok: env_command FIRST", self.run_bondage("verify", "test", conf).stdout)

    def test_stale_missing_and_nonexecutable_helpers_fail_audits(self):
        for problem in ("stale", "missing", "nonexecutable"):
            with self.subTest(problem=problem):
                self.executable("helper", 'printf ran > "$1"; printf value')
                conf = self.helper_config()
                if problem == "stale":
                    self.helper.write_text(self.helper.read_text() + "# changed\n")
                elif problem == "missing":
                    self.helper.unlink()
                else:
                    self.helper.chmod(0o644)
                before = conf.read_bytes()
                for args in (("verify", "test"), ("doctor",), ("repair",), ("exec", "test")):
                    proc = self.run_bondage(*args, conf)
                    self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertEqual(conf.read_bytes(), before)
                self.assertFalse(self.marker.exists())

    def test_stale_helper_blocks_other_repairs(self):
        old, _ = self.upgrade("a")
        conf = self.config(self.profile("a", old, ZERO_FP) + self.profile(extra=(
            f"env_command = FIRST={self.helper} {self.marker}\n"
            f"env_command_fp = FIRST={ZERO_FP}\n"
        )))
        self.assert_refused_unchanged(conf, "env_command FIRST fingerprint mismatch")
        self.assertFalse(self.marker.exists())

    def test_repin_does_not_partially_write_with_stale_helper(self):
        old, _ = self.upgrade("a")
        conf = self.config(self.profile(target=old, fp=ZERO_FP, extra=(
            f"env_command = FIRST={self.helper} {self.marker}\n"
            f"env_command_fp = FIRST={ZERO_FP}\n"
        )))
        before = conf.read_bytes()
        proc = self.run_bondage("repin", "test", conf)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("env_command FIRST fingerprint mismatch", proc.stderr)
        self.assertEqual(conf.read_bytes(), before)
        self.assertFalse(self.marker.exists())

    def test_helper_is_rechecked_after_touch_helper(self):
        touch = self.executable("touch", f"printf '# changed\\n' >> '{self.helper}'")
        conf = self.helper_config()
        sections = conf.read_text().replace("touch_policy = none", "touch_policy = prompt")
        sections = sections.replace("[global]\n", (
            f"[global]\ntouchid = {touch}\ntouchid_fp = {self.fingerprint(touch)}\n"
        ))
        conf.write_text(sections)
        proc = self.run_bondage("exec", "test", conf)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("env_command FIRST fingerprint mismatch", proc.stderr)
        self.assertFalse(self.marker.exists())

    def test_two_native_upgrades_keep_distinct_targets_in_either_order(self):
        upgrades = {name: self.upgrade(name) for name in ("a", "b")}
        for order in (("a", "b"), ("b", "a")):
            with self.subTest(order=order):
                conf = self.config("".join(self.profile(name, upgrades[name][0], ZERO_FP) for name in order))
                output = self.assert_repaired(conf)
                for name, (old, new) in upgrades.items():
                    self.assertIn(str(old), output)
                    self.assertIn(str(new), output)
                    self.assertIn(f"target_fp = {self.fingerprint(new)}", conf.read_text())
                    proc = self.run_bondage("exec", name, conf)
                    self.assertEqual(proc.returncode, 0, proc.stderr)
                    self.assertEqual(proc.stdout.strip(), name)
                self.assertIn("status: clean", self.run_bondage("repair", conf).stdout)

    def test_distinct_defaults_and_shared_aliases(self):
        sections = ""
        for name in ("a", "b"):
            old, _ = self.upgrade(name)
            sections += f'[defaults "{name}"]\ntarget_kind = native\ntarget = {old}\ntarget_fp = {ZERO_FP}\n'
        for name, owner in (("a", "a"), ("alias-a", "a"), ("b", "b")):
            sections += (f'[profile "{name}"]\ninherits = {owner}\nuse_envchain = false\n'
                         'use_nono = false\ntouch_policy = none\n')
        sections += self.profile("unchanged")
        conf = self.config(sections)
        self.assert_repaired(conf)
        for name, expected in (("a", "a"), ("alias-a", "a"), ("b", "b")):
            proc = self.run_bondage("exec", name, conf)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stdout.strip(), expected)
        self.assertIn(f"target = {self.target}", conf.read_text())

    def script_profile(self, name, move_interpreter=False):
        old, new = self.upgrade(name)
        interp = Path("/bin/sh").resolve()
        interp_fp = self.fingerprint(interp)
        if move_interpreter:
            interp_old, interp_new = self.upgrade(name + "-interpreter")
            self.executable(interp_new.relative_to(self.root), 'exec /bin/sh "$@"')
            interp, interp_fp = interp_old, ZERO_FP
        return self.profile(name, old, ZERO_FP, kind="script", extra=(
            f"interpreter = {interp}\ninterpreter_fp = {interp_fp}\n"
            f"package_root = {old.parent}\npackage_tree_fp = {ZERO_FP}\n"
        )), new

    def test_script_upgrade_with_unchanged_interpreter_and_native_upgrade(self):
        sections, new = self.script_profile("script")
        old_native, _ = self.upgrade("native")
        conf = self.config(self.profile("native", old_native, ZERO_FP) + sections)
        self.assert_repaired(conf)
        self.assertIn(f"package_root = {new.parent}", conf.read_text())
        self.assertEqual(self.run_bondage("exec", "script", conf).stdout.strip(), "script")

    def test_two_script_artifact_sets_remain_distinct(self):
        conf = self.config("".join(self.script_profile(name, True)[0] for name in ("a", "b")))
        self.assert_repaired(conf)
        for name in ("a", "b"):
            proc = self.run_bondage("exec", name, conf)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stdout.strip(), name)

    def test_interpreter_upgrade_with_unchanged_target_and_package(self):
        old, new = self.upgrade("interpreter")
        self.executable(new.relative_to(self.root), 'exec /bin/sh "$@"')
        script = self.executable("package/run.sh", "printf script")
        conf = self.config(self.profile(target=script, kind="script", extra=(
            f"interpreter = {old}\ninterpreter_fp = {ZERO_FP}\n"
            f"package_root = {script.parent}\npackage_tree_fp = {self.fingerprint(script.parent, True)}\n"
        )))
        self.assert_repaired(conf)
        proc = self.run_bondage("exec", "test", conf)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), "script")

    def test_later_profile_tampering_cannot_be_hidden_by_upgrade(self):
        old, _ = self.upgrade("a")
        conf = self.config(self.profile("a", old, ZERO_FP) + self.profile("tampered", fp=ZERO_FP))
        self.assert_refused_unchanged(conf, "target fingerprint changed")

    def test_interpreter_tampering_is_still_refused(self):
        old, _ = self.upgrade("script")
        conf = self.config(self.profile(target=old, fp=ZERO_FP, kind="script", extra=(
            f"interpreter = {Path('/bin/sh').resolve()}\ninterpreter_fp = {ZERO_FP}\n"
        )))
        self.assert_refused_unchanged(conf, "interpreter fingerprint changed")

    def test_package_tampering_is_still_refused(self):
        old, _ = self.upgrade("interpreter")
        script = self.executable("package/run.sh", "printf script")
        conf = self.config(self.profile(target=script, kind="script", extra=(
            f"interpreter = {old}\ninterpreter_fp = {ZERO_FP}\n"
            f"package_root = {script.parent}\npackage_tree_fp = {ZERO_FP}\n"
        )))
        self.assert_refused_unchanged(conf, "package_root fingerprint changed")

    def test_conflicting_shared_owner_updates_leave_config_untouched(self):
        sections = f'[defaults "shared-fp"]\ntarget_fp = {ZERO_FP}\n'
        for name in ("a", "b"):
            old, _ = self.upgrade(name)
            sections += self.profile(name, old, ZERO_FP, extra="inherits = shared-fp\n").replace(
                f"target_fp = {ZERO_FP}\n", ""
            )
        conf = self.config(sections)
        self.assert_refused_unchanged(conf, "conflicting repin updates for target_fp")


if __name__ == "__main__":
    unittest.main(verbosity=2)
