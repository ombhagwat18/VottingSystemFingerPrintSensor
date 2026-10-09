"""Link to the NodeMCU hardware bridge (USB serial), plus a built-in simulator for testing.

Both classes offer the same small interface used by core.Core:
    start(), stop(), send(line), last_rx (time.monotonic of the last line received), port_label
and call on_line(text) for every line received and on_connect() after every (re)connection.
"""
import queue
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:                         # the simulator works without pyserial
    serial = None
    list_ports = None

# USB-serial chips used on NodeMCU boards: CH340/CH341, CP210x, FTDI
KNOWN_VIDS = {0x1A86, 0x10C4, 0x0403}


def find_port(wanted="auto"):
    if wanted and wanted.lower() != "auto":
        return wanted
    if list_ports is None:
        return None
    ports = list(list_ports.comports())
    for p in ports:
        if p.vid in KNOWN_VIDS:
            return p.device
    for p in ports:
        text = ((p.description or "") + " " + (p.manufacturer or "")).lower()
        if "usb" in text and "bluetooth" not in text:
            return p.device
    return None


class SerialBridge(threading.Thread):
    def __init__(self, port, baud, on_line, on_connect, on_status):
        super().__init__(name="serial", daemon=True)
        self.wanted = port
        self.baud = baud
        self.on_line = on_line
        self.on_connect = on_connect
        self.on_status = on_status           # callable(str) for human-readable link problems
        self.ser = None
        self.port_label = ""
        self.last_rx = 0.0
        self._stop_flag = False
        self._wlock = threading.Lock()

    def stop(self):
        self._stop_flag = True

    def send(self, line):
        with self._wlock:
            ser = self.ser
            if ser is None:
                return False
            try:
                ser.write((line + "\n").encode("ascii", "replace"))
                return True
            except Exception:
                return False

    def run(self):
        last_msg = None
        while not self._stop_flag:
            port = find_port(self.wanted)
            if serial is None:
                self._status("pyserial is not installed (pip install pyserial)", last_msg)
                last_msg = "pyserial"
                time.sleep(5)
                continue
            if not port:
                msg = "No NodeMCU found on USB. Plug it in with a data cable."
                if msg != last_msg:
                    self.on_status(msg)
                    last_msg = msg
                time.sleep(2)
                continue
            try:
                ser = serial.Serial()
                ser.port = port
                ser.baudrate = self.baud
                ser.timeout = 0.2
                ser.write_timeout = 1
                ser.dtr = False                 # do not reset the board when opening the port
                ser.rts = False
                ser.open()
            except Exception as e:
                msg = "Cannot open %s (%s). Close the Arduino Serial Monitor." % (port, e)
                if msg != last_msg:
                    self.on_status(msg)
                    last_msg = msg
                time.sleep(2)
                continue
            last_msg = None
            self.ser = ser
            self.port_label = port
            self.last_rx = time.monotonic()
            self.on_status("Connected on %s" % port)
            self.on_connect()
            self._read_loop(ser)
            self.ser = None
            try:
                ser.close()
            except Exception:
                pass
            if not self._stop_flag:
                self.on_status("Lost connection to %s - retrying" % port)
                last_msg = "lost"
                time.sleep(1)

    def _status(self, msg, last):
        if last != "pyserial":
            self.on_status(msg)

    def _read_loop(self, ser):
        buf = b""
        last_ping = 0.0
        while not self._stop_flag:
            now = time.monotonic()
            if now - last_ping >= 2.0:          # keep-alive: the ESP shows "PC not connected" if this stops
                last_ping = now
                if not self.send("PING"):
                    return
            if now - self.last_rx > 8.0:         # the ESP sends a heartbeat every 2 s
                self.on_status("No data from the NodeMCU on %s - reconnecting" % self.port_label)
                return
            try:
                data = ser.read(256)
            except Exception:
                return
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("ascii", "ignore").strip()
                if text:
                    self.last_rx = time.monotonic()
                    self.on_line(text)
            if len(buf) > 512:
                buf = b""


