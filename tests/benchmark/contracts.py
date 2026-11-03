#!/usr/bin/env python3
"""Black-box checks for the measurement bugs found in the recruitment audit."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

bench, sim, compare = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)

    def run(name, *args):
        prefix = root / name
        subprocess.run([bench, '--reps', '1', '--frames', '100', '--out', str(prefix),
                        *args], check=True, stdout=subprocess.DEVNULL)
        data = json.loads(prefix.with_suffix('.json').read_text())
        for arm in data['arms']:
            for row in arm['runs']:
                assert row['offered'] == 100 * arm['clients'], row
                assert sum(row[k] for k in ('ontime', 'late', 'expired', 'abandoned')) == row['offered'], row
                assert 0 <= row['fdr_playout'] <= 1, row
        return data, prefix.with_suffix('.json')

    no_loss, _ = run('none', '--loss', '0', '--seed', '0x5EED')
    assert all(a['runs'][0]['ontime'] == 100 for a in no_loss['arms'])
    assert all(a['seeds'] == [24301] for a in no_loss['arms'])
    all_loss, _ = run('all', '--loss', '1')
    assert all(a['runs'][0]['ontime'] == 0 for a in all_loss['arms'])
    first, a = run('iid-a', '--seed', '0x5EED', '--loss', '0.05')
    _, b = run('iid-b', '--seed', '24301', '--loss', '0.05')
    subprocess.run([sys.executable, compare, str(a), str(b)], check=True)
    run('burst', '--loss', '0.2', '--burst', '4')
    run('fanout', '--loss', '0', '--fanout', '--clients', '3')
    result = root / 'sim.json'
    subprocess.run([sim, '--seed', '0x5EED', '--frames', '10', '--json', str(result)],
                   check=True, stdout=subprocess.DEVNULL)
    assert json.loads(result.read_text())['seed'] == 24301
    # Broken input must fail, rather than compare two empty process substitutions.
    b.write_text('invalid JSON')
    failed = subprocess.run([sys.executable, compare, str(a), str(b)], capture_output=True)
    assert failed.returncode != 0
print('benchmark contracts: PASS')
