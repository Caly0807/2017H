import argparse
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
from ipaddress import IPv4Address
import json
import os
from pathlib import Path
import tempfile
from urllib.request import Request, urlopen

from sweep_report import load_capture, validate, write_csv


ENDPOINT = '/v1/sweeps'
SCHEMA = '2017h.sweep.v1'
MAX_PAYLOAD = 65_536


def serve(bind, port, output, token):
    output.mkdir(parents=True, exist_ok=True)

    class Receiver(BaseHTTPRequestHandler):
        def setup(self):
            self.request.settimeout(5)
            super().setup()

        def respond(self, status, body):
            payload = json.dumps(body).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_POST(self):
            if self.path != ENDPOINT:
                return self.respond(404, {'error': 'unknown endpoint'})
            authorization = self.headers.get('Authorization', '').encode('utf-8')
            if token and not hmac.compare_digest(authorization, f'Bearer {token}'.encode('utf-8')):
                return self.respond(401, {'error': 'token required'})
            if self.headers.get_content_type() != 'application/json':
                return self.respond(415, {'error': 'expected application/json'})
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= MAX_PAYLOAD:
                    raise ValueError('invalid payload length')
                body = json.loads(self.rfile.read(length))
                if body['schema'] != SCHEMA:
                    raise ValueError('unknown schema')
                bundle = validate(body)
            except (OSError, KeyError, TypeError, ValueError, ArithmeticError, RecursionError):
                return self.respond(400, {'error': 'invalid sweep payload'})
            try:
                destination = Path(tempfile.mkdtemp(prefix='received-', dir=output))
                bundle['metadata']['transport'] = 'http'
                metadata = json.dumps(bundle['metadata'], indent=2, allow_nan=False)
                write_csv(destination / 'sweep.csv', bundle['records'])
                (destination / 'sweep.json').write_text(metadata + '\n', encoding='utf-8')
            except (OSError, ValueError):
                return self.respond(500, {'error': 'cannot save capture'})
            self.respond(201, {'capture': destination.name, 'records': 41})

    with HTTPServer((str(bind), port), Receiver) as server:
        print(f'Listening on {server.server_address[0]}:{server.server_address[1]}', flush=True)
        server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description='2017H laptop capture transport; no hardware control')
    subcommands = parser.add_subparsers(dest='command', required=True)
    receiver = subcommands.add_parser('serve')
    receiver.add_argument('--bind', type=IPv4Address, default=IPv4Address('127.0.0.1'))
    receiver.add_argument('--port', type=int, default=8765)
    receiver.add_argument('--output', type=Path, required=True)
    sender = subcommands.add_parser('send')
    sender.add_argument('capture', type=Path)
    sender.add_argument('--host', type=IPv4Address, default=IPv4Address('127.0.0.1'))
    sender.add_argument('--port', type=int, default=8765)
    options = parser.parse_args()
    if not 1 <= options.port <= 65535:
        parser.error('Invalid TCP port')
    token = os.environ.get('SWEEP_LAN_TOKEN', '')
    if options.command == 'serve' and not options.bind.is_loopback and len(token) < 16:
        parser.error('LAN binding requires SWEEP_LAN_TOKEN of at least 16 characters')
    try:
        if options.command == 'serve':
            serve(options.bind, options.port, options.output, token)
            return
        sweep = {'schema': SCHEMA, **load_capture(options.capture)}
        data = json.dumps(sweep, allow_nan=False).encode('utf-8')
        headers = {'Content-Type': 'application/json'}
        if token:
            headers['Authorization'] = f'Bearer {token}'
        with urlopen(Request(f'http://{options.host}:{options.port}{ENDPOINT}', data, headers), timeout=5) as reply:
            print(reply.read().decode('utf-8'))
    except KeyboardInterrupt:
        return
    except (OSError, KeyError, TypeError, ValueError, ArithmeticError) as error:
        parser.exit(1, f'Transport stopped: {error}\n')


if __name__ == '__main__':
    main()
