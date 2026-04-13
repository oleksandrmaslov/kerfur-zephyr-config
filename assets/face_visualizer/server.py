from __future__ import annotations

import argparse
import http.server
import socketserver
import webbrowser
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
VIEWER_PATH = "/assets/face_visualizer/index.html"


class NoCacheRequestHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, directory=None, **kwargs):
        super().__init__(*args, directory=str(REPO_ROOT), **kwargs)

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()


class ReusableTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Serve the Kerfur face visualizer locally.")
    parser.add_argument("--host", default="127.0.0.1", help="Host to bind to.")
    parser.add_argument("--port", default=8765, type=int, help="Port to bind to.")
    parser.add_argument(
        "--no-open",
        action="store_true",
        help="Do not open the browser automatically.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    address = (args.host, args.port)
    viewer_url = f"http://{args.host}:{args.port}{VIEWER_PATH}"

    with ReusableTCPServer(address, NoCacheRequestHandler) as server:
        print(f"Serving {REPO_ROOT}")
        print(f"Visualizer: {viewer_url}")

        if not args.no_open:
            webbrowser.open(viewer_url)

        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print("\nStopping visualizer server.")


if __name__ == "__main__":
    main()
