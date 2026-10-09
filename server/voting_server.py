#!/usr/bin/env python3
"""
Fingerprint Voting System - PC server.

The NodeMCU (firmware/voting_bridge) only reads the fingerprint sensor and the vote buttons.
This program runs everything else: voter database, vote counting, CircuitDigest SMS and the
web dashboard, which any device on the same network can open at  http://<this PC's IP>:8080

    python voting_server.py            normal (finds the NodeMCU on USB by itself)
    python voting_server.py --sim      no hardware: built-in simulator + fake finger buttons
    python voting_server.py --port COM5
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vs import config as config_mod               # noqa: E402
from vs.bridge import SerialBridge, SimBridge     # noqa: E402
from vs.core import Core                          # noqa: E402
from vs.store import Store                        # noqa: E402
from vs.web import make_server                    # noqa: E402


def main():
    ap = argparse.ArgumentParser(description="Fingerprint voting system server")
    ap.add_argument("--sim", action="store_true", help="run without hardware (simulator)")
    ap.add_argument("--port", help="serial port of the NodeMCU, e.g. COM3 (default: auto-detect)")
    ap.add_argument("--http-port", type=int, help="dashboard port (default from config.json, 8080)")
    ap.add_argument("--config", help="path to config.json")
    args = ap.parse_args()

    cfg = config_mod.load(args.config)
    if args.port:
        cfg["serial_port"] = args.port
    if args.http_port:
        cfg["http_port"] = args.http_port
    if args.sim:
        cfg["data_dir"] = os.path.join(cfg["data_dir"], "sim")      # keep simulated data away from real data

    store = Store(cfg["data_dir"])
    holder = {}

    def factory(on_line, on_connect, on_status):
        if args.sim:
            holder["sim"] = SimBridge(on_line, on_connect, on_status, cfg["max_id"])
            return holder["sim"]
        return SerialBridge(cfg["serial_port"], cfg["baud"], on_line, on_connect, on_status)

    core = Core(cfg, store, factory)
    try:
        httpd = make_server(core, cfg, holder.get("sim"))
    except OSError as e:
        sys.exit("Cannot start the dashboard on port %d: %s\nIs another copy already running? "
                 "Use --http-port to pick another port." % (cfg["http_port"], e))
    core.start()

    print("=" * 62)
    print(" Fingerprint Voting System - server running")
    print("   Dashboard on this PC :  http://localhost:%d" % cfg["http_port"])
    print("   From other devices   :  http://%s:%d   (same WiFi / network)" % (core.host, cfg["http_port"]))
    print("   Login                :  %s / (password from server/config.json)" % cfg["admin_user"])
    print("   Hardware             :  %s" % ("SIMULATOR" if args.sim else "NodeMCU on USB (%s)" % cfg["serial_port"]))
    if not cfg["config_found"]:
        print("   NOTE: server/config.json not found - using defaults (login admin / change-me, SMS off).")
        print("         Run setup.bat to create it.")
    elif cfg["admin_pass"] in ("", "change-me", "admin123"):
        print("   WARNING: change the dashboard password in server/config.json")
    print("=" * 62, flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        core.stop()
        httpd.server_close()


if __name__ == "__main__":
    main()
