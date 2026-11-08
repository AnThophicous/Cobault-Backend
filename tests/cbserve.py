#!/usr/bin/env python3
"""Servidor HTTP de teste para o Cobault.

Serve um diretório com suporte a Range (206 + Content-Range) e permite
simular os problemas que o downloader precisa tratar:

  --kbps N        limita a velocidade por conexão (como o gargalo por conexão
                  de CDNs de vídeo)
  --fail-every N  responde 503 a cada N requisições (testa retentativas)
  --no-range      ignora Range e devolve 200 com o arquivo inteiro

Uso: python3 -I tests/cbserve.py DIRETORIO PORTA [opções]
"""
import argparse
import os
import sys
import threading
import time
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

CHUNK = 64 * 1024


class Handler(SimpleHTTPRequestHandler):
    server_version = "cbserve/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # sem log de acesso
        pass

    def _path(self):
        return self.translate_path(self.path)

    def do_GET(self):
        opts = self.server.opts
        with self.server.lock:
            self.server.count += 1
            n = self.server.count
        if opts.fail_every and n % opts.fail_every == 0:
            body = b"temporariamente indisponivel"
            self.send_response(503)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
            return

        path = self._path()
        if not os.path.isfile(path):
            self.send_error(404)
            return
        size = os.path.getsize(path)
        rng = self.headers.get("Range")
        start, end, status = 0, size - 1, 200
        if rng and rng.startswith("bytes=") and not opts.no_range:
            spec = rng[6:].split("-", 1)
            start = int(spec[0]) if spec[0] else 0
            end = int(spec[1]) if len(spec) > 1 and spec[1] else size - 1
            end = min(end, size - 1)
            status = 206
        length = end - start + 1

        self.send_response(status)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()

        with open(path, "rb") as f:
            f.seek(start)
            remaining = length
            t0 = time.monotonic()
            sent = 0
            while remaining > 0:
                data = f.read(min(CHUNK, remaining))
                if not data:
                    break
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    return
                remaining -= len(data)
                sent += len(data)
                if opts.kbps:
                    expected = sent / (opts.kbps * 1024)
                    elapsed = time.monotonic() - t0
                    if expected > elapsed:
                        time.sleep(expected - elapsed)

    def do_HEAD(self):
        self.do_GET()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("port", type=int)
    ap.add_argument("--kbps", type=int, default=0)
    ap.add_argument("--fail-every", type=int, default=0)
    ap.add_argument("--no-range", action="store_true")
    opts = ap.parse_args()

    os.chdir(opts.root)
    httpd = ThreadingHTTPServer(("127.0.0.1", opts.port), Handler)
    httpd.daemon_threads = True
    httpd.opts = opts
    httpd.lock = threading.Lock()
    httpd.count = 0
    print(f"pronto em 127.0.0.1:{opts.port}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
