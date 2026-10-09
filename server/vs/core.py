"""The brain of the system: voting station, enrollment, fingerprint map, settings and SMS triggers.

All shared state is guarded by Core.lock. Events from the NodeMCU arrive on the bridge thread
(on_line), a ticker thread handles timeouts, and the web server calls the api_* methods.
A web handler must never hold the lock while it waits for the hardware (see rpc()).
"""
import collections
import logging
import os
import re
import socket
import threading
import time

from . import sms as smsmod
from .store import NUM_CAND

EV_INFO, EV_OK, EV_WARN, EV_ERR = 0, 1, 2, 3
LINK_TIMEOUT = 7.0               # seconds without a line from the NodeMCU = link down
FAIL_ALERT_COUNT = 5             # unknown fingers in a row before the admin is alerted
ALERT_COOLDOWN = 300             # seconds between alert SMS of the same kind

ENR_FAIL_TEXT = {
    "timeout": "Timed out", "no_finger": "No finger detected", "sensor": "Sensor offline",
    "mismatch": "Fingers did not match", "store": "Sensor could not store the template",
    "weak_template": "Could not get a reliable scan - clean the finger and the sensor",
    "cancelled": "Cancelled", "busy": "Hardware is busy", "bad_id": "Invalid voter ID",
}
ENR_RETRY_TEXT = {
    "mismatch": "No match - start again with the same finger",
    "weak_template": "Verification failed - scan again, press firmly and flat",
}