class SimBridge:
    """Emulates the firmware so the whole system can be tried without hardware.

    'Fingers' are plain numbers. Use sim_finger(n) to touch the sensor with finger n
    (0 = a finger that is not enrolled) and sim_button(k) to press candidate button k.
    """
    STEP = 0.15                              # seconds the simulated "lift your finger" takes

    def __init__(self, on_line, on_connect, on_status, max_id=127):
        self.on_line = on_line
        self.on_connect = on_connect
        self.on_status = on_status
        self.max_id = max_id
        self.port_label = "simulator"
        self.last_rx = 0.0
        self.templates = {}                  # slot -> finger number
        self.sensor_ok = True
        self.sec = 2
        self.mode = "idle"                   # idle | scan | enroll
        self.armed = False
        self.finger_down = False
        self.lcd = ("", "")
        self.beeps = []
        self.enr = None
        self.arm_until = 0.0
        self._q = queue.Queue()
        self._stop_flag = False
        self.thread = threading.Thread(target=self._run, name="sim", daemon=True)

    # ---- bridge interface ----
    def start(self):
        self.thread.start()

    def stop(self):
        self._stop_flag = True

    def send(self, line):
        self._q.put(("cmd", line))
        return True

    # ---- test hooks ----
    def sim_finger(self, n):
        self._q.put(("finger", int(n)))

    def sim_button(self, k):
        self._q.put(("button", int(k)))

    def sim_unplug_sensor(self, ok):
        self._q.put(("sensor", bool(ok)))

    # ---- internals ----
    def _emit(self, text):
        self.last_rx = time.monotonic()
        self.on_line(text)

    def _run(self):
        self.last_rx = time.monotonic()
        self.on_status("Simulator running (no hardware)")
        self.on_connect()
        self._emit("READY")
        self._emit("SENSOR OK 127" if self.sensor_ok else "SENSOR FAIL silent")
        last_hb = time.monotonic()
        while not self._stop_flag:
            try:
                kind, val = self._q.get(timeout=0.1)
            except queue.Empty:
                kind = None
            if kind == "cmd":
                self._cmd(val)
            elif kind == "finger":
                self._finger(val)
            elif kind == "button":
                if self.armed:
                    self.armed = False
                    self._emit("BTN %d" % val)
            elif kind == "sensor":
                self.sensor_ok = val
                self._emit("SENSOR OK 127" if val else "SENSOR LOST no_response")
            now = time.monotonic()
            if self.armed and now > self.arm_until:
                self.armed = False
                self._emit("ARM TIMEOUT")
            if self.enr and self.enr.get("lift_at") and now >= self.enr["lift_at"]:
                self._lifted()
            if now - last_hb >= 2.0:
                last_hb = now
                self._emit("HB %d" % (1 if self.sensor_ok else 0))

    def _cmd(self, c):
        p = c.split()
        if not p:
            return
        op = p[0]
        if op == "PING":
            self._emit("PONG")
        elif op == "INFO":
            self._emit("INFO sensor=%d capacity=%d count=%d sec=%d fw=4" %
                       (1 if self.sensor_ok else 0, 127 if self.sensor_ok else 0, len(self.templates), self.sec))
        elif op == "SEC":
            self.sec = int(p[1])
            self._emit("OK")
        elif op == "SCAN":
            if p[1] == "1" and self.mode != "enroll":
                self.mode = "scan"
                self.finger_down = True
            elif p[1] == "0" and self.mode == "scan":
                self.mode = "idle"
            self._emit("OK")
        elif op == "ENROLL":
            if not self.sensor_ok:
                self._emit("ENR FAIL sensor")
                return
            self.mode = "enroll"
            self.armed = False
            self.enr = {"id": int(p[1]), "rescan": p[2] == "1", "minconf": int(p[3]), "step": 1,
                        "tries": 0, "f1": None, "phase": 0, "after": 1, "lift_at": 0}
            self._emit("ENR PLACE1")
        elif op == "CANCEL":
            if self.mode == "enroll":
                self.mode, self.enr = "idle", None
                self._emit("ENR FAIL cancelled")
        elif op == "DEL":
            i = int(p[1])
            if self.sensor_ok:
                self.templates.pop(i, None)
            self._emit(("DEL OK %d" if self.sensor_ok else "DEL FAIL %d") % i)
        elif op == "EMPTY":
            if self.sensor_ok:
                self.templates.clear()
            self._emit("EMPTY OK" if self.sensor_ok else "EMPTY FAIL")
        elif op == "MAP":
            top = int(p[1]) if len(p) > 1 else self.max_id
            for i in range(1, top + 1):
                self._emit("SLOT %d %d" % (i, 1 if i in self.templates else 0))
            self._emit("MAP DONE %d" % len(self.templates))
        elif op == "ARM":
            self.armed = True
            self.arm_until = time.monotonic() + int(p[1]) / 1000.0
            self._emit("OK")
        elif op == "DISARM":
            self.armed = False
            self.finger_down = True
            self._emit("OK")
        elif op == "BEEP":
            self.beeps.append(p[1])
            self._emit("OK")
        elif op == "LCD":
            a, _, b = c[4:].partition("|")
            self.lcd = (a, b)
        elif op == "RESET":
            self.mode, self.enr, self.armed = "idle", None, False
            self._emit("READY")
            self._emit("SENSOR OK 127" if self.sensor_ok else "SENSOR FAIL silent")
        else:
            self._emit("ERR unknown_cmd")

    def _find(self, f):
        for slot, fin in self.templates.items():
            if fin == f:
                return slot
        return None

    def _finger(self, f):
        if not self.sensor_ok:
            return
        if self.mode == "scan" and not self.armed:
            slot = self._find(f)
            if slot is not None:
                self._emit("FP MATCH %d %d" % (slot, 120))
            else:
                self._emit("FP NOMATCH")
            return
        e = self.enr
        if self.mode != "enroll" or not e or e["lift_at"]:
            return
        if e["step"] == 1:                            # first scan
            slot = self._find(f)
            if slot is not None and not (e["rescan"] and slot == e["id"]):
                self.mode, self.enr = "idle", None
                self._emit("ENR FAIL duplicate %d" % slot)
                return
            e["f1"] = f
            self._to_lift(2, 0)
        elif e["step"] == 2:                          # second scan must be the same finger
            if f != e["f1"]:
                self._retry("mismatch")
                return
            self.templates[e["id"]] = f
            self._emit("SLOT %d 1" % e["id"])
            self._to_lift(3, 1)
        elif e["step"] == 3:                          # verification scan
            if self.templates.get(e["id"]) == f:
                self.mode, self.enr = "idle", None
                self._emit("ENR OK %d %d" % (e["id"], 150))
            else:
                self.templates.pop(e["id"], None)
                self._emit("SLOT %d 0" % e["id"])
                self._retry("weak_template")

    def _retry(self, why):
        e = self.enr
        e["tries"] += 1
        if e["tries"] >= 3:
            self.mode, self.enr = "idle", None
            self._emit("ENR FAIL " + why)
            return
        self._emit("ENR RETRY %d %s" % (e["tries"], why))
        self._to_lift(1, 0)

    def _to_lift(self, after, phase):
        e = self.enr
        e["after"], e["phase"] = after, phase
        e["lift_at"] = time.monotonic() + self.STEP
        self._emit("ENR REMOVE %d" % phase)

    def _lifted(self):
        e = self.enr
        e["lift_at"] = 0
        e["step"] = e["after"]
        self._emit({1: "ENR PLACE1", 2: "ENR PLACE2", 3: "ENR VERIFY"}[e["step"]])
