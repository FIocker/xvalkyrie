"""Drive the desktop window on X11: once empty, once with a positional file."""
import subprocess
import sys
import tempfile
from pathlib import Path


exe, fixture, xdotool = sys.argv[1:]


def session(args, name):
    """Start xscarlet, wait for a window with this exact name, quit with Ctrl+Q."""
    process = subprocess.Popen([exe, *args], stderr=subprocess.PIPE, text=True)
    try:
        window = subprocess.check_output(
            [xdotool, "search", "--all", "--sync", "--onlyvisible", "--pid", str(process.pid),
             "--name", f"^{name}$"], timeout=10, text=True
        ).splitlines()[0]
        subprocess.run([xdotool, "windowfocus", "--sync", window, "key", "ctrl+q"], check=True)
        _, errors = process.communicate(timeout=10)
        return process.returncode, errors
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


code, errors = session([], "XScarlet")
assert code == 0 and not errors, (code, errors)

with tempfile.TemporaryDirectory() as directory:
    title = Path(directory, "sample title.bin")
    title.write_bytes(Path(fixture).read_bytes())
    code, errors = session([title], "XScarlet - sample title\\.bin")
    assert code == 1 and str(title) in errors, (code, errors)
    assert "Scarlett runtime is not implemented" in errors
