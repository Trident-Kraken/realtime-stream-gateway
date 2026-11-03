#!/usr/bin/env python3
"""Compare deterministic protocol fields; fail on unreadable or invalid JSON."""
import json
import sys


def protocol_fields(value):
    if isinstance(value, dict):
        return {k: protocol_fields(v) for k, v in value.items()
                if k not in {'cpu_s', 'mem_hwm_kb'}}
    if isinstance(value, list):
        return [protocol_fields(v) for v in value]
    return value


if __name__ == '__main__':
    with open(sys.argv[1]) as left, open(sys.argv[2]) as right:
        if protocol_fields(json.load(left)) != protocol_fields(json.load(right)):
            raise SystemExit('bench determinism check: MISMATCH')
    print('bench determinism check: OK (host CPU/RSS excluded)')
