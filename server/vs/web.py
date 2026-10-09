"""HTTP server: serves the React dashboard (server/static) and the JSON API.

Every route needs HTTP Basic login. POST routes also need an X-Req header (cheap CSRF defence,
because a cross-site form cannot set custom headers).
"""
import base64
import hmac
import json
import mimetypes
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

STATIC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "static")

MIME = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".json": "application/json",
        ".png": "image/png", ".ico": "image/x-icon"}


def make_server(core, cfg, sim=None):
    user = cfg["admin_user"].encode()
    pwd = cfg["admin_pass"].encode()

    get_routes = {
        "/api/state": core.api_state, "/api/settings": core.api_settings, "/api/voters": core.api_voters,
        "/api/fp": core.api_fp, "/api/log": core.api_log, "/api/sms": core.api_sms, "/api/results": core.api_results,
    }
    post_routes = {          # path -> function(form) returning (http_code, ok, message)
        "/api/election": lambda f: core.election(f.get("open") == "1"),
        "/api/enroll": core.enroll,
        "/api/enroll/cancel": lambda f: core.enroll_cancel(),
        "/api/voter/update": core.voter_update,
        "/api/voter/delete": core.voter_delete,
        "/api/votes/reset": lambda f: core.votes_reset(),
        "/api/factory": core.factory,
        "/api/fp/verify": lambda f: core.fp_verify(),
        "/api/fp/identify": lambda f: core.fp_identify(),
        "/api/fp/delete": core.fp_delete,
        "/api/fp/clear": lambda f: core.fp_clear(),
        "/api/settings": core.settings_post,
        "/api/sms/test": lambda f: core.sms_test(),
        "/api/sms/resend": core.sms_resend,
        "/api/sms/clear": lambda f: core.sms_clear(),
        "/api/reboot": lambda f: core.reboot(),
    }
    if sim is not None:              # only with --sim: touch the fake sensor / press a fake button
        post_routes["/api/sim/finger"] = lambda f: (sim.sim_finger(f.get("f", 0)) or (200, True, "ok"))
        post_routes["/api/sim/button"] = lambda f: (sim.sim_button(f.get("n", 1)) or (200, True, "ok"))

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "VotingServer"

        def log_message(self, fmt, *args):
            if os.environ.get("VS_DEBUG"):             # set VS_DEBUG=1 to print every request
                print("HTTP", fmt % args, flush=True)

        # ---- helpers ----
        def _authorized(self):
            h = self.headers.get("Authorization", "")
            if h.startswith("Basic "):
                try:
                    u, _, p = base64.b64decode(h[6:]).partition(b":")
                    ok_u = hmac.compare_digest(u, user)
                    ok_p = hmac.compare_digest(p, pwd)
                    return ok_u and ok_p
                except Exception:
                    return False
            return False

        def _send(self, code, body, ctype, extra=None):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def _json(self, code, obj):
            self._send(code, json.dumps(obj, ensure_ascii=False), "application/json; charset=utf-8")

        def _reply(self, code, ok, msg):
            self._json(code, {"ok": 1 if ok else 0, "msg": msg})

        def _guard(self, post):
            if not self._authorized():
                self._send(401, "Login required", "text/plain", {"WWW-Authenticate": 'Basic realm="Voting console"'})
                return False
            if post and "X-Req" not in self.headers:
                self._reply(403, False, "Forbidden")
                return False
            return True

        # ---- verbs ----
        def do_GET(self):
            if not self._guard(False):
                return
            path = urlparse(self.path).path
            try:
                if path in get_routes:
                    return self._json(200, get_routes[path]())
                if path == "/api/export.csv":
                    return self._send(200, core.export_csv(), "text/csv; charset=utf-8",
                                      {"Content-Disposition": "attachment; filename=voters.csv"})
                if path == "/api/results.csv":
                    return self._send(200, core.export_results_csv(), "text/csv; charset=utf-8",
                                      {"Content-Disposition": "attachment; filename=results-%s.csv" % time.strftime("%Y%m%d-%H%M")})
                return self._static(path)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_POST(self):
            if not self._guard(True):
                return
            path = urlparse(self.path).path
            fn = post_routes.get(path)
            if fn is None:
                return self._reply(404, False, "Not found")
            try:
                n = min(int(self.headers.get("Content-Length") or 0), 8192)
                form = {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode("utf-8", "replace"),
                                                     keep_blank_values=True).items()}
                code, ok, msg = fn(form)
            except (BrokenPipeError, ConnectionResetError):
                return
            except Exception as e:                  # never kill the server because of a bad request
                code, ok, msg = 500, False, "Server error: %s" % e
            self._reply(code, ok, msg)

        def _static(self, path):
            rel = "index.html" if path in ("/", "") else path.lstrip("/")
            full = os.path.normpath(os.path.join(STATIC, rel))
            if not full.startswith(STATIC + os.sep) or not os.path.isfile(full):
                return self._send(404, "Not found", "text/plain")
            ext = os.path.splitext(full)[1].lower()
            ctype = MIME.get(ext) or mimetypes.guess_type(full)[0] or "application/octet-stream"
            with open(full, "rb") as f:
                self._send(200, f.read(), ctype)

    ThreadingHTTPServer.daemon_threads = True
    ThreadingHTTPServer.allow_reuse_address = True
    return ThreadingHTTPServer((cfg["bind"], cfg["http_port"]), Handler)
