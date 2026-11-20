#!/usr/bin/env python3
"""Exercise failure propagation/logging with fake tools, NOT RTL simulation."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
FAKE = '''#!/usr/bin/env python3
import json, os, pathlib, sys
name=pathlib.Path(sys.argv[0]).name
with open('calls.jsonl','a') as f: f.write(json.dumps([name,*sys.argv[1:]])+'\\n')
mode=os.environ['MOCK_MODE']
if mode==name: sys.exit(7)
if name=='xsim':
    if mode=='pass': print('GEMV REGISTER RTL: PASS; reads=1 writes=1 checks=1')
    elif mode=='reported_fail': print('GEMV REGISTER RTL: FAIL: injected error')
    else: print('Run ended without testbench PASS')
'''


def main():
    with tempfile.TemporaryDirectory(prefix="soc_regs_runner_") as temp:
        root = Path(temp)
        tools = root / "bin"
        tools.mkdir()
        for name in ("xvlog", "xelab", "xsim"):
            path = tools / name
            path.write_text(FAKE)
            path.chmod(0o755)
        rtl = root / "generated.v"
        text = "// fixture only; fake tools do not compile this\n"
        rtl.write_text(text)
        for mode in ("pass", "xvlog", "xelab", "xsim", "no_marker", "reported_fail", "missing_rtl"):
            work = root / mode
            env = {k:v for k,v in os.environ.items() if not k.startswith("SOC_GEMV_")}
            env.update(PATH=str(tools)+os.pathsep+env["PATH"], MOCK_MODE=mode,
                       SOC_GEMV_REGISTER_RTL=str(rtl if mode!="missing_rtl" else root/"absent.v"),
                       SOC_GEMV_REGISTER_WORK=str(work))
            if mode=="no_marker":
                work.mkdir()
                (work/"xsim.log").write_text("GEMV REGISTER RTL: PASS; stale previous run\n")
            proc = subprocess.run(["tclsh", str(HERE/"test_rtl.tcl")], env=env,
                                  capture_output=True, text=True)
            assert (proc.returncode == 0) == (mode == "pass"), (mode, proc.stdout, proc.stderr)
            if mode=="missing_rtl":
                assert not work.exists()
            else:
                calls=[json.loads(s)[0] for s in (work/"calls.jsonl").read_text().splitlines()]
                expected=["xvlog"] if mode=="xvlog" else ["xvlog","xelab"] if mode=="xelab" else ["xvlog","xelab","xsim"]
                assert calls==expected, calls
                assert (work/"dut_control.v").read_text()==text
            assert rtl.read_text()==text
            print(f"Runner case {mode}: PASS (tools mocked)")
    print("Register RTL runner tests: PASS; actual RTL simulation NOT executed")


if __name__ == "__main__":
    main()
