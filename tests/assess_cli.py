"""Exercise the artifact/config association without requiring a console artifact."""
import json
import subprocess
import sys


exe, artifact, config = sys.argv[1:]
args = [exe, "assess", artifact, "--config", config, "--entry"]
result = subprocess.run([*args, "synthetic.pe"], capture_output=True, check=True)
report = json.loads(result.stdout)
assert report["console_target_verified"] is False
assert report["execution_supported"] is False
assert report["target"] == "xbox-scarlett"
assert report["config_evidence"]["family_vs_scarlett"] == "conflict"
assert report["config_evidence"]["declared_architecture_vs_pe"] == "consistent"
symbols = report["standard_imports"][0]["symbols"]
assert symbols[0]["name"] == "Probe" and symbols[1]["ordinal"] == 17
assert all(row["implementation"] == "unsupported" for row in symbols)
bad = subprocess.run([*args, "other.exe"], capture_output=True)
assert bad.returncode != 0 and not bad.stdout and b"not found" in bad.stderr
