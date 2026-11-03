#!/usr/bin/env python3
"""Real gateway/client processes through a bounded loopback UDP fault proxy."""
import argparse
import heapq
import json
import platform
from pathlib import Path
import random
import selectors
import socket
import struct
import subprocess
import threading
import time


class FaultProxy:
    """One upstream socket per client preserves the gateway's session identity."""
    def __init__(self, gateway, seed, loss=0, burst=0, outage=False):
        self.gateway = gateway
        self.rng = random.Random(seed)
        self.loss, self.burst, self.outage = loss, burst, outage
        self.selector = selectors.DefaultSelector()
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.setblocking(False)
        self.selector.register(self.listener, selectors.EVENT_READ, None)
        self.port = self.listener.getsockname()[1]
        self.peers = {}
        self.pending = []
        self.events = []
        self.started = time.monotonic()
        self.stats = dict(data_attempts=0, data_dropped=0, retransmissions=0,
                          datagram_bytes=0, queue_overflow=0, queue_high_water=0)
        self.burst_left = 0
        self.error = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            raise RuntimeError('proxy did not stop')
        for key in list(self.selector.get_map().values()):
            key.fileobj.close()
        self.selector.close()
        if self.error:
            raise RuntimeError('proxy failed') from self.error

    def _run(self):
        try:
            self._loop()
        except Exception as error:
            self.error = error

    def _loop(self):
        serial = 0
        while not self.stop.is_set():
            now = time.monotonic()
            while self.pending and self.pending[0][0] <= now:
                _, _, sock, peer, packet = heapq.heappop(self.pending)
                sock.sendto(packet, peer)
            timeout = min(0.01, max(0, self.pending[0][0] - now)) if self.pending else 0.01
            for key, _ in self.selector.select(timeout):
                for _ in range(64):  # bounded receive turn
                    try:
                        packet, peer = key.fileobj.recvfrom(65535)
                    except BlockingIOError:
                        break
                    downstream = key.data is not None
                    if downstream:
                        dest, out_socket = key.data, self.listener
                    else:
                        if peer not in self.peers:
                            if len(self.peers) >= 16:
                                continue
                            upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                            upstream.bind(('127.0.0.1', 0))
                            upstream.setblocking(False)
                            self.peers[peer] = upstream
                            self.selector.register(upstream, selectors.EVENT_READ, peer)
                        dest, out_socket = self.gateway, self.peers[peer]
                    now = time.monotonic()
                    elapsed = now - self.started
                    kind = packet[1] if len(packet) >= 28 else 0
                    is_data = downstream and kind == 1
                    drop = False
                    if is_data:
                        self.stats['data_attempts'] += 1
                        flags = struct.unpack_from('!H', packet, 2)[0]
                        if flags & 1:
                            self.stats['retransmissions'] += 1
                        if self.outage and 0.8 <= elapsed < 1.2:
                            drop = True
                        elif self.burst_left:
                            self.burst_left -= 1
                            drop = True
                        elif self.loss:
                            probability = self.loss / (self.burst * (1-self.loss)) if self.burst > 1 else self.loss
                            if self.rng.random() < probability:
                                drop = True
                                self.burst_left = max(0, self.burst-1)
                        self.stats['data_dropped'] += int(drop)
                    self.stats['datagram_bytes'] += len(packet)
                    if downstream and kind in (1, 6):
                        self.events.append(dict(time_ms=round(elapsed*1000, 3), type=kind,
                            seq=struct.unpack_from('!I', packet, 8)[0],
                            frame_id=struct.unpack_from('!I', packet, 12)[0],
                            action='drop' if drop else 'schedule', bytes=len(packet)))
                    if drop:
                        continue
                    if len(self.pending) >= 4096:
                        self.stats['queue_overflow'] += 1
                        continue
                    serial += 1
                    delay = max(0, 0.005 + self.rng.uniform(-0.002, 0.002))
                    heapq.heappush(self.pending, (now+delay, serial, out_socket, dest, packet))
                    self.stats['queue_high_water'] = max(self.stats['queue_high_water'], len(self.pending))


def final_snapshot(path):
    rows = [json.loads(line) for line in path.read_text().splitlines() if line]
    if not rows or not rows[-1]['final']:
        raise RuntimeError(f'no final snapshot: {path}')
    return rows[-1]


