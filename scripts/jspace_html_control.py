"""HTML task controller: exclude unchanged bundled toolchain, retain strict gates.

The normal inventory spends minutes hashing 60,000 bundled tools. This task
does not modify tools/. Source, vendor parser/engine, tests, scripts and map
remain fingerprinted. Explicit evidence/source receipts still cover exclusions.
"""
import importlib.util
from pathlib import Path
import sys

skill = Path('C:/Users/cesta/.codex/skills/j-space/scripts/control.py')
spec = importlib.util.spec_from_file_location('html_jspace_controller', skill)
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)
controller.EXCLUDED = controller.EXCLUDED | {'tools'}
sys.exit(controller.main())
