"""Command-line, configuration and terminal regression tests for baudsh.

Run with: python -m unittest discover -s tests -v
Set BAUDSH_BINARY to test an executable outside the repository root.
"""

import errno
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

if os.name == "posix":
    import fcntl
    import pty
    import select
    import termios


ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("BAUDSH_BINARY", str(ROOT / ("baudsh.exe" if os.name == "nt" else "baudsh")))).resolve()
PREFIX = b"\x1b[18;5~"


class IsolatedTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.is_file():
            raise RuntimeError(f"Build baudsh first or set BAUDSH_BINARY: {BINARY}")

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="baudsh-test-")
        self.addCleanup(self.directory.cleanup)
        self.home = Path(self.directory.name)
        self.env = dict(os.environ, HOME=str(self.home), USERPROFILE=str(self.home),
                        APPDATA=str(self.home / "AppData"), PYTHONIOENCODING="utf-8")
        self.config = self.home / ("AppData" if os.name == "nt" else ".config") / "baudsh" / "baudsh.conf"
        self.config.parent.mkdir(parents=True)

    def cli(self, *args):
        return subprocess.run([str(BINARY), *args], env=self.env, capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=10)

    def effective(self, *args):
        result = self.cli(*args, "--print-effective")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)


class CommandLineTests(IsolatedTest):
    def test_help_and_version_without_terminal(self):
        self.assertEqual(self.cli("--help").returncode, 0)
        version = self.cli("--version")
        self.assertEqual(version.returncode, 0)
        self.assertIn("baudsh 1.0.0", version.stdout)

    def test_invalid_options_fail(self):
        invalid = [("--baud", "10junk"), ("--baud", "2147483648"), ("--baud", "0"),
                   ("--data-bits", "8junk"), ("--stop-bits", "nan"), ("--stop-bits", "inf"),
                   ("--parity", "no"), ("--burst-ms", "0"), ("--exit-arm-ms", "-1"),
                   ("--baud",), ("--cmd",), ("--unknown-option",)]
        for args in invalid:
            with self.subTest(args=args):
                self.assertEqual(self.cli("--baud", "9600", *args).returncode, 64)

    def test_option_values_are_not_early_flags(self):
        settings = self.effective("--baud", "9600", "--exit-key", "--version")
        self.assertEqual(settings["exit-key"], "--version")
        self.assertEqual(self.cli("--baud", "--version").returncode, 64)

    def test_completion_shell_validation(self):
        for shell in ("bash", "zsh", "fish", "dash"):
            with self.subTest(shell=shell):
                result = self.cli("--autocomp", shell)
                self.assertEqual(result.returncode, 0)
                self.assertTrue(result.stdout.strip())
        self.assertEqual(self.cli("--autocomp", "unsupported").returncode, 64)
        self.assertEqual(self.cli("--autocomp").returncode, 64)

    def test_default_template_round_trip(self):
        defaults = self.cli("--print-default")
        self.assertEqual(defaults.returncode, 0)
        self.config.write_text(defaults.stdout, encoding="utf-8")
        values = self.effective()
        self.assertEqual(values["baud"], "-1")
        self.assertEqual(values["data-bits"], "8")
        self.assertEqual(values["parity"], "n")
        self.assertEqual(values["burst-ms"], "200")

    def test_sample_configuration(self):
        self.config.write_bytes((ROOT / "extras" / "baudsh.conf").read_bytes())
        values = self.effective()
        self.assertEqual(values["baud"], "9600")
        self.assertEqual(values["parity"], "e")
        self.assertEqual(values["exit-key"], "ctrl+f7")

    def test_configuration_comments_bom_and_whitespace(self):
        self.config.write_text("\ufeffbaud = 300 # comment\r\nparity n\nexit-key raw:abc=def;ghi\n",
                               encoding="utf-8")
        values = self.effective()
        self.assertEqual(values["baud"], "300")
        self.assertEqual(values["exit-key"], "raw:abc=def;ghi")

    def test_long_configuration_lines_are_not_split(self):
        self.config.write_text("#" + "x" * 4094 + "baud=1\nbaud=300\nexit-key raw:" + "x" * 5000 + "\n",
                               encoding="utf-8")
        values = self.effective()
        self.assertEqual(values["baud"], "300")
        self.assertEqual(values["exit-key"], "raw:" + "x" * 5000)

    def test_invalid_configuration_and_cli_override(self):
        for field in ("baud=10junk", "baud=-1", "data-bits=9", "parity=extra", "stop-bits=nan",
                      "burst-ms=0", "exit-arm-ms=-1"):
            with self.subTest(field=field):
                self.config.write_text("baud=300\n" + field + "\n", encoding="utf-8")
                self.assertEqual(self.cli("--print-effective").returncode, 64)
        self.config.write_text("baud=10junk\n", encoding="utf-8")
        self.assertEqual(self.effective("--baud", "2400")["baud"], "2400")

    def test_configuration_precedence(self):
        self.config.write_text("baud=300\n", encoding="utf-8")
        (self.home / ".baudsh.rc").write_text("baud=1200\n", encoding="utf-8")
        self.assertEqual(self.effective()["baud"], "1200")
        self.assertEqual(self.effective("--baud", "9600")["baud"], "9600")