def run_cell(build, output, name, seed, duration):
    folder = output / f'{name}-{seed}'
    folder.mkdir(parents=True, exist_ok=True)
    # Ask the OS for an unused loopback port, then let the gateway bind it.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    proxy = FaultProxy(('127.0.0.1', port), seed,
                       loss=0.05 if name == 'iid-5' else 0.2 if name == 'burst-20-4' else 0,
                       burst=4 if name == 'burst-20-4' else 0, outage=name == 'outage-400ms')
    processes, logs = [], []
    started = False
    try:
        def spawn(binary, args, log):
            handle = (folder / log).open('w')
            logs.append(handle)
            process = subprocess.Popen([str(build / binary), *args], stdout=handle, stderr=subprocess.STDOUT)
            processes.append(process)
            return process
        gateway = spawn('rudp_gateway', ['--port', str(port), '--duration', str(duration),
                         '--metrics-out', str(folder/'gateway.jsonl')], 'gateway.log')
        proxy.start()
        started = True
        count = 3 if name == 'fanout-3' else 1
        clients = [spawn('rudp_client', ['--port', str(proxy.port), '--duration', str(duration+1200),
                   '--metrics-out', str(folder/f'client-{i}.jsonl')], f'client-{i}.log') for i in range(count)]
        for process in [gateway, *clients]:
            if process.wait(timeout=duration/1000 + 8) != 0:
                raise RuntimeError(f'process failed; logs in {folder}')
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if started:
            proxy.close()
        else:
            proxy.listener.close()
            proxy.selector.close()
        for log in logs:
            log.close()
    gateway = final_snapshot(folder/'gateway.jsonl')
    clients = [final_snapshot(folder/f'client-{i}.jsonl') for i in range(count)]
    if gateway['watchdog_violations'] or gateway['resource_exhausted'] or proxy.stats['queue_overflow']:
        raise RuntimeError(f'bounded-resource or watchdog failure in {folder}')
    if any(s['frames']['in_flight'] for s in gateway['sessions']):
        raise RuntimeError('unclosed session frames')
    offered = gateway['session_offered']
    ontime = sum(c['delivered_ontime'] for c in clients)
    terminal = sum(sum(c[k] for k in ('delivered_ontime','delivered_late','delivery_expired','delivery_abandoned')) for c in clients)
    if not offered or not ontime or terminal > offered:
        raise RuntimeError(f'invalid frame accounting in {folder}: {terminal}/{offered}')
    if name == 'outage-400ms':
        before = {e['frame_id'] for e in proxy.events
                  if e['type'] == 1 and e['action'] == 'schedule' and e['time_ms'] < 1200}
        if ontime <= len(before):
            raise RuntimeError('no evidence of receiver recovery after outage')
    with (folder/'packet-events.jsonl').open('w') as log:
        for event in proxy.events:
            log.write(json.dumps(event)+'\n')
    result = dict(scenario=name, seed=seed, clients=count, duration_ms=duration,
                  offered=offered, ontime=ontime, fdr_playout=ontime/offered,
                  session_fdr=gateway['session_fdr'], unobserved=offered-terminal,
                  proxy=proxy.stats)
    (folder/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(f'{name} seed={seed}: {ontime}/{offered} playout={ontime/offered:.3f}', flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--out', type=Path, default=Path('docs/experiments/socket'))
    parser.add_argument('--duration', type=int, default=3000)
    parser.add_argument('--reps', type=int, default=3)
    parser.add_argument('--scenario', choices=['no-loss','iid-5','burst-20-4','outage-400ms','fanout-3'])
    args = parser.parse_args()
    if not 1500 <= args.duration <= 60000 or not 1 <= args.reps <= 10:
        parser.error('duration must be 1500..60000 ms and reps 1..10')
    args.out.mkdir(parents=True, exist_ok=True)
    cases = [args.scenario] if args.scenario else ['no-loss','iid-5','burst-20-4','outage-400ms','fanout-3']
    results = [run_cell(args.build.resolve(), args.out.resolve(), name, 24301+i, args.duration)
               for name in cases for i in range(args.reps)]
    commit = subprocess.check_output(['git','rev-parse','HEAD'], text=True).strip()
    metadata = dict(schema_version=1, platform=platform.platform(), python=platform.python_version(),
                    source_commit=commit, clock='real monotonic', delay_ms=5, jitter_ms=2,
                    fault_scope='downstream DATA only; control delayed, not intentionally dropped',
                    reproducibility='seeded fault decisions; OS scheduling changes packet order and results',
                    results=results)
    (args.out/'results.json').write_text(json.dumps(metadata, indent=2)+'\n')


if __name__ == '__main__':
    main()
