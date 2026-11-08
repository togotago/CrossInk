#!/usr/bin/env python3
"""Drive the sleep-time progress push (AutoProgressSync) against a loopback sync server.

The simulator cannot enter deep sleep (the process would end), so this exercises
the library entry point the enterDeepSleep() hook calls: real GET/PUT over HTTP
and real pending-bit and parked-position files on the simulated SD card.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
FIRST = hashlib.md5(b'first.epub').hexdigest()
SECOND = hashlib.md5(b'second.EPUB').hexdigest()
MODES = ['upload', 'batch', 'batch-unsupported', 'remote-ahead', 'server-error', 'no-network',
         'parked-ahead-no-network']


def describe(requests):
    """A compact log: method, path, and the JSON keys sent, never the auth key."""
    lines = []
    for method, path, body in requests:
        keys = sorted(body) if isinstance(body, dict) else []
        lines.append(f'  {method} {path} keys={keys}')
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program', type=Path, default=ROOT / '.pio/build/simulator/program')
    parser.add_argument('--mode', action='append', help='Run only the named scenarios (repeatable)')
    args = parser.parse_args()
    modes = MODES
    if args.mode:
        unknown = set(args.mode) - set(MODES)
        if unknown:
            parser.error(f"Unknown scenarios: {', '.join(sorted(unknown))}")
        modes = args.mode

    requests = []
    state = {'mode': modes[0]}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, status, body):
            data = json.dumps(body).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            requests.append(('GET', self.path, None))
            assert self.headers['x-auth-user'] == 'smoke'
            if self.path.startswith('/syncs/progress/') and state['mode'] == 'remote-ahead':
                self.reply(200, {
                    'document': FIRST,
                    'progress': '/body/DocFragment[2]/body/p[3]/text().0',
                    'percentage': 0.97,
                    'device': 'Kindle',
                    'device_id': 'kindle-partner',
                    'timestamp': 1800000000,
                })
                return
            self.reply(404, {})

        def do_PUT(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            requests.append(('PUT', self.path, body))
            assert self.headers['x-auth-key'] == hashlib.md5(b'smoke-password').hexdigest()
            if state['mode'] == 'server-error':
                self.reply(500, {})
                return
            if state['mode'] == 'batch-unsupported' and self.path == '/api/v1/progress/batch':
                self.reply(404, {})
                return
            self.reply(200, {'until': 1, 'accepted': 1, 'timestamp': 1})

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for mode in modes:
            state['mode'] = mode
            requests.clear()
            with tempfile.TemporaryDirectory(prefix='crossink-autosync-') as directory:
                fs = Path(directory) / 'fs_'
                (fs / 'read/sub').mkdir(parents=True)
                fixture = ROOT / 'test/epubs/test_reader_rendering_matrix.epub'
                for book in ('read/first.epub', 'read/sub/second.EPUB'):
                    shutil.copy2(fixture, fs / book)
                env = os.environ.copy()
                for key in tuple(env):
                    if key.startswith('CROSSINK_'):
                        del env[key]
                env.update(
                    SDL_VIDEODRIVER='dummy',
                    CROSSINK_SIMULATOR_SMOKE_TEST='1',
                    CROSSINK_AUTOSYNC_TEST_SERVER=f'http://127.0.0.1:{server.server_port}',
                    CROSSINK_AUTOSYNC_TEST_MODE=mode,
                )
                if mode in ('no-network', 'parked-ahead-no-network'):
                    # No join can succeed, so any attempt shows up as a timeout
                    # in the log and as a NoNetwork result.
                    env['CROSSPOINT_SIM_WIFI_CONNECT'] = 'fail'
                result = subprocess.run([str(args.program.resolve())], cwd=directory, env=env,
                                        capture_output=True, text=True, timeout=180)
                log = result.stdout + result.stderr
                if result.returncode or 'Auto progress sync transport smoke passed' not in log:
                    print(log[-12000:])
                    raise RuntimeError(f'{mode}: simulator failed ({result.returncode})')

                gets = [p for m, p, _ in requests if m == 'GET']
                puts = [(p, b) for m, p, b in requests if m == 'PUT']
                sleep_line = next((l for l in log.splitlines() if 'Sleep sync' in l), '')
                assert sleep_line, log[-4000:]
                pending = (fs / '.crosspoint/sync_pending.bin').read_bytes()
                joined = 'WiFi join timed out' in log or 'WiFi joined' in log

                if mode == 'upload':
                    assert len(gets) == 1 and gets[0].startswith(f'/syncs/progress/{FIRST}'), requests
                    assert len(puts) == 1 and puts[0][0] == '/syncs/progress', requests
                    body = puts[0][1]
                    assert set(body) >= {'document', 'percentage', 'progress', 'device', 'device_id'}, body
                    assert body['document'] == FIRST, body
                    assert 0 < body['percentage'] < 1, body
                    # No stats upload confirms the server in this flow, so the
                    # shared device id is sent, as for any unconfirmed server.
                    assert body['device_id'] == 'crossink-device', body
                    assert 'pushed=1' in sleep_line, sleep_line
                    assert b'/read/first.epub' not in pending, 'confirmed upload should clear the bit'
                elif mode == 'batch':
                    assert len(gets) == 2, requests
                    assert len(puts) == 1 and puts[0][0] == '/api/v1/progress/batch', requests
                    docs = {item['document'] for item in puts[0][1]['items']}
                    assert docs == {FIRST, SECOND}, docs
                    assert 'pushed=2' in sleep_line, sleep_line
                    assert b'/read/first.epub' not in pending and b'/read/sub/second.EPUB' not in pending
                elif mode == 'batch-unsupported':
                    # A server without the batch route gets one PUT per book for
                    # the same burst, and every confirmed book clears its bit.
                    assert len(gets) == 2, requests
                    # The batch is tried once, refused, then one PUT per book.
                    assert puts[0][0] == '/api/v1/progress/batch', requests
                    assert [path for path, _ in puts[1:]] == ['/syncs/progress', '/syncs/progress'], requests
                    assert {body['document'] for _, body in puts[1:]} == {FIRST, SECOND}, puts
                    assert 'No batch progress route' in log, log[-4000:]
                    assert 'pushed=2' in sleep_line, sleep_line
                    assert b'/read/first.epub' not in pending and b'/read/sub/second.EPUB' not in pending
                elif mode == 'remote-ahead':
                    # The remote is further ahead: nothing is uploaded and the
                    # parked position is offered on the book's next open.
                    assert len(gets) == 1 and not puts, requests
                    assert 'parked=1' in sleep_line and 'pushed=0' in sleep_line, sleep_line
                    assert (fs / '.crosspoint/sync_parked.bin').exists(), 'remote position was not parked'
                    assert b'/read/first.epub' not in pending, 'a position behind the server must not stay pending'
                elif mode == 'server-error':
                    assert len(gets) == 1 and len(puts) == 1, requests
                    assert 'failed=1' in sleep_line, sleep_line
                    assert b'/read/first.epub' in pending, 'pending bit lost after a 500'
                elif mode == 'no-network':
                    # A GET is needed, the join fails, and the bit stays for the
                    # next sleep or a manual sync.
                    assert not requests, requests
                    assert joined, log[-4000:]
                    assert b'/read/first.epub' in pending, 'pending bit lost without a network'
                    assert 'no network' in sleep_line, sleep_line
                else:  # parked-ahead-no-network
                    # The parked remote settles the book from the card: no join,
                    # no request, and the bit is cleared.
                    assert not requests, requests
                    assert not joined, 'a burst that needs no request must not join Wi-Fi'
                    assert 'parked=1' in sleep_line and 'failed=0' in sleep_line, sleep_line
                    assert 'ran' in sleep_line, sleep_line
                    assert b'/read/first.epub' not in pending, 'a position behind the server must not stay pending'
                print(f'PASS: {mode}: {sleep_line.strip()}')
                for line in describe(requests):
                    print(line)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == '__main__':
    main()
