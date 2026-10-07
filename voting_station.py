#!/usr/bin/env python3
"""
Fingerprint Voting Station  -  laptop side (desktop GUI, no web dashboard)

The NodeMCU (voting_bridge.ino) only reads the fingerprint sensor and the vote
buttons.  Everything else runs here: voter database, vote counting, SMS,
and the GUI.  Data is saved to voting_data.json so nothing is lost on restart.

    pip install pyserial requests
    python voting_station.py            # normal
    python voting_station.py --sim      # no hardware: built-in simulator
"""
import json
import os
import queue
import re
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Missing package.  Run:  pip install pyserial requests")
try:
    import requests
except ImportError:
    requests = None

# ============================== CONFIG ==============================
BAUD = 115200
HERE = os.path.dirname(os.path.abspath(__file__))
DATA_FILE = os.path.join(HERE, "voting_data.json")
LOG_FILE = os.path.join(HERE, "voting_log.txt")

CANDIDATES = ["Candidate A", "Candidate B", "Candidate C", "Candidate D"]   # button 1..4
COLORS = ["#667eea", "#11998e", "#f2994a", "#eb3349"]
VOTE_TIMEOUT_MS = 30000
MIN_CONFIDENCE = 40

SMS_ENABLED = True
API_KEY = os.environ.get("CD_API_KEY", "cd_omb_280926_ax9jt4")
SMS_URL = "https://www.circuitdigest.cloud/api/v1/send_sms?ID={tid}"
TPL_VOTER, TPL_ADMIN = "111", "101"
ADMIN_PHONE = "919421600826"
ADMIN_SMS_INCLUDES_CANDIDATE = True     # set False to keep the ballot secret
SIM_LABEL = "Simulator (no hardware)"
# ====================================================================

BG, INK, MUTED, ACCENT = "#f3f4fb", "#22243d", "#6b6f8d", "#667eea"
HEAD = "#1f2340"
F_BASE, F_BOLD = ("Segoe UI", 10), ("Segoe UI", 10, "bold")
F_SMALL, F_BIG = ("Segoe UI", 9), ("Segoe UI", 26, "bold")


def norm_phone(p):
    d = re.sub(r"\D", "", p)
    d = d.lstrip("0")
    return d if (d.startswith("91") and len(d) == 12) else "91" + d[-10:]


