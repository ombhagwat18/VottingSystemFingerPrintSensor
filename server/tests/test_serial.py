"""SerialBridge test: a fake serial port is wired to the firmware simulator, so the real
framing, keep-alive PING, auto-detect and reconnect code runs without hardware."""
import os
import queue
import sys
import tempfile
import time
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from vs import bridge as bridge_mod      # noqa: E402
from vs import config as config_mod      # noqa: E402
from vs.core import Core                 # noqa: E402
from vs.store import Store               # noqa: E402


def wait_for(cond, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


class FakeSerialModule:
    """Stands in for the pyserial module; every open() gives a new port object sharing one simulated board."""
    def __init__(self, sim_holder):
        self.holder = sim_holder
        self.current = None
        self.opens = 0
        self.Serial = self._make()

    def _make(self):
        outer = self

        class Serial:
            def __init__(self):
                self.rx = outer.holder["to_pc"]
                self.broken = False
                self.dtr = self.rts = None

            def open(self):
                outer.opens += 1
                outer.current = self

            def write(self, data):
                if self.broken:
                    raise OSError("unplugged")
                for line in data.decode().splitlines():
                    outer.holder["sim"].send(line)

            def read(self, n):
                if self.broken:
                    raise OSError("unplugged")
                try:
                    return self.rx.get(timeout=0.05)
                except queue.Empty:
                    return b""

            def close(self):
                pass
        return Serial


class SerialBridgeTest(unittest.TestCase):
    def test_framing_keepalive_and_reconnect(self):
        holder = {"to_pc": queue.Queue()}
        fake = FakeSerialModule(holder)
        orig = (bridge_mod.serial, bridge_mod.find_port)
        bridge_mod.serial = fake
        bridge_mod.find_port = lambda wanted="auto": "FAKE1"
        sim = bridge_mod.SimBridge(lambda text: holder["to_pc"].put((text + "\r\n").encode()), lambda: None,
                                   lambda m: None, 127)
        holder["sim"] = sim
        sim.start()
        tmp = tempfile.TemporaryDirectory()
        cfg = config_mod.load("none.json")
        cfg.update(data_dir=tmp.name, http_port=0)
        try:
            core = Core(cfg, Store(tmp.name),
                        lambda a, b, c: bridge_mod.SerialBridge("auto", 115200, a, b, c), log_file=False)
            core.start()
            self.assertTrue(wait_for(lambda: core.api_state()["bridge"]["ok"] == 1 and core.sensor_ok), "never connected")
            self.assertEqual(core.api_state()["bridge"]["port"], "FAKE1")
            self.assertEqual(fake.opens, 1)
            # garbage (ROM boot text) between valid lines must be ignored
            holder["to_pc"].put(b"\xff\xfe boot garbage \x00\r\nHB 1\r\n")
            time.sleep(0.2)
            self.assertTrue(core.api_state()["bridge"]["ok"])

            # unplug: reads fail -> bridge reconnects by itself and the system recovers
            fake.current.broken = True
            self.assertTrue(wait_for(lambda: fake.opens >= 2, 10), "did not reconnect")
            self.assertTrue(wait_for(lambda: core.api_state()["bridge"]["ok"] == 1 and core.sensor_ok, 10))
            core.stop()
        finally:
            bridge_mod.serial, bridge_mod.find_port = orig
            sim.stop()
            tmp.cleanup()


if __name__ == "__main__":
    unittest.main()
