"""Static configuration read from server/config.json (git-ignored, written by setup.bat)."""
import json
import os

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))    # the server/ folder

DEFAULTS = {
    "admin_user": "admin",
    "admin_pass": "change-me",
    "api_key": "",                  # CircuitDigest API key
    "admin_phone": "",              # 10 digits or 91 + 10 digits
    "http_port": 8080,
    "bind": "0.0.0.0",              # 0.0.0.0 = reachable from other devices by this PC's IP
    "serial_port": "auto",          # "auto" or e.g. "COM3"
    "baud": 115200,
    "booth": "Voting Booth 1",
    "max_id": 127,                  # highest voter ID / sensor slot (1..127)
    "data_dir": os.path.join(HERE, "data"),
}


def load(path=None):
    path = path or os.path.join(HERE, "config.json")
    cfg = dict(DEFAULTS)
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8-sig") as f:
            cfg.update(json.load(f))
    cfg["http_port"] = int(cfg["http_port"])
    cfg["max_id"] = max(1, min(127, int(cfg["max_id"])))
    cfg["config_path"] = path
    cfg["config_found"] = os.path.exists(path)
    return cfg
