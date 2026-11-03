#!/usr/bin/env python3
"""Verify actual compilation and dynamic runtime linkage, not just CMake cache."""
import json
from pathlib import Path
import subprocess
import sys

build = Path(sys.argv[1])
mode = sys.argv[2]
flag = '-fsanitize=address,undefined' if mode == 'address' else '-fsanitize=thread'
commands = json.loads((build / 'compile_commands.json').read_text())
missing = [row['file'] for row in commands if flag not in row['command']]
if not commands or missing:
    raise SystemExit(f'missing instrumentation: {missing}')
for name in ('rudp_gateway', 'rudp_client', 'rudp_sim', 'rudp_bench'):
    libs = subprocess.check_output(['ldd', str(build / name)], text=True)
    expected = ('libasan', 'libubsan') if mode == 'address' else ('libtsan',)
    if any(lib not in libs for lib in expected):
        raise SystemExit(f'missing sanitizer runtime in {name}')
print(f'{mode}: {len(commands)} translation units instrumented; 4 applications linked')
