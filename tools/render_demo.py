#!/usr/bin/env python3
"""Render the recorded protocol events as an offline SVG timeline."""
import argparse
import html
import json
from pathlib import Path
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('trace', type=Path)
parser.add_argument('--svg', type=Path, default=Path('docs/demo/gop-recovery.svg'))
parser.add_argument('--replay', action='store_true')
args = parser.parse_args()
data = json.loads(args.trace.read_text())
assert data['checks']['conserved'] and data['checks']['recovered_frames'] == [4, 5]
events = data['events']
if args.replay:
    previous = 0
    for event in events:
        time.sleep(max(0, event['time_ms']-previous) / 1000 * 4)
        print(f"{event['time_ms']:7.2f} ms | frame {event['frame_id']} | {event['event']}", flush=True)
        previous = event['time_ms']

svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="620" viewBox="0 0 1200 620">',
       '<rect width="1200" height="620" fill="#0f172a"/>',
       '<style>text{font-family:Arial,sans-serif;fill:#e2e8f0} .muted{fill:#94a3b8}</style>']
def text(x, y, value, size=14, extra=''):
    svg.append(f'<text x="{x}" y="{y}" font-size="{size}" {extra}>{html.escape(value)}</text>')
def x(ms):
    return 205 + ms * 4.3
text(32, 40, 'A failed GOP, explicit abandonment, then recovery', 25)
text(32, 68, 'Recorded C++ protocol callbacks | seed 24301 | virtual time | synthetic frames', 14, 'class="muted"')
start, end = data['blackout_ms']
svg.append(f'<rect x="{x(start)}" y="94" width="{x(end)-x(start)}" height="418" fill="#7f1d1d" opacity="0.35"/>')
text(x(start)+6, 115, 'Downstream blackout', 13)
for tick in range(0, 221, 20):
    svg.append(f'<path d="M{x(tick)},125 V510" stroke="#334155" stroke-width="1"/>')
    text(x(tick)-9, 535, str(tick), 12, 'class="muted"')
text(1080, 556, 'time / ms', 12, 'class="muted"')
labels = ['0  KEY', '1  DELTA -> 0', '2  DELTA -> 0', '3  DELTA -> 0', '4  KEY', '5  DELTA -> 4']
colors = {'sender:abandoned':'#fb7185', 'sender:pre_admission_dropped':'#fbbf24', 'playout:ontime':'#34d399'}
for fid, label in enumerate(labels):
    y = 155 + fid*62
    text(32, y+5, label, 16)
    lane = [e for e in events if e['frame_id'] == fid]
    offered = next(e for e in lane if e['event'].startswith('offer:'))
    terminal = next(e for e in lane if e['event'] in colors)
    color = colors[terminal['event']]
    svg.append(f'<path d="M{x(offered["time_ms"])},{y} H{x(terminal["time_ms"])}" stroke="{color}" stroke-width="3"/>')
    svg.append(f'<circle cx="{x(offered["time_ms"])}" cy="{y}" r="5" fill="#93c5fd"/>')
    svg.append(f'<circle cx="{x(terminal["time_ms"])}" cy="{y}" r="7" fill="{color}"/>')
    label = {'sender:abandoned':'abandoned', 'sender:pre_admission_dropped':'pruned before admission', 'playout:ontime':'played on time'}[terminal['event']]
    text(x(terminal['time_ms'])+12, y+5, label, 13)
text(32, 582, 'Red: deadline/cascade abandon    Amber: broken-GOP pruning    Green: receiver playout', 14)
text(32, 607, 'This demonstrates state transitions; it is not a video decoder or a performance benchmark.', 12, 'class="muted"')
svg.append('</svg>')
args.svg.parent.mkdir(parents=True, exist_ok=True)
args.svg.write_text('\n'.join(svg)+'\n')
print(f'wrote {args.svg}')
