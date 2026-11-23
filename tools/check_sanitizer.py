#!/usr/bin/env python3
"""Verify actual compilation and dynamic runtime linkage, not just CMake cache."""
import json
from pathlib import Path
import subprocess
import sys

build = Path(sys.argv[1])
mode = sys.argv[2]
modes = {
    'address': (('-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'),
                ('libasan', 'libubsan')),
    'undefined': (('-fsanitize=undefined', '-fno-sanitize-recover=undefined'),
                  ('libubsan',)),
    'thread': (('-fsanitize=thread',), ('libtsan',)),
}
if mode not in modes:
    raise SystemExit(f'unknown sanitizer mode: {mode}')
flags, expected = modes[mode]
commands = json.loads((build / 'compile_commands.json').read_text())
missing = [row['file'] for row in commands if any(f not in row['command'] for f in flags)]
if not commands or missing:
    raise SystemExit(f'missing instrumentation: {missing}')
for name in ('rudp_gateway', 'rudp_client', 'rudp_sim', 'rudp_bench', 'rudp_demo'):
    libs = subprocess.check_output(['ldd', str(build / name)], text=True)
    if any(lib not in libs for lib in expected):
        raise SystemExit(f'missing sanitizer runtime in {name}')
print(f'{mode}: {len(commands)} translation units instrumented; 5 applications linked')