class PtySession:
    """Keep the outer slave open so terminal state can be inspected after exit."""
    def __init__(self, args, env):
        self.master, self.slave = pty.openpty()
        self.attrs = termios.tcgetattr(self.slave)
        self.flags = fcntl.fcntl(self.slave, fcntl.F_GETFL)
        self.output = bytearray()
        self.process = subprocess.Popen([str(BINARY), "--baud", "115200", "--exit-arm-ms", "0", *args],
                                        stdin=self.slave, stdout=self.slave, stderr=self.slave,
                                        env=env, start_new_session=True)

    def pump(self, timeout=0.02):
        if select.select([self.master], [], [], timeout)[0]:
            try:
                self.output.extend(os.read(self.master, 65536))
            except OSError as exc:
                if exc.errno != errno.EIO:
                    raise

    def until(self, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise AssertionError(f"PTY timeout; exit={self.process.poll()}, tail={bytes(self.output[-200:])!r}")
            self.pump()

    def ready(self):
        self.until(lambda: b"READY" in self.output)

    def finish(self, timeout=10):
        self.until(lambda: self.process.poll() is not None, timeout)
        while select.select([self.master], [], [], 0)[0]:
            self.pump(0)
        return self.process.wait(), bytes(self.output)

    def restored(self):
        return termios.tcgetattr(self.slave) == self.attrs and fcntl.fcntl(self.slave, fcntl.F_GETFL) == self.flags

    def close(self):
        if self.process.poll() is None:
            os.kill(self.process.pid, signal.SIGCONT)
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        os.close(self.master)
        os.close(self.slave)


@unittest.skipUnless(os.name == "posix", "requires POSIX PTYs")
class PosixTerminalTests(IsolatedTest):
    def session(self, *args):
        session = PtySession(args, self.env)
        self.addCleanup(session.close)
        return session

    def child(self, code, *options):
        return self.session(*options, "--cmd", sys.executable, "-c", code)

    def test_child_exit_status_and_terminal_restoration(self):
        session = self.child("import sys; sys.exit(37)")
        self.assertEqual(session.finish()[0], 37)
        self.assertTrue(session.restored())

    def test_child_signal_status(self):
        session = self.child("import os,signal; os.kill(os.getpid(),signal.SIGTERM)")
        self.assertEqual(session.finish()[0], 128 + signal.SIGTERM)
        self.assertTrue(session.restored())

    def test_suspend_resume_restores_terminal(self):
        session = self.child("import os,time; os.write(1,b'READY'); time.sleep(10)")
        session.ready()
        os.kill(session.process.pid, signal.SIGTSTP)
        deadline = time.monotonic() + 5
        while True:
            pid, status = os.waitpid(session.process.pid, os.WNOHANG | os.WUNTRACED)
            if pid:
                self.assertTrue(os.WIFSTOPPED(status))
                break
            self.assertLess(time.monotonic(), deadline, "wrapper did not suspend")
            time.sleep(0.01)
        self.assertTrue(session.restored(), "terminal state was not restored while suspended")
        os.kill(session.process.pid, signal.SIGCONT)
        deadline = time.monotonic() + 5
        while termios.tcgetattr(session.slave) == session.attrs:
            self.assertLess(time.monotonic(), deadline, "raw mode was not reapplied")
            time.sleep(0.01)
        os.kill(session.process.pid, signal.SIGTERM)
        self.assertEqual(session.finish()[0], 128 + signal.SIGTERM)
        self.assertTrue(session.restored())

    def test_single_string_shell_command(self):
        session = self.session("--cmd", "printf shell-command-ok")
        status, output = session.finish()
        self.assertEqual(status, 0)
        self.assertEqual(output, b"shell-command-ok")

    def test_child_option_is_not_wrapper_option(self):
        session = self.session("--cmd", sys.executable, "-c", "import sys; print(sys.argv[1])", "--version")
        status, output = session.finish()
        self.assertEqual(status, 0)
        self.assertEqual(output.strip(), b"--version")

    def receive(self, payload, expected):
        code = ("import os,tty; tty.setraw(0); os.write(1,b'READY'); data=b''\n"
                f"while len(data)<{len(expected)}: data+=os.read(0,{len(expected)}-len(data))\n"
                "os.write(1,b'HEX:'+data.hex().encode())")
        session = self.child(code)
        session.ready()
        os.write(session.master, payload)
        status, output = session.finish()
        self.assertEqual(status, 0)
        self.assertIn(b"HEX:" + expected.hex().encode(), output)
        self.assertTrue(session.restored())

    def test_lone_escape_is_forwarded(self):
        self.receive(b"\x1b", b"\x1b")

    def test_duplicate_escape_prefix_is_literal(self):
        self.receive(PREFIX + PREFIX, PREFIX)

    def test_partial_second_prefix_does_not_run_local_commands(self):
        for suffix in (b"q", b"?", b"h"):
            with self.subTest(suffix=suffix):
                self.receive(PREFIX + b"\x1b" + suffix, b"\x1b" + suffix)

    def test_escape_quit_restores_terminal(self):
        session = self.child("import os,time; os.write(1,b'READY'); time.sleep(10)")
        session.ready()
        os.write(session.master, PREFIX + b"q")
        self.assertEqual(session.finish()[0], 0)
        self.assertTrue(session.restored())

    def test_queued_input_does_not_prevent_exit(self):
        session = self.child("import time; time.sleep(0.3)", "--baud", "10")
        time.sleep(0.1)
        os.write(session.master, b"x" * 200)
        self.assertEqual(session.finish(timeout=5)[0], 0)
        self.assertTrue(session.restored())

    def test_bulk_output_is_not_truncated(self):
        session = self.child("import os; os.write(1,b'Z'*200000)", "--baud", "10000000")
        status, output = session.finish(timeout=15)
        self.assertEqual(status, 0)
        self.assertEqual(output, b"Z" * 200000)

    def test_output_throttling(self):
        start = time.monotonic()
        session = self.child("import os; os.write(1,b'Z'*50)", "--baud", "300", "--burst-ms", "10")
        status, output = session.finish(timeout=15)
        elapsed = time.monotonic() - start
        self.assertEqual(status, 0)
        self.assertEqual(output, b"Z" * 50)
        self.assertGreater(elapsed, 1.3, "output was not throttled at 300 baud / 8N1")

    def test_outer_terminal_disconnect_exits(self):
        master, slave = pty.openpty()
        process = subprocess.Popen([str(BINARY), "--baud", "9600", "--cmd", sys.executable, "-c",
                                    "import time; time.sleep(10)"], stdin=slave, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, env=self.env, start_new_session=True)
        try:
            time.sleep(0.2)
            os.close(master)
            master = None
            process.communicate(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            if master is not None:
                os.close(master)
            os.close(slave)


@unittest.skipUnless(os.name == "nt", "requires native Windows ConPTY")
class WindowsTerminalTests(IsolatedTest):
    def command(self, *args, baud="10000000"):
        # Keep stdin open while the command runs. Closing it at spawn tests
        # ConPTY input shutdown instead of normal command output draining.
        process = subprocess.Popen([str(BINARY), "--baud", baud, "--exit-key", "none", "--cmd", *args],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=self.env)
        input_pipe = process.stdin
        process.stdin = None  # communicate() must not close ConPTY input early.
        try:
            output, error = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        finally:
            input_pipe.close()
        return process.returncode, output.decode("utf-8", errors="replace"), error.decode("utf-8", errors="replace")

    def test_child_exit_status(self):
        status, output, error = self.command(sys.executable, "-c", "import sys;sys.exit(37)")
        self.assertEqual(status, 37, output + error)

    def test_direct_arguments_quotes_unicode_and_backslashes(self):
        arguments = ["with spaces", 'embedded"quote', "trailing\\", "árvíz_日本語", "--version"]
        code = "import sys;print('|'.join(a.encode('utf-8').hex() for a in sys.argv[1:]),flush=True)"
        status, output, error = self.command(sys.executable, "-c", code, *arguments)
        self.assertEqual(status, 0, output + error)
        expected = "|".join(arg.encode("utf-8").hex() for arg in arguments)
        self.assertIn(expected, re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", output).replace("\r", "").replace("\n", ""))

    def test_single_string_shell_quotes(self):
        status, output, error = self.command('echo "quoted shell value"')
        self.assertEqual(status, 0, output + error)
        self.assertIn('"quoted shell value"', output)
        self.assertNotIn('\\"quoted', output)

    def test_output_is_drained_after_child_exit(self):
        status, output, error = self.command(sys.executable, "-c", "import os;os.write(1,b'Z'*50)", baud="300")
        self.assertEqual(status, 0, output + error)
        self.assertEqual(output.count("Z"), 50)

    def test_partial_second_prefix_does_not_quit(self):
        self.receive_custom_prefix("raw:abc", b"abcabq", b"abq")

    def test_short_hex_like_prefix_is_literal(self):
        self.receive_custom_prefix("raw:ab", b"abaq", b"aq")

    def test_legacy_spaced_hex_prefix(self):
        self.receive_custom_prefix("raw:61 62 63", b"abcabq", b"abq")

    def receive_custom_prefix(self, prefix, payload, expected):
        # getwch() reads console key events without enabling line buffering.
        code = ("import msvcrt;print('READY',flush=True);"
                f"data=''.join(msvcrt.getwch() for _ in range({len(expected)}));"
                "print('HEX:'+data.encode('utf-8').hex(),flush=True)")
        process = subprocess.Popen([str(BINARY), "--baud", "10000000", "--exit-arm-ms", "0",
                                    "--exit-key", prefix,
                                    "--cmd", sys.executable, "-c", code],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   env=self.env)
        output = bytearray()
        ready = threading.Event()

        def read_output():
            while True:
                byte = process.stdout.read(1)
                if not byte:
                    return
                output.extend(byte)
                if b"READY" in output:
                    ready.set()

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        try:
            self.assertTrue(ready.wait(10), "ConPTY child did not become ready")
            process.stdin.write(payload)
            process.stdin.flush()
            self.assertEqual(process.wait(timeout=10), 0)
            reader.join(timeout=5)
            self.assertFalse(reader.is_alive(), "output pipe did not close")
            self.assertIn(b"HEX:" + expected.hex().encode(), output)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
            reader.join(timeout=5)
            process.stdout.close()

    def test_unicode_configuration_path(self):
        directory = self.home / "árvíz_日本語"
        directory.mkdir()
        self.env["HOME"] = self.env["USERPROFILE"] = str(directory)
        (directory / ".baudsh.rc").write_text("baud=4800\n", encoding="utf-8")
        self.assertEqual(self.effective()["baud"], "4800")


if __name__ == "__main__":
    unittest.main()
