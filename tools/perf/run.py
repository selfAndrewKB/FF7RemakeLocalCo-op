"""Run one analysis script with this folder importable (python -I leaves the script's folder off sys.path).
    python -I tools/perf/run.py p1_threads.py <capture>\\dump <label>     (outputs land in the current folder)"""
import os
import runpy
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
script = os.path.join(here, sys.argv[1])
sys.argv = [script] + sys.argv[2:]
runpy.run_path(script, run_name='__main__')
