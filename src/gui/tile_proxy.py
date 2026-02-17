"""
Local tile proxy — feeds Google Hybrid tiles to Qt's OSM plugin.

Qt's OSM plugin sends requests without browser headers, so Google
blocks them. This proxy runs on localhost, receives standard z/x/y
tile requests from Qt, and forwards them to Google with proper
Referer / User-Agent headers.  Includes disk cache for instant loads.
"""
import http.server
import logging
import os
import socket
import threading
import urllib.parse
import urllib.request

logger = logging.getLogger(__name__)

_GOOGLE_URL = "https://mt{s}.google.com/vt/lyrs=y&z={z}&x={x}&y={y}&scale={scale}"
_TILE_CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'cache', 'tiles')

_HEADERS = {
    'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
                  '(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
    'Referer': 'https://www.google.com/',
    'Accept': 'image/webp,image/apng,image/*,*/*;q=0.8',
}


class _TileHandler(http.server.BaseHTTPRequestHandler):
    """Handle /{z}/{x}/{y}[.ext] → Google Hybrid tile."""

    def do_GET(self):
        try:
            self._serve_tile()
        except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError, OSError):
            pass  # Qt closed connection — normal during rapid panning

    def _serve_tile(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.strip('/')
        query = urllib.parse.parse_qs(parsed.query)
        # Strip file extension if Qt appends one (.png, .jpg, etc.)
        last = path.split('/')[-1] if '/' in path else path
        if '.' in last:
            path = path.rsplit('.', 1)[0]

        parts = path.split('/')
        if len(parts) != 3:
            self.send_error(404)
            return

        z, x, y = parts
        scale = 1

        # Qt high-DPI tile mode may request .../y@2x(.png) style paths.
        if y.endswith('@2x'):
            y = y[:-3]
            scale = 2

        # Optional explicit scale query (if client includes it).
        q_scale = query.get('scale')
        if q_scale:
            try:
                if int(q_scale[0]) >= 2:
                    scale = 2
            except (ValueError, TypeError):
                pass

        # ── Disk cache ──
        cache_path = os.path.join(_TILE_CACHE_DIR, z, f"{x}_{y}_s{scale}.tile")
        if os.path.exists(cache_path):
            try:
                with open(cache_path, 'rb') as f:
                    tile = f.read()
                self.send_response(200)
                self.send_header('Content-Type', 'image/png')
                self.send_header('Content-Length', str(len(tile)))
                self.send_header('Cache-Control', 'max-age=604800')
                self.end_headers()
                self.wfile.write(tile)
                return
            except Exception:
                pass  # cache read failed, fall through to fetch

        # ── Fetch from Google ──
        server = int(x) % 4
        url = _GOOGLE_URL.format(s=server, z=z, x=x, y=y, scale=scale)

        req = urllib.request.Request(url, headers=_HEADERS)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                tile = resp.read()
                ctype = resp.headers.get('Content-Type', 'image/png')
        except Exception as e:
            logger.debug("[TileProxy] fetch failed z=%s x=%s y=%s: %s", z, x, y, e)
            self.send_error(502)
            return

        # ── Save to disk cache ──
        try:
            os.makedirs(os.path.dirname(cache_path), exist_ok=True)
            with open(cache_path, 'wb') as f:
                f.write(tile)
        except Exception:
            pass

        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(tile)))
        self.send_header('Cache-Control', 'max-age=604800')
        self.end_headers()
        self.wfile.write(tile)

    def log_message(self, fmt, *args):
        pass  # suppress per-request HTTP logs


def start_tile_proxy() -> int:
    """Start tile proxy on a free port. Returns port number."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]

    server = http.server.ThreadingHTTPServer(('127.0.0.1', port), _TileHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    logger.info("[TileProxy] Google Hybrid proxy on http://127.0.0.1:%d", port)
    return port
