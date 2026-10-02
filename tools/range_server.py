#!/usr/bin/env python3
"""
Static file server for testing the launcher: like `python3 -m http.server`,
but it honours Range requests (so resuming can be tested), can ignore them
(--no-range), can throttle (--rate, to interrupt a download half way) and
logs each request with its Range header.

  tools/range_server.py --dir repo --port-file port.txt --log requests.log
"""

import argparse
import http.server
import os
import re
import sys
import time


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path = self.translate_path(self.path)
        if self.server.log_file:
            with open(self.server.log_file, "a") as log:
                log.write(f"GET {self.path} range={self.headers.get('Range', '-')}\n")
        if not os.path.isfile(path):
            self.send_error(404)
            return

        size = os.path.getsize(path)
        start, end = 0, size - 1
        status = 200
        match = re.match(r"bytes=(\d+)-(\d*)$", self.headers.get("Range", ""))
        if match and self.server.ranges:
            start = int(match.group(1))
            if match.group(2):
                end = min(int(match.group(2)), size - 1)
            if start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.end_headers()
                return
            status = 206

        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(end - start + 1))
        if self.server.ranges:
            self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()

        with open(path, "rb") as source:
            source.seek(start)
            remaining = end - start + 1
            while remaining > 0:
                chunk = source.read(min(16384, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                remaining -= len(chunk)
                if self.server.rate:
                    time.sleep(len(chunk) / self.server.rate)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", default=".", help="directory to serve")
    parser.add_argument("--port", type=int, default=0, help="port (default: any free one)")
    parser.add_argument("--port-file", help="write the port number here once listening")
    parser.add_argument("--log", help="append one line per request to this file")
    parser.add_argument("--no-range", action="store_true", help="ignore Range requests")
    parser.add_argument("--rate", type=int, default=0, help="limit to this many bytes per second")
    args = parser.parse_args()

    os.chdir(args.dir)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.ranges = not args.no_range
    server.rate = args.rate
    server.log_file = os.path.abspath(args.log) if args.log else None
    port = server.server_address[1]
    if args.port_file:
        with open(args.port_file + ".tmp", "w") as out:
            out.write(f"{port}\n")
        os.replace(args.port_file + ".tmp", args.port_file)
    print(f"serving {os.getcwd()} on http://127.0.0.1:{port}/", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
