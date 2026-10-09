"""End-to-end test: real Core + web server + built-in hardware simulator + fake SMS sender.

Run from the server/ folder:   python -m unittest discover -s tests -v
"""
import base64
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from vs import config as config_mod      # noqa: E402
from vs.bridge import SimBridge          # noqa: E402
from vs.core import Core                 # noqa: E402
from vs.store import Store               # noqa: E402
from vs.web import make_server           # noqa: E402


def wait_for(cond, timeout=6.0, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(step)
    return False


class System:
    def __init__(self, data_dir, sender):
        self.cfg = config_mod.load("does-not-exist.json")
        self.cfg.update(http_port=0, bind="127.0.0.1", admin_user="tester", admin_pass="s3cret", api_key="KEY",
                        data_dir=data_dir, booth="Test Booth")
        self.store = Store(data_dir)
        self.holder = {}

        def factory(a, b, c):
            self.holder["sim"] = SimBridge(a, b, c, self.cfg["max_id"])
            return self.holder["sim"]

        self.core = Core(self.cfg, self.store, factory, sms_sender=sender, log_file=False)
        self.httpd = make_server(self.core, self.cfg, self.holder["sim"])
        self.base = "http://127.0.0.1:%d" % self.httpd.server_address[1]
        self.sim = self.holder["sim"]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.core.start()
        self.auth = "Basic " + base64.b64encode(b"tester:s3cret").decode()

    def close(self):
        self.core.stop()
        self.httpd.shutdown()
        self.httpd.server_close()

    def get(self, path, auth=True):
        req = urllib.request.Request(self.base + path, headers={"Authorization": self.auth} if auth else {})
        with urllib.request.urlopen(req, timeout=5) as r:
            body = r.read().decode()
            return json.loads(body) if "json" in r.headers.get("Content-Type", "") else body

    def post(self, path, data=None, xreq=True):
        h = {"Authorization": self.auth, "Content-Type": "application/x-www-form-urlencoded"}
        if xreq:
            h["X-Req"] = "1"
        req = urllib.request.Request(self.base + path, data=urllib.parse.urlencode(data or {}).encode(),
                                     headers=h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=8) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def state(self):
        return self.get("/api/state")

    def register(self, vid, finger, name="Asha Rao", phone="9876543210"):
        code, j = self.post("/api/enroll", {"id": vid, "name": name, "age": 30, "phone": phone, "address": "12 Test Road"})
        assert code == 200, j
        for step, f in (("wait1", finger), ("wait2", finger), ("wait3", finger)):
            assert wait_for(lambda: self.state()["en"]["s"] == step), "stuck before %s: %s" % (step, self.state()["en"])
            self.sim.sim_finger(f)
        assert wait_for(lambda: self.state()["en"]["s"] == "ok"), self.state()["en"]


class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.sent = []

        def sender(key, tpl, phone, v1, v2):
            cls.sent.append((key, tpl, phone, v1, v2))
            return 200, '{"ok":true}'

        cls.sys = System(cls.tmp.name, sender)
        assert wait_for(lambda: cls.sys.state()["sensor"]["ok"] == 1 and not cls.sys.state()["ver"]["run"])

    @classmethod
    def tearDownClass(cls):
        cls.sys.close()
        cls.tmp.cleanup()

    def test_1_security(self):
        s = self.sys
        with self.assertRaises(urllib.error.HTTPError) as cm:
            s.get("/api/state", auth=False)
        self.assertEqual(cm.exception.code, 401)
        code, _ = s.post("/api/election", {"open": 1}, xreq=False)
        self.assertEqual(code, 403)
        with self.assertRaises(urllib.error.HTTPError) as cm:
            s.get("/../config.json")
        self.assertEqual(cm.exception.code, 404)
        self.assertIn("<title>", s.get("/"))

    def test_2_full_vote_flow(self):
        s = self.sys
        code, j = s.post("/api/settings", {"c0": "Alpha", "c1": "Beta", "c2": "Gamma", "c3": "Delta", "vt": 30, "mc": 40,
                                           "sec": 2, "sms": 1, "smsV": 1, "smsA": 1, "smsR": 1, "seeC": 0,
                                           "admin": "9421600826"})
        self.assertEqual(code, 200, j)
        s.register(1, finger=101)
        s.register(2, finger=102, name="Ben Cole", phone="9123456780")
        self.assertEqual(len(s.get("/api/voters")), 2)
        # same finger under a new ID is refused
        code, _ = s.post("/api/enroll", {"id": 3, "name": "Dup Dup", "age": 40, "phone": "9000000000", "address": "Somewhere"})
        self.assertEqual(code, 200)
        self.assertTrue(wait_for(lambda: s.state()["en"]["s"] == "wait1"))
        s.sim.sim_finger(101)
        self.assertTrue(wait_for(lambda: s.state()["en"]["s"] == "fail"))
        self.assertIn("already enrolled", s.state()["en"]["msg"])

        self.assertEqual(s.post("/api/election", {"open": 1})[0], 200)
        self.assertTrue(wait_for(lambda: s.sim.mode == "scan"))
        s.sim.sim_finger(999)                                   # unknown finger
        self.assertTrue(wait_for(lambda: "not recognised" in s.state()["st"]["msg"]))
        s.sim.sim_finger(101)
        self.assertTrue(wait_for(lambda: s.state()["st"]["s"] == "auth" and s.state()["st"]["name"] == "Asha Rao"))
        s.sim.sim_button(2)
        self.assertTrue(wait_for(lambda: s.state()["voted"] == 1))
        self.assertEqual(s.state()["votes"], [0, 1, 0, 0])
        # repeat attempt by the same voter is refused
        self.assertTrue(wait_for(lambda: s.sim.mode == "scan"))
        s.sim.sim_finger(101)
        self.assertTrue(wait_for(lambda: "already voted" in s.state()["st"]["msg"]))
        self.assertEqual(sum(s.state()["votes"]), 1)
        # second voter
        s.sim.sim_finger(102)
        self.assertTrue(wait_for(lambda: s.state()["st"]["s"] == "auth"))
        s.sim.sim_button(4)
        self.assertTrue(wait_for(lambda: s.state()["votes"] == [0, 1, 0, 1]))

        # ballot secrecy: nothing in voter list, log or SMS says who voted for whom
        blob = json.dumps([s.get("/api/voters"), s.get("/api/log"), s.get("/api/sms")])
        self.assertNotIn("Beta", blob)
        self.assertNotIn("Delta", blob)
        # SMS: registration x2, voter x2, admin x2 (queue drains in the background)
        self.assertTrue(wait_for(lambda: len(self.sent) >= 6, 10), self.sent)
        phones = [m[2] for m in self.sent]
        self.assertIn("919876543210", phones)
        self.assertIn("919421600826", phones)
        self.assertTrue(all(len(m[3]) <= 30 and len(m[4]) <= 30 for m in self.sent))
        self.assertTrue(wait_for(lambda: s.state()["sms"]["pend"] == 0))
        self.assertEqual(s.get("/api/results")["votes"], [0, 1, 0, 1])
        self.assertIn('"Alpha",0,0.0', s.get("/api/results.csv"))

    def test_3_timeout_and_close(self):
        s = self.sys
        s.post("/api/votes/reset")
        self.assertEqual(s.state()["voted"], 0)
        self.assertTrue(wait_for(lambda: s.sim.mode == "scan"))
        s.sim.sim_finger(101)
        self.assertTrue(wait_for(lambda: s.state()["st"]["s"] == "auth"))
        s.post("/api/election", {"open": 0})                    # closing cancels the pending voter
        self.assertTrue(wait_for(lambda: s.state()["st"]["s"] == "closed"))
        s.sim.sim_button(1)
        time.sleep(0.4)
        self.assertEqual(sum(s.state()["votes"]), 0)

    def test_4_fingerprint_tools_and_delete(self):
        s = self.sys
        self.assertEqual(s.post("/api/fp/verify")[0], 200)
        self.assertTrue(wait_for(lambda: not s.state()["ver"]["run"]))
        fp = s.get("/api/fp")
        self.assertEqual(fp["slots"][:3], "OO.")
        self.assertEqual(s.post("/api/fp/identify")[0], 200)
        self.assertTrue(wait_for(lambda: s.sim.mode == "scan"))
        s.sim.sim_finger(102)
        self.assertTrue(wait_for(lambda: "Ben Cole" in s.state()["idn"]["msg"]))
        code, j = s.post("/api/voter/delete", {"id": 2})
        self.assertEqual(code, 200, j)
        self.assertEqual(len(s.get("/api/voters")), 1)
        self.assertNotIn(102, s.sim.templates.values())

    def test_5_persistence(self):
        s = self.sys
        s.post("/api/election", {"open": 0})
        data = Store(self.tmp.name)
        self.assertEqual(sorted(data.voters), [1])
        self.assertEqual(data.settings["cand"][0], "Alpha")


if __name__ == "__main__":
    unittest.main()