# ============================== DATA ================================
class Store:
    """Voters + tally, saved atomically to JSON after every change."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.RLock()
        self.voters = {}                       # id -> dict(name, age, phone, address, voted)
        self.votes = [0] * len(CANDIDATES)
        self.load_error = None
        self._load()

    def _load(self):
        if not os.path.exists(self.path):
            return
        try:
            with open(self.path, encoding="utf-8") as f:
                d = json.load(f)
            self.voters = {int(k): v for k, v in d.get("voters", {}).items()}
            v = list(d.get("votes", []))
            self.votes = (v + [0] * len(CANDIDATES))[:len(CANDIDATES)]
        except Exception as e:                 # keep the broken file, start clean
            os.replace(self.path, self.path + ".corrupt")
            self.load_error = f"Data file was unreadable ({e}); moved to .corrupt"

    def _save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"voters": self.voters, "votes": self.votes}, f, indent=1)
        os.replace(tmp, self.path)

    def next_free_id(self):
        with self.lock:
            return next((i for i in range(1, 128) if i not in self.voters), None)

    def add_voter(self, vid, name, age, phone, address):
        with self.lock:
            self.voters[vid] = dict(name=name, age=age, phone=phone, address=address, voted=False)
            self._save()

    def delete_voter(self, vid):
        with self.lock:
            self.voters.pop(vid, None)
            self._save()

    def cast(self, vid, idx):
        with self.lock:
            v = self.voters.get(vid)
            if not v or v["voted"]:
                return False
            v["voted"] = True
            self.votes[idx] += 1
            self._save()
            return True

    def reset_votes(self):
        with self.lock:
            self.votes = [0] * len(CANDIDATES)
            for v in self.voters.values():
                v["voted"] = False
            self._save()

    def clear_all(self):
        with self.lock:
            self.voters, self.votes = {}, [0] * len(CANDIDATES)
            self._save()


# ============================== SMS =================================
class SMSWorker(threading.Thread):
    """Sends SMS in the background with retries; never blocks the GUI."""

    def __init__(self, log):
        super().__init__(daemon=True)
        self.q, self.log = queue.Queue(), log

    def send(self, phone, var1, var2, tid):
        self.q.put([phone, var1, var2, tid, 0])

    def run(self):
        while True:
            phone, v1, v2, tid, tries = self.q.get()
            if self._post(phone, v1, v2, tid):
                time.sleep(0.75)
                continue
            tries += 1
            if tries >= 5:
                self.log("err", f"SMS to {phone[-4:].rjust(10, '*')} dropped after 5 tries")
                continue
            t = threading.Timer(5 * tries, self.q.put, [[phone, v1, v2, tid, tries]])
            t.daemon = True
            t.start()

    def _post(self, phone, v1, v2, tid):
        if requests is None:
            self.log("err", "SMS skipped: 'requests' not installed (pip install requests)")
            return True
        try:
            r = requests.post(SMS_URL.format(tid=tid),
                              headers={"Authorization": API_KEY, "Content-Type": "application/json"},
                              json={"mobiles": phone, "var1": v1, "var2": v2}, timeout=15)
            if r.status_code == 200:
                self.log("ok", f"SMS sent to ...{phone[-4:]}")
                return True
            self.log("warn", f"SMS HTTP {r.status_code}: {r.text[:100]}")
        except requests.RequestException as e:
            self.log("warn", f"SMS network error: {type(e).__name__}")
        return False


# ============================== DEVICES =============================
class SerialDevice:
    def __init__(self, port, ev_q):
        self.ev_q, self.last_rx, self._run = ev_q, time.time(), True
        self.simulated = False
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate = port, BAUD
        self.ser.timeout, self.ser.write_timeout = 0.1, 1
        self.ser.dtr = False        # keep NodeMCU from resetting when the port opens
        self.ser.rts = False
        self.ser.open()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while self._run:
            try:
                chunk = self.ser.read(self.ser.in_waiting or 1)
            except Exception as e:
                if self._run:
                    self.ev_q.put(("lost", str(e)))
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("ascii", "ignore").strip()
                if text:
                    self.last_rx = time.time()
                    self.ev_q.put(("line", text))

    def send(self, cmd):
        try:
            self.ser.write((cmd + "\n").encode())
        except Exception as e:
            self.ev_q.put(("lost", str(e)))

    def close(self):
        self._run = False
        try:
            self.ser.close()
        except Exception:
            pass


class SimDevice:
    """Pretends to be the NodeMCU so the GUI can be tried without hardware."""

    def __init__(self, ev_q):
        self.ev_q, self.simulated, self.last_rx, self._run = ev_q, True, time.time(), True
        self.scanning = self.armed = False
        self.count = 0
        threading.Thread(target=self._hb, daemon=True).start()

    def _emit(self, text, delay=0.0):
        def go():
            if self._run:
                self.last_rx = time.time()
                self.ev_q.put(("line", text))
        if delay:
            t = threading.Timer(delay, go)
            t.daemon = True
            t.start()
        else:
            go()

    def _hb(self):
        while self._run:
            self._emit("HB")
            time.sleep(2)

    def send(self, cmd):
        p = cmd.split()
        k = p[0]
        if k == "PING":
            self._emit("PONG")
        elif k == "INFO":
            self._emit(f"INFO sensor=1 capacity=127 count={self.count}")
            self._emit("SENSOR OK 127")
        elif k == "SCAN":
            self.scanning = p[1] == "1"
            self._emit("OK")
        elif k == "ENROLL":
            self._emit("ENR PLACE1")
            self._emit("ENR REMOVE", 2.0)
            self._emit("ENR PLACE2", 3.5)
            self._emit(f"ENR OK {p[1]}", 5.5)
            self.count += 1
        elif k == "ARM":
            self.armed = True
            ms = int(p[1])
            t = threading.Timer(ms / 1000, self._arm_timeout)
            t.daemon = True
            t.start()
            self._emit("OK")
        elif k in ("DISARM", "BEEP", "CANCEL"):
            self.armed = self.armed and k != "DISARM"
            if k == "CANCEL":
                self._emit("ENR FAIL cancelled")
            elif k != "BEEP":
                self._emit("OK")
        elif k == "DEL":
            self._emit(f"DEL OK {p[1]}")
        elif k == "EMPTY":
            self._emit("EMPTY OK")

    def _arm_timeout(self):
        if self.armed:
            self.armed = False
            self._emit("ARM TIMEOUT")

    # helpers used by the GUI simulator bar
    def touch(self, fid):
        if self.scanning:
            self._emit(f"FP MATCH {fid} 120" if fid else "FP NOMATCH")

    def press(self, n):
        if self.armed:
            self.armed = False
            self._emit(f"BTN {n}")

    def close(self):
        self._run = False


# ============================== GUI =================================
PALETTE = {                      # kind -> (background, text)
    "off": ("#e3e6f3", INK), "wait": (ACCENT, "white"), "auth": ("#11998e", "white"),
    "ok": ("#11998e", "white"), "err": ("#eb3349", "white"), "warn": ("#f2994a", "white"),
}
ENR_FAIL = {
    "timeout": "Timed out waiting for the finger.",
    "duplicate": "This fingerprint is already enrolled",
    "sensor": "Fingerprint sensor not ready.",
    "bad_id": "Sensor ID must be 1-127.",
    "model": "Could not build the fingerprint model. Try again.",
    "store": "Could not store the fingerprint on the sensor.",
    "too_many_retries": "Fingerprints never matched (3 tries). Start again.",
    "cancelled": "Enrollment cancelled.",
}


class App(tk.Tk):
    def __init__(self, sim=False):
        super().__init__()
        self.title("Fingerprint Voting Station")
        self.geometry("1060x740")
        self.minsize(920, 640)
        self.configure(bg=BG)

        self.store = Store(DATA_FILE)
        self.ev_q = queue.Queue()
        self.sms = SMSWorker(lambda lvl, msg: self.ev_q.put(("log", lvl, msg)))
        self.sms.start()

        self.dev, self.link, self.sensor_ok = None, "OFF", False     # link: OFF/CONNECTING/UP
        self.want_conn, self.next_reconnect = False, 0
        self.connect_started = self.last_ping = 0
        self.port_map = {}
        self.vstate, self.cur, self.auth_deadline = "OFF", None, 0   # OFF/WAIT/AUTH
        self.flash_kind = self.flash_text = ""
        self.flash_until = 0
        self.enrolling, self.pending = False, None

        self._style()
        self._header()
        self._tabs()
        self.refresh_ports()
        self.refresh_all()
        self.log("info", "Station started")
        if self.store.load_error:
            self.log("err", self.store.load_error)
        if not SMS_ENABLED:
            self.log("warn", "SMS is disabled in CONFIG")
        if sim:
            self.port_var.set(SIM_LABEL)
            self.after(300, self.toggle_connection)
        self.after(40, self.pump)
        self.after(200, self.tick)
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    # ------------------------------ layout ------------------------------
    def _style(self):
        s = ttk.Style(self)
        s.theme_use("clam")
        s.configure(".", font=F_BASE, background=BG, foreground=INK)
        s.configure("TNotebook", background=BG, borderwidth=0)
        s.configure("TNotebook.Tab", padding=(20, 9), font=F_BOLD, background="#dfe2f3", foreground=MUTED)
        s.map("TNotebook.Tab", background=[("selected", "white")], foreground=[("selected", ACCENT)])
        for name, col, hov in (("Accent", ACCENT, "#5468d4"), ("Ok", "#11998e", "#0d7a70"),
                               ("Danger", "#eb3349", "#c72a3d"), ("Warn", "#f2994a", "#d97f2e")):
            s.configure(f"{name}.TButton", background=col, foreground="white", font=F_BOLD,
                        padding=(16, 9), borderwidth=0, focusthickness=0)
            s.map(f"{name}.TButton", background=[("active", hov), ("disabled", "#c9cce0")])
        s.configure("Treeview", rowheight=30, font=F_BASE, background="white", fieldbackground="white", borderwidth=0)
        s.configure("Treeview.Heading", font=F_BOLD, background="#e8eaf7", padding=8)
        s.map("Treeview", background=[("selected", "#dde2ff")], foreground=[("selected", INK)])
        s.configure("TEntry", padding=7)
        s.configure("Horizontal.TProgressbar", troughcolor="#dfe2f3", background=ACCENT, thickness=10)

    def _header(self):
        h = tk.Frame(self, bg=HEAD)
        h.pack(fill="x")
        tk.Label(h, text="Fingerprint Voting Station", bg=HEAD, fg="white",
                 font=("Segoe UI", 16, "bold")).pack(side="left", padx=20, pady=14)
        right = tk.Frame(h, bg=HEAD)
        right.pack(side="right", padx=16)
        self.pill = tk.Label(right, text="Disconnected", bg="#eb3349", fg="white", font=F_BOLD, padx=12, pady=4)
        self.pill.pack(side="right", padx=(12, 0))
        self.btn_conn = ttk.Button(right, text="Connect", style="Accent.TButton", command=self.toggle_connection)
        self.btn_conn.pack(side="right", padx=6)
        ttk.Button(right, text="Refresh", command=self.refresh_ports).pack(side="right", padx=4)
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(right, textvariable=self.port_var, width=34, state="readonly")
        self.port_cb.pack(side="right", padx=6)

    def _tabs(self):
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=14, pady=14)
        self.tab_dash, self.tab_vote = tk.Frame(nb, bg="white"), tk.Frame(nb, bg="white")
        self.tab_reg, self.tab_vot = tk.Frame(nb, bg="white"), tk.Frame(nb, bg="white")
        self.tab_log = tk.Frame(nb, bg="white")
        for t, name in ((self.tab_dash, "Dashboard"), (self.tab_vote, "Voting"), (self.tab_reg, "Register"),
                        (self.tab_vot, "Voters"), (self.tab_log, "Log")):
            nb.add(t, text=name)
        self._build_dash()
        self._build_vote()
        self._build_reg()
        self._build_voters()
        self._build_log()

    def _build_dash(self):
        t = self.tab_dash
        row = tk.Frame(t, bg="white")
        row.pack(fill="x", padx=24, pady=(24, 8))
        self.stat = {}
        for i, (key, label, col) in enumerate((("votes", "TOTAL VOTES", ACCENT),
                                               ("reg", "REGISTERED", "#11998e"),
                                               ("turn", "TURNOUT", "#f2994a"))):
            c = tk.Frame(row, bg=col)
            c.grid(row=0, column=i, sticky="nsew", padx=8)
            row.columnconfigure(i, weight=1)
            v = tk.StringVar(value="0")
            self.stat[key] = v
            tk.Label(c, textvariable=v, bg=col, fg="white", font=("Segoe UI", 30, "bold")).pack(pady=(14, 0))
            tk.Label(c, text=label, bg=col, fg="white", font=F_SMALL).pack(pady=(0, 14))
        tk.Label(t, text="Live results", bg="white", fg=INK, font=("Segoe UI", 13, "bold")).pack(anchor="w", padx=32, pady=(14, 0))
        self.bars = tk.Canvas(t, bg="white", height=280, highlightthickness=0)
        self.bars.pack(fill="x", padx=24, pady=6)
        self.bars.bind("<Configure>", lambda e: self.draw_bars())
        ttk.Button(t, text="Reset votes", style="Danger.TButton", command=self.reset_votes).pack(anchor="e", padx=32, pady=10)

    def _build_vote(self):
        t = self.tab_vote
        self.panel = tk.Frame(t, bg=PALETTE["off"][0])
        self.panel.pack(fill="x", padx=24, pady=(24, 10), ipady=26)
        self.v_title = tk.Label(self.panel, text="", font=F_BIG, bg=PALETTE["off"][0], fg=INK, wraplength=880)
        self.v_title.pack(pady=(14, 4))
        self.v_sub = tk.Label(self.panel, text="", font=("Segoe UI", 13), bg=PALETTE["off"][0], fg=INK, wraplength=880)
        self.v_sub.pack()
        self.v_bar = ttk.Progressbar(self.panel, maximum=VOTE_TIMEOUT_MS / 1000, length=420)
        self.v_bar.pack(pady=(14, 6))
        legend = tk.Frame(t, bg="white")
        legend.pack(pady=6)
        for i, name in enumerate(CANDIDATES):
            tk.Label(legend, text=f" Button {i + 1}  -  {name} ", bg=COLORS[i], fg="white", font=F_BOLD,
                     padx=6, pady=6).grid(row=0, column=i, padx=6)
        btns = tk.Frame(t, bg="white")
        btns.pack(pady=14)
        self.btn_start = ttk.Button(btns, text="Start voting station", style="Ok.TButton", command=self.start_station)
        self.btn_start.grid(row=0, column=0, padx=8)
        self.btn_stop = ttk.Button(btns, text="Stop", style="Danger.TButton", command=self.stop_station)
        self.btn_stop.grid(row=0, column=1, padx=8)
        self.sim_bar = tk.Frame(t, bg="#fff8e1")
        tk.Label(self.sim_bar, text="Simulator:", bg="#fff8e1", font=F_BOLD).pack(side="left", padx=(12, 6), pady=8)
        self.sim_id = tk.Spinbox(self.sim_bar, from_=1, to=127, width=5)
        self.sim_id.pack(side="left")
        ttk.Button(self.sim_bar, text="Touch finger", command=lambda: self.dev.touch(int(self.sim_id.get()))).pack(side="left", padx=6)
        ttk.Button(self.sim_bar, text="Unknown finger", command=lambda: self.dev.touch(0)).pack(side="left", padx=6)
        for n in range(1, 5):
            ttk.Button(self.sim_bar, text=f"Btn {n}", width=6, command=lambda n=n: self.dev.press(n)).pack(side="left", padx=2)

    def _build_reg(self):
        t = self.tab_reg
        form = tk.Frame(t, bg="white")
        form.pack(anchor="nw", padx=32, pady=28)
        self.e_vars = {}
        for r, (key, label) in enumerate((("name", "Full name"), ("age", "Age"), ("phone", "Mobile (10 digits)"),
                                          ("address", "Address"), ("id", "Sensor ID (1-127)"))):
            tk.Label(form, text=label, bg="white", fg=MUTED, font=F_BOLD).grid(row=r, column=0, sticky="w", pady=8, padx=(0, 20))
            v = tk.StringVar()
            self.e_vars[key] = v
            ttk.Entry(form, textvariable=v, width=42).grid(row=r, column=1, pady=8)
        btns = tk.Frame(form, bg="white")
        btns.grid(row=5, column=1, sticky="w", pady=14)
        self.btn_enroll = ttk.Button(btns, text="Start enrollment", style="Accent.TButton", command=self.start_enroll)
        self.btn_enroll.pack(side="left")
        self.btn_cancel = ttk.Button(btns, text="Cancel", command=lambda: self.dev and self.dev.send("CANCEL"))
        self.btn_cancel.pack(side="left", padx=10)
        self.e_msg = tk.Label(t, text="Fill the form, then press Start enrollment.", bg="white", fg=INK,
                              font=("Segoe UI", 13), wraplength=800, justify="left")
        self.e_msg.pack(anchor="w", padx=32, pady=(6, 8))
        self.e_bar = ttk.Progressbar(t, maximum=100, length=460)
        self.e_bar.pack(anchor="w", padx=32)

    def _build_voters(self):
        t = self.tab_vot
        cols = ("id", "name", "age", "phone", "address", "status")
        self.tree = ttk.Treeview(t, columns=cols, show="headings", selectmode="browse")
        for c, w in zip(cols, (60, 200, 60, 140, 320, 110)):
            self.tree.heading(c, text=c.upper())
            self.tree.column(c, width=w, anchor="w")
        self.tree.tag_configure("voted", foreground="#155724", background="#e6f6ea")
        sb = ttk.Scrollbar(t, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        bar = tk.Frame(t, bg="white")
        bar.pack(side="bottom", fill="x", padx=16, pady=12)
        ttk.Button(bar, text="Delete selected voter", style="Warn.TButton", command=self.delete_voter).pack(side="left")
        ttk.Button(bar, text="Erase ALL voters + sensor", style="Danger.TButton", command=self.erase_all).pack(side="left", padx=10)
        sb.pack(side="right", fill="y", pady=(16, 0))
        self.tree.pack(fill="both", expand=True, padx=(16, 0), pady=(16, 0))

    def _build_log(self):
        self.logbox = tk.Text(self.tab_log, bg="#14162b", fg="#d7daf5", font=("Consolas", 10), state="disabled",
                              relief="flat", padx=12, pady=10, wrap="word")
        self.logbox.pack(fill="both", expand=True, padx=12, pady=12)
        for tag, col in (("ok", "#4be3a1"), ("err", "#ff6b81"), ("warn", "#ffc266"), ("info", "#9fa8ff"), ("dev", "#7a7fa8")):
            self.logbox.tag_configure(tag, foreground=col)

    # ------------------------------ helpers ------------------------------
    def log(self, level, text):
        line = f"{time.strftime('%H:%M:%S')}  {text}\n"
        self.logbox.configure(state="normal")
        self.logbox.insert("end", line, level)
        self.logbox.see("end")
        self.logbox.configure(state="disabled")
        try:
            with open(LOG_FILE, "a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%Y-%m-%d')} {line}")
        except OSError:
            pass

    def send(self, cmd):
        if self.dev:
            self.dev.send(cmd)

    def set_pill(self):
        txt, col = {("OFF", 0): ("Disconnected", "#eb3349"), ("OFF", 1): ("Disconnected", "#eb3349"),
                    ("CONNECTING", 0): ("Connecting...", "#f2994a"), ("CONNECTING", 1): ("Connecting...", "#f2994a"),
                    ("UP", 0): ("Sensor NOT found", "#eb3349"), ("UP", 1): ("Connected  |  Sensor OK", "#11998e")
                    }[(self.link, int(self.sensor_ok))]
        self.pill.configure(text=txt, bg=col)
        self.btn_conn.configure(text="Disconnect" if self.dev else "Connect")
        if self.dev and getattr(self.dev, "simulated", False):
            self.sim_bar.pack(pady=(10, 0))
        else:
            self.sim_bar.pack_forget()

    # ------------------------------ connection ------------------------------
    def refresh_ports(self):
        self.port_map = {}
        for p in list_ports.comports():
            self.port_map[f"{p.device} - {p.description}"] = p.device
        labels = list(self.port_map) + [SIM_LABEL]
        self.port_cb["values"] = labels
        cur = self.port_var.get()
        if cur not in labels:
            pick = next((l for l in labels if re.search(r"CH340|CP210|USB.?Serial|USB-SERIAL", l, re.I)), labels[0])
            self.port_var.set(pick)

    def toggle_connection(self):
        if self.dev:
            self.want_conn = False
            self.drop_link("Disconnected")
            return
        if not self.port_var.get():
            messagebox.showwarning("Port", "Select a COM port first.")
            return
        self.want_conn = True
        self.connect()

    def connect(self, quiet=False):
        label = self.port_var.get()
        try:
            self.dev = SimDevice(self.ev_q) if label == SIM_LABEL else SerialDevice(self.port_map.get(label, label), self.ev_q)
        except Exception as e:
            self.dev = None
            if not quiet:
                self.log("err", f"Cannot open {label}: {e}")
                messagebox.showerror("Connection", f"Cannot open the port.\n\n{e}\n\nClose the Arduino Serial Monitor and try again.")
                self.want_conn = False
            return
        self.link, self.sensor_ok = "CONNECTING", False
        self.connect_started, self.last_ping = time.time(), 0
        self.log("info", f"Opened {label}")
        self.set_pill()
        self.render_vote()

    def drop_link(self, why):
        if self.dev:
            self.dev.close()
        self.dev, self.link, self.sensor_ok = None, "OFF", False
        self.vstate = "OFF"
        if self.enrolling:
            self.enroll_done(False, "Connection lost during enrollment.")
        self.log("warn", why)
        self.set_pill()
        self.render_vote()

    def on_link_up(self):
        self.link = "UP"
        self.log("ok", "NodeMCU responded")
        self.send("INFO")
        self.set_pill()

    # ------------------------------ event pump ------------------------------
    def pump(self):
        try:
            while True:
                ev = self.ev_q.get_nowait()
                if ev[0] == "line":
                    self.on_line(ev[1])
                elif ev[0] == "lost":
                    if self.dev:
                        self.drop_link(f"Serial connection lost ({ev[1]}). Reconnecting...")
                elif ev[0] == "log":
                    self.log(ev[1], ev[2])
        except queue.Empty:
            pass
        self.after(40, self.pump)

    def on_line(self, text):
        p = text.split()
        k = p[0]
        if k == "PONG":
            if self.link == "CONNECTING":
                self.on_link_up()
        elif k == "READY":
            if self.vstate != "OFF" or self.enrolling:
                self.log("warn", "NodeMCU restarted - session stopped")
                self.vstate = "OFF"
                if self.enrolling:
                    self.enroll_done(False, "NodeMCU restarted during enrollment.")
            self.send("INFO")
        elif k == "SENSOR":
            self.sensor_ok = p[1] == "OK"
            self.log("ok" if self.sensor_ok else "err", "Fingerprint sensor " + ("ready" if self.sensor_ok else "NOT found - check wiring"))
            if self.link == "CONNECTING":
                self.on_link_up()
            self.set_pill()
            self.render_vote()
        elif k == "INFO":
            kv = dict(x.split("=") for x in p[1:] if "=" in x)
            self.sensor_ok = kv.get("sensor") == "1"
            self.log("info", f"Sensor stores {kv.get('count', '?')} fingerprints (capacity {kv.get('capacity', '?')})")
            self.set_pill()
        elif k == "FP":
            self.on_fp(p[1:])
        elif k == "BTN":
            self.on_btn(int(p[1]))
        elif k == "ARM" and p[1:] == ["TIMEOUT"]:
            self.on_arm_timeout()
        elif k == "ENR":
            self.on_enr(p[1:])
        elif k in ("DEL", "EMPTY"):
            self.log("dev", text)
        elif k.startswith("#"):
            self.log("dev", text)

    def tick(self):
        now = time.time()
        if self.link == "CONNECTING" and self.dev:
            if now - self.last_ping > 1:
                self.last_ping = now
                self.dev.send("PING")
            if now - self.connect_started > 12:
                self.log("err", "No reply from NodeMCU. Is voting_bridge.ino uploaded and the baud 115200?")
                self.drop_link("Connection attempt failed")
        elif self.link == "UP" and self.dev and now - self.dev.last_rx > 6:
            self.drop_link("NodeMCU stopped responding. Reconnecting...")
        if not self.dev and self.want_conn and now > self.next_reconnect:
            self.next_reconnect = now + 4
            self.connect(quiet=True)
        if self.vstate == "AUTH":
            left = self.auth_deadline - now
            if left < -2:                                  # safety net if the timeout event was lost
                self.on_arm_timeout()
            else:
                self.render_vote()
        elif self.flash_until and now > self.flash_until:
            self.flash_until = 0
            self.render_vote()
        self.after(200, self.tick)

    # ------------------------------ voting ------------------------------
    def start_station(self):
        if self.link != "UP" or not self.sensor_ok:
            messagebox.showwarning("Not ready", "Connect the NodeMCU and make sure the sensor is detected.")
            return
        if self.enrolling:
            messagebox.showinfo("Busy", "Finish or cancel the enrollment first.")
            return
        self.vstate = "WAIT"
        self.send("SCAN 1")
        self.log("info", "Voting station started")
        self.render_vote()

    def stop_station(self):
        if self.vstate != "OFF":
            self.send("SCAN 0")
            self.send("DISARM")
            self.log("info", "Voting station stopped")
        self.vstate, self.cur, self.flash_until = "OFF", None, 0
        self.render_vote()

    def flash(self, kind, text, ms, beep=None):
        self.flash_kind, self.flash_text, self.flash_until = kind, text, time.time() + ms / 1000
        if beep:
            self.send(f"BEEP {beep}")
        self.render_vote()

    def on_fp(self, a):
        if self.vstate != "WAIT" or not a:
            return
        if a[0] == "MATCH":
            fid, conf = int(a[1]), int(a[2])
            v = self.store.voters.get(fid)
            if conf < MIN_CONFIDENCE:
                self.flash("warn", "Low match confidence - please try again", 2500, "ERR")
            elif not v:
                self.log("warn", f"Fingerprint #{fid} matched but has no voter record")
                self.flash("err", f"Fingerprint #{fid} has no voter record - re-enroll", 4000, "ERR")
            elif v["voted"]:
                self.log("warn", f"{v['name']} (ID {fid}) tried to vote again")
                self.flash("err", f"{v['name']} has already voted", 4000, "ERR")
            else:
                self.begin_auth(fid)
        elif a[0] == "NOMATCH":
            self.flash("err", "Fingerprint not recognised", 2500, "ERR")
        else:
            self.log("dev", "FP " + " ".join(a))

    def begin_auth(self, fid):
        self.vstate, self.cur, self.flash_until = "AUTH", fid, 0
        self.auth_deadline = time.time() + VOTE_TIMEOUT_MS / 1000
        self.send("SCAN 0")
        self.send(f"ARM {VOTE_TIMEOUT_MS}")
        self.send("BEEP OK")
        self.log("ok", f"Authenticated {self.store.voters[fid]['name']} (ID {fid})")
        self.render_vote()

    def back_to_wait(self):
        self.vstate, self.cur = "WAIT", None
        self.send("SCAN 1")

    def on_btn(self, n):
        if self.vstate != "AUTH" or not 1 <= n <= len(CANDIDATES):
            return
        fid, idx = self.cur, n - 1
        v = self.store.voters[fid]
        if not self.store.cast(fid, idx):
            return
        self.log("ok", f"Vote recorded for {v['name']} (ID {fid})")
        self.send("BEEP DONE")
        if SMS_ENABLED:
            cand = CANDIDATES[idx] if ADMIN_SMS_INCLUDES_CANDIDATE else "a candidate"
            self.sms.send(norm_phone(v["phone"]), v["name"], "cast their vote", TPL_VOTER)
            self.sms.send(ADMIN_PHONE, f"{v['name']} (ID:{fid})", f"voted for {cand}", TPL_ADMIN)
        self.back_to_wait()
        self.flash("ok", f"Thank you, {v['name']} - your vote is recorded", 3500)
        self.refresh_all()

    def on_arm_timeout(self):
        if self.vstate != "AUTH":
            return
        self.log("warn", "Voting time expired - no vote recorded")
        self.send("DISARM")
        self.back_to_wait()
        self.flash("warn", "Time expired - vote not recorded", 3500)

    def render_vote(self):
        now = time.time()
        sub, bar = "", None
        if self.link != "UP":
            kind, title, sub = "off", "Not connected", "Connect the NodeMCU (top right) to begin."
        elif self.flash_until > now:
            kind, title = self.flash_kind, self.flash_text
        elif self.vstate == "OFF":
            kind, title, sub = "off", "Station stopped", "Press 'Start voting station'."
        elif self.vstate == "WAIT":
            kind, title, sub = "wait", "Place your finger on the sensor", "Waiting for a voter..."
        else:
            v = self.store.voters.get(self.cur, {})
            left = max(0, self.auth_deadline - now)
            kind, title = "auth", f"Welcome, {v.get('name', '')}"
            sub, bar = f"Press the button for your candidate  -  {left:0.0f}s left", left
        bg, fg = PALETTE[kind]
        for w in (self.panel, self.v_title, self.v_sub):
            w.configure(bg=bg)
        self.v_title.configure(text=title, fg=fg)
        self.v_sub.configure(text=sub, fg=fg)
        if bar is None:
            self.v_bar.pack_forget()
        else:
            self.v_bar.pack(pady=(14, 6))
            self.v_bar["value"] = bar
        active = self.vstate != "OFF"
        self.btn_start.configure(state="disabled" if active else "normal")
        self.btn_stop.configure(state="normal" if active else "disabled")

    # ------------------------------ enrollment ------------------------------
    def start_enroll(self):
        v = {k: x.get().strip() for k, x in self.e_vars.items()}
        phone = re.sub(r"\D", "", v["phone"])[-10:]
        err = None
        if self.link != "UP" or not self.sensor_ok:
            err = "Connect the NodeMCU and make sure the sensor is detected."
        elif not v["name"] or not v["address"]:
            err = "Name and address are required."
        elif not v["age"].isdigit() or not 1 <= int(v["age"]) <= 120:
            err = "Enter a valid age."
        elif len(phone) != 10:
            err = "Mobile number must have 10 digits."
        elif not v["id"].isdigit() or not 1 <= int(v["id"]) <= 127:
            err = "Sensor ID must be between 1 and 127."
        elif int(v["id"]) in self.store.voters:
            err = f"ID {v['id']} is already used by {self.store.voters[int(v['id'])]['name']}."
        if err:
            messagebox.showwarning("Register", err)
            return
        if self.vstate != "OFF":
            self.stop_station()
        self.pending = dict(id=int(v["id"]), name=v["name"], age=v["age"], phone=phone, address=v["address"])
        self.enrolling = True
        self.btn_enroll.configure(state="disabled")
        self.e_set("Starting...", 0)
        self.send(f"ENROLL {self.pending['id']}")
        self.log("info", f"Enrolling {v['name']} as ID {v['id']}")

    def e_set(self, msg, step):
        self.e_msg.configure(text=msg, fg=INK)
        self.e_bar["value"] = step * 33.4

    def on_enr(self, a):
        if not self.enrolling or not a:
            return
        k = a[0]
        if k == "PLACE1":
            self.e_set("Step 1 of 3  -  place the finger on the sensor", 0.3)
        elif k == "REMOVE":
            self.e_set("Lift your finger off the sensor", 1)
        elif k == "PLACE2":
            self.e_set("Step 3 of 3  -  place the SAME finger again, same position", 2)
        elif k == "RETRY":
            why = "The two scans did not match" if a[2:3] == ["mismatch"] else "Bad scan"
            self.e_msg.configure(text=f"{why} (attempt {a[1]} of 3). Lift your finger and try again.", fg="#c26a00")
        elif k == "OK":
            p = self.pending
            self.store.add_voter(p["id"], p["name"], p["age"], p["phone"], p["address"])
            self.log("ok", f"Enrolled {p['name']} as ID {p['id']}")
            self.enroll_done(True, f"{p['name']} registered as ID {p['id']}.")
        elif k == "FAIL":
            reason = a[1] if len(a) > 1 else "unknown"
            msg = ENR_FAIL.get(reason, f"Enrollment failed ({reason}).")
            if reason == "duplicate" and len(a) > 2:
                other = int(a[2])
                who = self.store.voters.get(other, {}).get("name")
                msg += f" as ID {other}" + (f" ({who})." if who else " but that ID is not in the database - use 'Erase ALL voters + sensor' to clean the sensor.")
            self.log("err", msg)
            self.enroll_done(False, msg)

    def enroll_done(self, ok, msg):
        self.enrolling, self.pending = False, None
        self.btn_enroll.configure(state="normal")
        self.e_msg.configure(text=("Done: " if ok else "Failed: ") + msg, fg="#11998e" if ok else "#eb3349")
        self.e_bar["value"] = 100 if ok else 0
        if ok:
            for x in self.e_vars.values():
                x.set("")
        self.refresh_all()

    # ------------------------------ voters / admin ------------------------------
    def selected_id(self):
        sel = self.tree.selection()
        return int(self.tree.item(sel[0], "values")[0]) if sel else None

    def delete_voter(self):
        vid = self.selected_id()
        if vid is None:
            messagebox.showinfo("Voters", "Select a voter first.")
            return
        v = self.store.voters[vid]
        if not messagebox.askyesno("Delete voter", f"Delete {v['name']} (ID {vid})?\nTheir fingerprint is removed from the sensor too."):
            return
        if self.link == "UP":
            self.send(f"DEL {vid}")
        else:
            messagebox.showwarning("Sensor offline", "The sensor is not connected, so the fingerprint stays on it until you erase the sensor.")
        self.store.delete_voter(vid)
        self.log("warn", f"Deleted voter {v['name']} (ID {vid})")
        self.refresh_all()

    def erase_all(self):
        if not messagebox.askyesno("Erase everything", "Delete ALL voters, ALL votes and ALL fingerprints on the sensor?\n\nThis cannot be undone."):
            return
        if self.link == "UP":
            self.send("EMPTY")
        else:
            messagebox.showwarning("Sensor offline", "Sensor not connected - fingerprints on the sensor were NOT erased.")
        self.store.clear_all()
        self.log("warn", "All voters and votes erased")
        self.refresh_all()

    def reset_votes(self):
        if messagebox.askyesno("Reset votes", "Clear all votes and mark every voter as 'not voted'?\nVoter registrations are kept."):
            self.store.reset_votes()
            self.log("warn", "Votes reset")
            self.refresh_all()

    # ------------------------------ refresh ------------------------------
    def refresh_all(self):
        s = self.store
        total, reg = sum(s.votes), len(s.voters)
        voted = sum(1 for v in s.voters.values() if v["voted"])
        self.stat["votes"].set(str(total))
        self.stat["reg"].set(str(reg))
        self.stat["turn"].set(f"{voted * 100 / reg:.0f}%" if reg else "0%")
        self.draw_bars()
        self.tree.delete(*self.tree.get_children())
        for vid in sorted(s.voters):
            v = s.voters[vid]
            self.tree.insert("", "end", values=(vid, v["name"], v["age"], "+91 " + v["phone"], v["address"],
                                                "Voted" if v["voted"] else "Pending"), tags=("voted",) if v["voted"] else ())
        nxt = s.next_free_id()
        if nxt and not self.e_vars["id"].get():
            self.e_vars["id"].set(str(nxt))
        self.render_vote()

    def draw_bars(self):
        c = self.bars
        c.delete("all")
        w = max(c.winfo_width(), 400)
        total = sum(self.store.votes)
        for i, name in enumerate(CANDIDATES):
            y, n = 12 + i * 66, self.store.votes[i]
            pct = n * 100 / total if total else 0
            c.create_text(8, y + 18, text=name, anchor="w", font=F_BOLD, fill=INK)
            c.create_text(8, y + 38, text=f"Button {i + 1}", anchor="w", font=F_SMALL, fill=MUTED)
            x0, x1 = 150, w - 110
            c.create_rectangle(x0, y + 8, x1, y + 42, fill="#e6e8f5", outline="")
            if n:
                c.create_rectangle(x0, y + 8, x0 + (x1 - x0) * n / total, y + 42, fill=COLORS[i], outline="")
            c.create_text(w - 8, y + 18, text=str(n), anchor="e", font=("Segoe UI", 16, "bold"), fill=COLORS[i])
            c.create_text(w - 8, y + 38, text=f"{pct:.1f}%", anchor="e", font=F_SMALL, fill=MUTED)

    def on_close(self):
        if self.dev:
            self.dev.send("SCAN 0")
            self.dev.send("DISARM")
            self.dev.close()
        self.destroy()


if __name__ == "__main__":
    App(sim="--sim" in sys.argv).mainloop()
