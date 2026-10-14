"""Runs the test guests through `xscarlet exec` (Windows only)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile

exe, raw, crt = sys.argv[1:]
UNIMPLEMENTED, FAULT = 0xE0585301, 0xE0585302


def run(*args, mode=""):
    env = dict(os.environ, XS_GUEST_MODE=mode)
    return subprocess.run([exe, *args], capture_output=True, text=True, env=env, timeout=30)


# Next to its DLL the import is the title's own: the DLL gets loaded and called.
local = run("exec", raw, mode="m")
assert local.returncode == 1 and "loaded the title's xs_guest_absent.dll" in local.stderr, local
assert run("exec", raw).returncode == 42

# The rest runs a copy without the DLL, where that import has nothing behind it.
directory = tempfile.TemporaryDirectory()
raw = shutil.copy(raw, directory.name)

plan = json.loads(run("bind", raw).stdout)
origins = {f'{i["module"].lower()}!{i["symbol"]}': i["origin"] for i in plan["imports"]}
assert origins["xs_guest_absent.dll!Absent"] == "missing", origins
assert origins["kernel32.dll!GetModuleHandleW"] == "own", origins
assert origins["kernel32.dll!VirtualAlloc"] == "host", origins
assert plan["missing"] == 1 and plan["execution_supported_on_this_host"] is True

# without --allow-missing it must refuse to start
strict = run("exec", raw)
assert strict.returncode == 1 and "xs_guest_absent.dll!Absent" in strict.stderr, strict
assert "guest-raw-ok" not in strict.stdout

ok = run("exec", raw, "--allow-missing")
assert ok.returncode == 42 and ok.stdout == "guest-raw-ok\n", ok

missing = run("exec", raw, "--allow-missing", mode="m")
assert missing.returncode == UNIMPLEMENTED, missing
assert "unimplemented import xs_guest_absent.dll!Absent" in missing.stderr, missing

fault = run("exec", raw, "--allow-missing", mode="f")
assert fault.returncode == FAULT and "guest exception 0xc0000005 at image+0x" in fault.stderr, fault

assert run("exec", raw, "--allow-missing", mode="e").returncode == 9

crt_plan = json.loads(run("bind", crt).stdout)
assert crt_plan["missing"] == 0, [i for i in crt_plan["imports"] if i["origin"] == "missing"]
result = run("exec", crt)
assert result.returncode == 7 and result.stdout == "guest-crt-ok 6 unwound\n", result

print("guest execution checks passed")