def local_ip():
    """The address other devices on the network can use to reach this PC."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))            # no packet is sent; this only selects the outgoing interface
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return "127.0.0.1"


def digits10(text):
    d = re.sub(r"\D", "", str(text or ""))
    return d[-10:] if len(d) >= 10 else None


def clean_text(text, limit):
    return "".join(ch for ch in str(text or "") if ch >= " " and ch != "\x7f").strip()[:limit]


def mask_phone(full):
    return full[:2] + "******" + full[-2:] if len(full) >= 6 else full


class Core:
    def __init__(self, cfg, store, bridge_factory, sms_sender=None, log_file=True):
        self.cfg = cfg
        self.store = store
        self.max_id = cfg["max_id"]
        self.lock = threading.RLock()
        self.t0 = time.monotonic()
        self.rev = {"v": 1, "s": 1, "l": 0, "f": 1, "c": 1}
        self.events = collections.deque(maxlen=200)
        self.ev_total = 0
        self.waiters = []
        self.logger = None
        if log_file:
            self.logger = logging.getLogger("votes")
            if not self.logger.handlers:
                h = logging.FileHandler(os.path.join(store.dir, "voting_log.txt"), encoding="utf-8")
                h.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
                self.logger.addHandler(h)
                self.logger.setLevel(logging.INFO)
        self.host = local_ip()
        self.link_msg = "Starting"
        self.link_up = False
        self._link_seen = False
        self.fw = ""
        self._reset_hw_state()
        self.stop_flag = False
        self._last_alert = {}
        self._lcd_sent = None
        self._lcd_sent_at = 0.0
        self._lcd_flash = None
        if store.fresh:                        # first run: take the admin phone from config.json
            d = digits10(cfg.get("admin_phone"))
            if d:
                store.settings["admin"] = "91" + d
        self.sms = smsmod.SmsQueue(self, cfg["api_key"], sms_sender or smsmod.http_send)
        self.bridge = bridge_factory(self.on_line, self.on_connect, self.on_link_status)
        self.ticker = threading.Thread(target=self._tick_loop, name="tick", daemon=True)
        if store.warning:
            self.evlog(EV_ERR, store.warning)
        reg, voted = store.counts()
        self.evlog(EV_INFO, "Loaded %d voters (%d voted), %d votes" % (reg, voted, store.total_votes()))
        if store.total_votes() != voted:
            self.evlog(EV_WARN, "Tally mismatch: %d votes vs %d voted flags" % (store.total_votes(), voted))
        if self.st["open"]:
            self.evlog(EV_INFO, "Election was OPEN before restart - resuming")

    # ------------------------------------------------------------------ plumbing
    @property
    def st(self):
        return self.store.settings

    def start(self):
        self.sms.start()
        self.bridge.start()
        self.ticker.start()

    def stop(self):
        self.stop_flag = True
        self.sms.stop()
        self.bridge.stop()

    def uptime_ms(self):
        return int((time.monotonic() - self.t0) * 1000)

    def evlog(self, typ, msg):
        with self.lock:
            self.ev_total += 1
            self.events.append({"i": self.ev_total, "t": int(time.time()), "ms": self.uptime_ms(), "y": typ, "m": msg})
            self.rev["l"] = self.ev_total
        print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)
        if self.logger:
            self.logger.info(msg)

    def _reset_hw_state(self):
        self.sensor_ok = False
        self.sensor_cap = 0
        self.sensor_cnt = 0
        self.slot_map = {}                    # id -> 1 present, 2 empty
        self.scan_sent = None
        self.ver = {"run": False, "pos": 0}
        self.enr = {"step": "idle", "id": 0, "msg": "", "retries": 0, "seq": getattr(self, "enr", {}).get("seq", 0),
                    "rescan": False, "phase": 0, "pending": None, "done_at": 0.0, "last": 0.0}
        self.auth_id = 0
        self.auth_start = 0.0
        self.st_msg, self.st_type, self.st_until = "", EV_INFO, 0.0
        self.ident_on, self.ident_until, self.ident_msg, self.ident_clear = False, 0.0, "", 0.0
        self.fail_streak = 0
        self.map_done = False

    def link_ok(self):
        return self.bridge.last_rx > 0 and time.monotonic() - self.bridge.last_rx < LINK_TIMEOUT

    def send(self, line):
        return self.bridge.send(line)

    def rpc(self, cmd, ok_prefix, fail_prefix, timeout=4.0):
        """Send a command and wait for its reply. Never call this while holding the lock."""
        w = {"ok": ok_prefix, "fail": fail_prefix, "ev": threading.Event(), "res": None}
        with self.lock:
            self.waiters.append(w)
        if not self.send(cmd):
            with self.lock:
                self.waiters.remove(w)
            return "down"
        w["ev"].wait(timeout)
        with self.lock:
            if w in self.waiters:
                self.waiters.remove(w)
        return w["res"] or "timeout"

    # ------------------------------------------------------------------ link events
    def on_link_status(self, msg):
        with self.lock:
            self.link_msg = msg

    def on_connect(self):
        with self.lock:
            self._link_changed(True, "serial connect")
            self._hello()

    def _hello(self):
        self.send("SEC %d" % self.st["sec"])
        self.send("INFO")

    def _link_changed(self, up, why):
        """Forget everything the old session knew about the hardware."""
        if self.enr["step"] not in ("idle", "ok", "fail"):
            self._enr_finish(False, "Hardware connection was interrupted")
        if self.auth_id:
            self.auth_id = 0
        seq = self.enr["seq"]
        self._reset_hw_state()
        self.enr["seq"] = seq
        self.rev["f"] += 1
        if up and not self.link_up:
            self.evlog(EV_OK, "Hardware connected (%s)" % self.bridge.port_label)
        self.link_up = up

    def on_line(self, text):
        with self.lock:
            p = text.split()
            if not p:
                return
            for w in list(self.waiters):
                if text.startswith(w["ok"]):
                    w["res"] = "ok"
                    w["ev"].set()
                elif text.startswith(w["fail"]):
                    w["res"] = "fail"
                    w["ev"].set()
            if not self.link_up:
                self._link_changed(True, "traffic")
                self._hello()
            op = p[0]
            try:
                getattr(self, "_rx_" + op.lower(), self._rx_ignore)(p, text)
            except (ValueError, IndexError):
                pass                           # a garbled line (e.g. ROM boot text) is simply ignored

    def _rx_ignore(self, p, text):
        pass

    def _rx_ready(self, p, text):
        self.evlog(EV_INFO, "Hardware (re)started")
        self._link_changed(True, "ready")
        self._hello()

    def _rx_sensor(self, p, text):
        if p[1] == "OK":
            was = self.sensor_ok
            self.sensor_ok = True
            self.sensor_cap = int(p[2])
            self.rev["f"] += 1
            if not was:
                self.evlog(EV_OK, "Fingerprint sensor online (capacity %d)" % self.sensor_cap)
                self.send("INFO")
                self._start_map()
        else:
            was = self.sensor_ok
            self.sensor_ok = False
            self.rev["f"] += 1
            self.ver["run"] = False
            self.auth_id = 0
            if p[1] == "LOST":
                self.evlog(EV_ERR, "Sensor lost: %s" % (p[2] if len(p) > 2 else "no response"))
                self.sms_alert("sensor", "fingerprint sensor offline")
                if self.enr["step"] not in ("idle", "ok", "fail"):
                    self._enr_finish(False, "Sensor offline")
            elif was or not self.map_done:
                why = p[2] if len(p) > 2 else ""
                self.evlog(EV_ERR, "Sensor not found (%s) - check power (5V + GND) and the D1/D2 wires" % why)
                self.map_done = True

    def _rx_hb(self, p, text):
        ok = p[1] == "1"
        if ok and not self.sensor_ok:
            self.send("INFO")
        elif not ok and self.sensor_ok:
            self.sensor_ok = False
            self.rev["f"] += 1

    def _rx_info(self, p, text):
        kv = dict(x.split("=", 1) for x in p[1:] if "=" in x)
        self.fw = kv.get("fw", "")
        ok = kv.get("sensor") == "1"
        if ok:
            self.sensor_cap = int(kv.get("capacity", 0))
            self.sensor_cnt = int(kv.get("count", 0))
            if not self.sensor_ok:
                self.sensor_ok = True
                self.evlog(EV_OK, "Fingerprint sensor online (capacity %d)" % self.sensor_cap)
                self._start_map()
        self.rev["f"] += 1

    def _rx_slot(self, p, text):
        i, v = int(p[1]), int(p[2])
        if v == 1:
            self.slot_map[i] = 1
        elif v == 0:
            self.slot_map[i] = 2
        if self.ver["run"]:
            self.ver["pos"] = i
        self.rev["f"] += 1
        self.rev["v"] += 1

    def _rx_map(self, p, text):
        if p[1] == "DONE":
            self.ver["run"] = False
            self.sensor_cnt = int(p[2])
            self.map_done = True
            miss = sum(1 for i, v in self.store.voters.items() if self.slot_map.get(i) == 2)
            orph = sum(1 for i in range(1, self.max_id + 1) if i not in self.store.voters and self.slot_map.get(i) == 1)
            self.evlog(EV_WARN if miss or orph else EV_OK,
                       "Sensor check: %d templates, %d missing, %d orphan" % (self.sensor_cnt, miss, orph))
        else:
            self.ver["run"] = False
        self.rev["f"] += 1

    def _start_map(self):
        if not self.sensor_ok or self.ver["run"]:
            return False
        self.slot_map = {}
        self.ver = {"run": True, "pos": 0}
        self.send("MAP %d" % self.max_id)
        self.rev["f"] += 1
        return True

    # ------------------------------------------------------------------ voting station
    def _set_msg(self, typ, text):
        self.st_msg, self.st_type, self.st_until = text, typ, time.monotonic() + 5
        a, b = text[:16], text[16:32]
        self.lcd_flash(a, b, 3.0)

    def lcd_flash(self, a, b, secs):
        self._lcd_flash = (a, b, time.monotonic() + secs)

    def _rx_fp(self, p, text):
        if p[1] == "MATCH":
            self._finger_match(int(p[2]), int(p[3]))
        elif p[1] == "NOMATCH":
            self._finger_unknown()
        elif p[1] == "ERR":
            self.send("BEEP ERR")
            if not self.ident_on:
                self._set_msg(EV_WARN, "Poor image - lift and try again")

    def _finger_unknown(self):
        self.send("BEEP ERR")
        if self.ident_on:
            self.ident_on = False
            self.ident_msg, self.ident_clear = "Not enrolled: no matching template", time.monotonic() + 15
            return
        if not self.st["open"] or self.auth_id:
            return
        self._set_msg(EV_ERR, "Fingerprint not recognised")
        self.fail_streak += 1
        self.evlog(EV_WARN, "Unrecognised fingerprint (%d in a row)" % self.fail_streak)
        if self.fail_streak >= FAIL_ALERT_COUNT:
            self.fail_streak = 0
            self.sms_alert("unknown", "repeated unknown fingerprints")
            self.evlog(EV_ERR, "Repeated unknown fingerprints - admin alerted")

    def _finger_match(self, vid, conf):
        voter = self.store.voters.get(vid)
        if self.ident_on:
            self.ident_on = False
            if voter:
                self.ident_msg = "Match: %s (ID %d), confidence %d, %s" % (
                    voter["name"], vid, conf, "already voted" if voter.get("voted") else "not voted yet")
            else:
                self.ident_msg = "Template %d matched but has no voter record (orphan)" % vid
            self.ident_clear = time.monotonic() + 15
            self.send("BEEP OK")
            return
        if not self.st["open"] or self.auth_id or self.enr["step"] not in ("idle", "ok", "fail"):
            return
        self.fail_streak = 0
        if conf < self.st["mc"]:
            self.send("BEEP ERR")
            self._set_msg(EV_WARN, "Low confidence (%d) - try again" % conf)
            return
        if not voter:
            self.send("BEEP ERR")
            self._set_msg(EV_ERR, "Fingerprint has no voter record")
            self.evlog(EV_WARN, "Orphan template matched (ID %d)" % vid)
            return
        if voter.get("voted"):
            self.send("BEEP ERR")
            self._set_msg(EV_ERR, "%s has already voted" % voter["name"])
            self.evlog(EV_WARN, "Repeat vote attempt by %s (ID %d)" % (voter["name"], vid))
            return
        self.auth_id, self.auth_start = vid, time.monotonic()
        self.st_msg = ""
        self.send("BEEP OK")
        self.send("ARM %d" % (self.st["vt"] * 1000))
        self.evlog(EV_INFO, "%s (ID %d) authenticated, confidence %d" % (voter["name"], vid, conf))

    def _rx_btn(self, p, text):
        if self.auth_id and self.st["open"]:
            self._cast_vote(int(p[1]) - 1)

    def _rx_arm(self, p, text):
        if p[1] == "TIMEOUT" and self.auth_id:
            self._vote_timeout()

    def _vote_timeout(self):
        v = self.store.voters.get(self.auth_id, {})
        self.evlog(EV_WARN, "Vote timeout for %s" % v.get("name", "?"))
        self.auth_id = 0
        self.send("BEEP ERR")
        self._set_msg(EV_WARN, "Voting time expired")

    def _cast_vote(self, cand):
        if not 0 <= cand < NUM_CAND:
            return
        vid, self.auth_id = self.auth_id, 0
        voter = self.store.voters[vid]
        voter["voted"], voter["voted_at"] = True, int(time.time())
        self.store.votes[cand] += 1
        self.store.history.append([int(time.time()), self.store.total_votes()])   # totals only, never per candidate
        self.store.save()                     # flag and tally in ONE atomic write: no half-counted vote after a power cut
        self.rev["v"] += 1
        self.send("BEEP DONE")
        self._set_msg(EV_OK, "Vote recorded for %s - thank you!" % voter["name"])
        self.lcd_flash("Vote recorded", "Thank you!", 4.0)
        self.evlog(EV_OK, "VOTE cast by %s (ID %d)" % (voter["name"], vid))       # candidate is NOT logged (secret ballot)
        self.sms_on_vote(vid, cand)

    # ------------------------------------------------------------------ enrollment events
    def _enr_active(self):
        return self.enr["step"] in ("wait1", "remove", "wait2", "wait3")

    def _enr_set(self, step, msg):
        self.enr["step"], self.enr["msg"], self.enr["last"] = step, msg, time.monotonic()

    def _enr_finish(self, ok, msg):
        e = self.enr
        e["step"], e["msg"], e["done_at"] = ("ok" if ok else "fail"), msg, time.monotonic()
        self.evlog(EV_OK if ok else EV_ERR, "Enrollment ID %d %s: %s" % (e["id"], "done" if ok else "failed", msg))
        self.rev["f"] += 1
        self.rev["v"] += 1

    def _rx_enr(self, p, text):
        e = self.enr
        if not self._enr_active() and p[1] not in ("FAIL",):
            return
        k = p[1]
        if k == "PLACE1":
            self._enr_set("wait1", "Place finger on the sensor")
        elif k == "REMOVE":
            e["phase"] = int(p[2])
            self._enr_set("remove", "Saved - lift your finger" if e["phase"] else "Lift your finger")
        elif k == "PLACE2":
            self._enr_set("wait2", "Place the SAME finger again")
        elif k == "VERIFY":
            self._enr_set("wait3", "Place the same finger once more to verify")
        elif k == "NOTE":
            e["msg"] = "Poor image - try again"
        elif k == "RETRY":
            e["retries"] = int(p[2])
            e["msg"] = ENR_RETRY_TEXT.get(p[3], "Try again")
        elif k == "OK":
            self._enr_done_ok(int(p[2]), int(p[3]))
        elif k == "FAIL":
            if not self._enr_active():
                return
            why = p[2] if len(p) > 2 else "failed"
            if why == "duplicate":
                msg = "Finger already enrolled (ID %s)" % (p[3] if len(p) > 3 else "?")
            elif why == "model":
                msg = "Model error (code %s)" % (p[3] if len(p) > 3 else "?")
            else:
                msg = ENR_FAIL_TEXT.get(why, why)
            self._enr_finish(False, msg)

    def _enr_done_ok(self, vid, conf):
        e = self.enr
        if e["id"] != vid:
            return
        self.slot_map[vid] = 1
        self.sensor_cnt = len([1 for v in self.slot_map.values() if v == 1])
        if not e["rescan"] and e["pending"]:
            v = dict(e["pending"])
            v.update(voted=False, voted_at=0)
            self.store.voters[vid] = v
            self.store.save()
            self.evlog(EV_OK, "Voter registered: %s (ID %d, verify confidence %d)" % (v["name"], vid, conf))
            if self.st["smsR"]:
                self.sms_on_register(vid)
        else:
            self.evlog(EV_OK, "Finger updated: %s (ID %d, verify confidence %d)" %
                       (self.store.voters.get(vid, {}).get("name", "?"), vid, conf))
        self._enr_finish(True, "Fingerprint updated" if e["rescan"] else "Voter enrolled")

    # ------------------------------------------------------------------ SMS triggers (lock held)
    def full_phone(self, p10):
        return "91" + p10

    def sms_on_register(self, vid):
        v = self.store.voters[vid]
        self.sms.enqueue("VOTER", self.full_phone(v["phone"]), smsmod.TPL_VOTER, "Voter registration",
                         "%s ID %d" % (self.cfg["booth"], vid))

    def sms_on_vote(self, vid, cand):
        v = self.store.voters[vid]
        reg, voted = self.store.counts()
        stamp = time.strftime("%d-%b %H:%M")
        if self.st["smsV"]:
            self.sms.enqueue("VOTER", self.full_phone(v["phone"]), smsmod.TPL_VOTER, "Your vote",
                             "%s %s" % (self.cfg["booth"], stamp))
        if self.st["smsA"] and self.st["admin"]:
            v2 = "VOTED"
            if self.st["seeC"]:
                v2 += " for " + self.st["cand"][cand]
            v2 += " (%d/%d)" % (voted, reg)
            self.sms.enqueue("ADMIN", self.st["admin"], smsmod.TPL_ADMIN, "voter %s #%d" % (v["name"], vid), v2)

    def sms_alert(self, slot, what):
        now = time.monotonic()
        if slot in self._last_alert and now - self._last_alert[slot] < ALERT_COOLDOWN:
            return
        self._last_alert[slot] = now
        if self.st["admin"]:
            self.sms.enqueue("ALERT", self.st["admin"], smsmod.TPL_ERROR, what, self.cfg["booth"])

    # ------------------------------------------------------------------ ticker
    def _tick_loop(self):
        while not self.stop_flag:
            try:
                self.tick()
            except Exception as e:             # keep the system alive whatever happens
                print("tick error:", repr(e), flush=True)
            time.sleep(0.25)

    def tick(self):
        with self.lock:
            now = time.monotonic()
            up = self.link_ok()
            if self.link_up and not up:
                self.link_up = False
                self.evlog(EV_ERR, "Lost contact with the voting hardware (USB)")
                self._link_changed(False, "timeout")
                if self.st["open"]:
                    self.sms_alert("link", "voting hardware offline")
            if not up:
                return
            if self.auth_id and (not self.st["open"] or now - self.auth_start > self.st["vt"] + 3):
                if self.st["open"]:
                    self._vote_timeout()
                else:
                    self.auth_id = 0
                    self.send("DISARM")
            if self.ident_on and now > self.ident_until:
                self.ident_on, self.ident_msg, self.ident_clear = False, "Test timed out", now + 15
            if self.ident_msg and not self.ident_on and self.ident_clear and now > self.ident_clear:
                self.ident_msg, self.ident_clear = "", 0.0
            if self.st_msg and now > self.st_until:
                self.st_msg = ""
            e = self.enr
            if e["step"] in ("ok", "fail") and now - e["done_at"] > 8:
                e["step"] = "idle"
            elif self._enr_active() and now - e["last"] > 150:
                self.send("CANCEL")
                self._enr_finish(False, "Timed out")
            want = bool(self.sensor_ok and e["step"] in ("idle", "ok", "fail") and not self.ver["run"]
                        and not self.auth_id and (self.st["open"] or self.ident_on))
            if want != self.scan_sent:
                self.scan_sent = want
                self.send("SCAN 1" if want else "SCAN 0")
            self._lcd_update(now)

    def _lcd_lines(self, now):
        if self._lcd_flash and now < self._lcd_flash[2]:
            return self._lcd_flash[0], self._lcd_flash[1]
        e = self.enr
        if e["step"] != "idle":
            names = {"wait1": "Place finger", "remove": "Lift finger", "wait2": "Same finger again",
                     "wait3": "Verify: place", "ok": "Enrolled OK!", "fail": "Failed"}
            return "Enroll ID %d" % e["id"], names.get(e["step"], "")
        if not self.sensor_ok:
            return "Sensor offline", "Check wiring"
        if self.ident_on or self.ident_msg:
            return "Finger test", self.ident_msg or "Place a finger"
        if self.auth_id:
            left = max(0, int(self.st["vt"] - (now - self.auth_start)))
            return self.store.voters.get(self.auth_id, {}).get("name", "")[:16], "Press 1-4  %ds" % left
        reg, voted = self.store.counts()
        url = "%s:%d" % (self.host, self.cfg["http_port"])
        if self.st["open"]:
            return "Place finger", url if int(now / 4) % 2 else "Voted %d/%d" % (voted, reg)
        return "Election closed", url

    def _lcd_update(self, now):
        a, b = self._lcd_lines(now)
        text = "LCD %s|%s" % (a.replace("|", " ")[:16], b.replace("|", " ")[:16])
        if text != self._lcd_sent or now - self._lcd_sent_at > 5:
            self._lcd_sent, self._lcd_sent_at = text, now
            self.send(text)

    # ------------------------------------------------------------------ views for the dashboard
    def slot_code(self, i):
        s = self.slot_map.get(i)
        if i in self.store.voters:
            return "O" if s == 1 else "M" if s == 2 else "U"
        return "X" if s == 1 else "."

    def api_state(self):
        with self.lock:
            now = time.monotonic()
            reg, voted = self.store.counts()
            up = self.link_ok()
            e = self.enr
            if self.auth_id:
                stn = "auth"
            else:
                stn = "scan" if self.st["open"] else "closed"
            left = max(0, int(self.st["vt"] - (now - self.auth_start))) if self.auth_id else 0
            mt = {EV_OK: "ok", EV_ERR: "err"}.get(self.st_type, "warn")
            return {
                "up": self.uptime_ms(), "ep": int(time.time()), "maxid": self.max_id,
                "host": self.host, "port": self.cfg["http_port"], "booth": self.cfg["booth"],
                "bridge": {"ok": 1 if up else 0, "port": self.bridge.port_label, "msg": self.link_msg, "fw": self.fw},
                "sensor": {"ok": 1 if (up and self.sensor_ok) else 0, "cap": self.sensor_cap,
                           "cnt": self.sensor_cnt, "sec": self.st["sec"]},
                "open": self.st["open"], "cand": self.st["cand"], "votes": list(self.store.votes),
                "reg": reg, "voted": voted,
                "st": {"s": stn, "id": self.auth_id, "name": self.store.voters.get(self.auth_id, {}).get("name", ""),
                       "left": left, "msg": self.st_msg, "mt": mt},
                "en": {"s": e["step"], "id": e["id"], "msg": e["msg"], "try": e["retries"], "seq": e["seq"],
                       "resc": 1 if e["rescan"] else 0, "ph": e["phase"]},
                "idn": {"on": 1 if self.ident_on else 0, "msg": self.ident_msg},
                "ver": {"run": 1 if self.ver["run"] else 0, "pos": self.ver["pos"], "max": self.max_id},
                "sms": {"pend": self.sms.pending_count(), "sent": self.sms.sent, "fail": self.sms.failed,
                        "last": self.sms.last_resp, "key": 1 if self.cfg["api_key"] else 0},
                "rev": dict(self.rev),
            }

    def api_settings(self):
        with self.lock:
            s = self.st
            return {"cand": s["cand"], "admin": s["admin"], "sms": s["sms"], "smsV": s["smsV"], "smsA": s["smsA"],
                    "smsR": s["smsR"], "seeC": s["seeC"], "vt": s["vt"], "mc": s["mc"], "sec": s["sec"]}

    def api_voters(self):
        with self.lock:
            return [{"id": i, "n": v["name"], "a": v["age"], "p": v["phone"], "ad": v["address"],
                     "v": 1 if v.get("voted") else 0, "vt": v.get("voted_at", 0), "fp": self.slot_code(i)}
                    for i, v in sorted(self.store.voters.items())]

    def api_fp(self):
        with self.lock:
            codes = "".join(self.slot_code(i) for i in range(1, self.max_id + 1))
            return {"slots": codes, "cnt": self.sensor_cnt, "cap": self.sensor_cap, "ok": 1 if self.sensor_ok else 0,
                    "sec": self.st["sec"], "miss": codes.count("M"), "orph": codes.count("X")}

    def api_log(self):
        with self.lock:
            return list(reversed(self.events))

    def api_sms(self):
        with self.lock:
            return [{"i": j["i"], "t": j["t"], "ms": 0, "k": j["k"], "to": mask_phone(j["phone"]), "tp": j["tp"],
                     "v1": j["v1"], "v2": j["v2"], "s": j["s"], "a": j["a"], "h": j["h"]}
                    for j in reversed(self.store.sms)]

    def api_results(self):
        """Totals only. Never says who voted for whom."""
        with self.lock:
            reg, voted = self.store.counts()
            return {"cand": self.st["cand"], "votes": list(self.store.votes), "reg": reg, "voted": voted,
                    "history": self.store.history[-300:], "open": self.st["open"], "ep": int(time.time()),
                    "booth": self.cfg["booth"]}

    # ------------------------------------------------------------------ actions
    def _busy(self):
        return self._enr_active() or bool(self.auth_id)

    def election(self, open_):
        with self.lock:
            if open_ and not (self.link_ok() and self.sensor_ok):
                return 503, False, "Voting hardware or fingerprint sensor is offline"
            self.st["open"] = 1 if open_ else 0
            if not open_ and self.auth_id:
                self.auth_id = 0
                self.send("DISARM")
            self.store.save()
            self.rev["c"] += 1
            self.evlog(EV_INFO, "Election %s" % ("OPENED" if open_ else "CLOSED"))
            self.send("BEEP OK")
            return 200, True, "Election opened" if open_ else "Election closed"

    def settings_post(self, f):
        with self.lock:
            cand = [clean_text(f.get("c%d" % i), 20) for i in range(NUM_CAND)]
            if not all(cand):
                return 400, False, "Candidate names cannot be empty"
            try:
                vt, mc, sec = int(f.get("vt")), int(f.get("mc")), int(f.get("sec"))
            except (TypeError, ValueError):
                return 400, False, "Invalid number in settings"
            if not 10 <= vt <= 120:
                return 400, False, "Vote timeout must be 10-120 s"
            if not 10 <= mc <= 200:
                return 400, False, "Confidence must be 10-200"
            if not 1 <= sec <= 5:
                return 400, False, "Security level must be 1-5"
            admin = ""
            if str(f.get("admin", "")).strip():
                d = digits10(f.get("admin"))
                if not d:
                    return 400, False, "Admin phone must be 10 digits"
                admin = "91" + d
            s = self.st
            s["cand"], s["admin"], s["vt"], s["mc"] = cand, admin, vt, mc
            for k in ("sms", "smsV", "smsA", "smsR", "seeC"):
                s[k] = 1 if str(f.get(k)) == "1" else 0
            if sec != s["sec"]:
                s["sec"] = sec
                self.send("SEC %d" % sec)
            self.store.save()
            self.rev["c"] += 1
            self.evlog(EV_INFO, "Settings updated")
            return 200, True, "Settings saved"

    def enroll(self, f):
        with self.lock:
            if not (self.link_ok() and self.sensor_ok):
                return 503, False, "Fingerprint sensor is offline"
            if self._enr_active():
                return 409, False, "An enrollment is already running"
            if self.auth_id:
                return 409, False, "A voter is voting right now"
            if self.ver["run"]:
                return 409, False, "Sensor check is running - try again in a few seconds"
            try:
                vid = int(f.get("id"))
            except (TypeError, ValueError):
                vid = 0
            rescan = str(f.get("rescan")) == "1"
            if not 1 <= vid <= self.max_id:
                return 400, False, "Voter ID must be 1-%d" % self.max_id
            pending = None
            if rescan:
                if vid not in self.store.voters:
                    return 404, False, "No voter with that ID"
                name = self.store.voters[vid]["name"]
            else:
                if vid in self.store.voters:
                    return 409, False, "ID %d is already used by %s" % (vid, self.store.voters[vid]["name"])
                ok, pending = self._validate_voter(f)
                if not ok:
                    return 400, False, pending
                name = pending["name"]
            self.ident_on = False
            e = self.enr
            e.update(step="wait1", id=vid, msg="Place finger on the sensor", retries=0, rescan=rescan, phase=0,
                     pending=pending, seq=e["seq"] + 1, last=time.monotonic())
            self.scan_sent = False
            self.send("SCAN 0")
            self.send("ENROLL %d %d %d" % (vid, 1 if rescan else 0, self.st["mc"]))
            self.evlog(EV_INFO, "Enrollment started for ID %d (%s)" % (vid, "re-scan" if rescan else name))
            return 200, True, "Enrollment started - follow the instructions on screen"

    def _validate_voter(self, f):
        name, address = clean_text(f.get("name"), 39), clean_text(f.get("address"), 79)
        try:
            age = int(f.get("age"))
        except (TypeError, ValueError):
            age = 0
        phone = digits10(f.get("phone"))
        if len(name) < 2:
            return False, "Name is too short"
        if len(address) < 3:
            return False, "Address is too short"
        if not 18 <= age <= 120:
            return False, "Voter must be 18 or older"
        if not phone:
            return False, "Phone must be 10 digits"
        return True, {"name": name, "age": age, "phone": phone, "address": address}

    def enroll_cancel(self):
        with self.lock:
            if self._enr_active():
                self.send("CANCEL")
                self._enr_finish(False, "Cancelled")
            return 200, True, "Cancelled"

    def voter_update(self, f):
        with self.lock:
            try:
                vid = int(f.get("id"))
            except (TypeError, ValueError):
                vid = 0
            v = self.store.voters.get(vid)
            if not v:
                return 404, False, "No such voter"
            ok, new = self._validate_voter(f)
            if not ok:
                return 400, False, new
            v.update(new)
            self.store.save()
            self.rev["v"] += 1
            self.evlog(EV_INFO, "Voter %d updated" % vid)
            return 200, True, "Voter updated"

    def voter_delete(self, f):
        with self.lock:
            try:
                vid = int(f.get("id"))
            except (TypeError, ValueError):
                vid = 0
            v = self.store.voters.get(vid)
            if not v:
                return 404, False, "No such voter"
            if v.get("voted"):
                return 409, False, "This voter has already voted. Reset the votes first."
            if not (self.link_ok() and self.sensor_ok):
                return 503, False, "Sensor offline - cannot delete the fingerprint"
            if self._busy():
                return 409, False, "Busy - try again in a moment"
            missing = self.slot_map.get(vid) == 2
        res = self.rpc("DEL %d" % vid, "DEL OK", "DEL FAIL")
        with self.lock:
            if res != "ok" and not missing:
                return 500, False, "Sensor refused to delete (%s)" % res
            self.evlog(EV_INFO, "Voter %s (ID %d) deleted" % (v["name"], vid))
            self.store.voters.pop(vid, None)
            self.slot_map[vid] = 2
            self.store.save()
            self.rev["v"] += 1
            self.rev["f"] += 1
            return 200, True, "Voter deleted"

    def votes_reset(self):
        with self.lock:
            self.store.votes = [0] * NUM_CAND
            self.store.history = []
            for v in self.store.voters.values():
                v["voted"], v["voted_at"] = False, 0
            self.auth_id = 0
            self.store.save()
            self.rev["v"] += 1
            self.evlog(EV_WARN, "ALL VOTES RESET by admin")
            return 200, True, "All votes cleared; voters can vote again"

    def factory(self, f):
        if f.get("confirm") != "ERASE":
            return 400, False, "Confirmation missing"
        with self.lock:
            if not (self.link_ok() and self.sensor_ok):
                return 503, False, "Sensor offline - cannot clear templates"
        if self.rpc("EMPTY", "EMPTY OK", "EMPTY FAIL") != "ok":
            return 500, False, "Sensor refused to clear templates"
        with self.lock:
            self.store.voters, self.store.votes, self.store.history = {}, [0] * NUM_CAND, []
            self.st["open"] = 0
            self.auth_id = 0
            self.slot_map = {i: 2 for i in range(1, self.max_id + 1)}
            self.sensor_cnt = 0
            self.store.save()
            for k in ("v", "f", "c"):
                self.rev[k] += 1
            self.evlog(EV_WARN, "ALL DATA ERASED by admin")
            return 200, True, "All voters, votes and templates erased"

    def fp_verify(self):
        with self.lock:
            if not (self.link_ok() and self.sensor_ok):
                return 503, False, "Sensor offline"
            if self._busy():
                return 409, False, "Busy - try again in a moment"
            self._start_map()
            return 200, True, "Verifying sensor templates..."

    def fp_identify(self):
        with self.lock:
            if not (self.link_ok() and self.sensor_ok):
                return 503, False, "Sensor offline"
            if self._busy():
                return 409, False, "Busy - try again in a moment"
            self.ident_on, self.ident_until = True, time.monotonic() + 20
            self.ident_msg, self.ident_clear = "Place a finger on the sensor...", 0.0
            return 200, True, "Test mode on for 20 s"

    def fp_delete(self, f):
        with self.lock:
            try:
                vid = int(f.get("id"))
            except (TypeError, ValueError):
                vid = 0
            if not 1 <= vid <= self.max_id:
                return 400, False, "Bad ID"
            if vid in self.store.voters:
                return 409, False, "Slot belongs to a voter - delete the voter instead"
            if not (self.link_ok() and self.sensor_ok):
                return 503, False, "Sensor offline"
        if self.rpc("DEL %d" % vid, "DEL OK", "DEL FAIL") != "ok":
            return 500, False, "Sensor refused to delete"
        with self.lock:
            self.slot_map[vid] = 2
            self.rev["f"] += 1
            self.evlog(EV_INFO, "Orphan template %d deleted" % vid)
            return 200, True, "Template deleted"

    def fp_clear(self):
        with self.lock:
            if not (self.link_ok() and self.sensor_ok):
                return 503, False, "Sensor offline"
            if self._busy():
                return 409, False, "Busy - try again in a moment"
        if self.rpc("EMPTY", "EMPTY OK", "EMPTY FAIL") != "ok":
            return 500, False, "Sensor refused"
        with self.lock:
            self.slot_map = {i: 2 for i in range(1, self.max_id + 1)}
            self.sensor_cnt = 0
            self.rev["f"] += 1
            self.rev["v"] += 1
            self.evlog(EV_WARN, "All sensor templates cleared")
            return 200, True, "All templates cleared from the sensor"

    def sms_test(self):
        with self.lock:
            if not self.st["admin"]:
                return 400, False, "Set an admin phone in Settings first"
            self.sms.enqueue("TEST", self.st["admin"], smsmod.TPL_ADMIN, "SMS test", "OK " + time.strftime("%H:%M"))
            return 200, True, "Test SMS queued" if self.st["sms"] and self.cfg["api_key"] else "SMS is off or no API key is configured"

    def sms_resend(self, f):
        with self.lock:
            try:
                jid = int(f.get("id"))
            except (TypeError, ValueError):
                jid = 0
            job, err = self.sms.resend(jid)
            if err:
                return (409 if err.startswith("Already") else 404), False, err
            return 200, True, "Message re-queued"

    def sms_clear(self):
        with self.lock:
            self.sms.clear_finished()
            return 200, True, "Finished messages cleared"

    def reboot(self):
        self.send("RESET")
        return 200, True, "Restarting the voting hardware..."

    def export_csv(self):
        def q(s):
            s = str(s)
            if s[:1] in ("=", "+", "-", "@"):
                s = "'" + s                      # neutralise spreadsheet formulas
            return '"' + s.replace('"', '""') + '"'
        with self.lock:
            rows = ["id,name,age,phone,address,voted,voted_at,fingerprint"]
            for i, v in sorted(self.store.voters.items()):
                when = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(v["voted_at"])) if v.get("voted_at") else ""
                rows.append(",".join([str(i), q(v["name"]), str(v["age"]), v["phone"], q(v["address"]),
                                      "yes" if v.get("voted") else "no", when, self.slot_code(i)]))
            return "\r\n".join(rows) + "\r\n"

    def export_results_csv(self):
        with self.lock:
            tot = self.store.total_votes()
            reg, voted = self.store.counts()
            rows = ["candidate,votes,percent"]
            for n, v in zip(self.st["cand"], self.store.votes):
                rows.append('"%s",%d,%.1f' % (n.replace('"', '""'), v, v * 100.0 / tot if tot else 0))
            rows += ["", "registered,%d" % reg, "voted,%d" % voted,
                     "turnout_percent,%.1f" % (voted * 100.0 / reg if reg else 0),
                     "exported," + time.strftime("%Y-%m-%d %H:%M:%S")]
            return "\r\n".join(rows) + "\r\n"
