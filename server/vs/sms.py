"""CircuitDigest SMS queue: a background thread sends, retries and logs every message.

Voting never waits on the network. Queued messages are saved with the database, so a
restart does not lose them.
"""
import json
import re
import threading
import time
import urllib.error
import urllib.request

API_URL = "https://www.circuitdigest.cloud/api/v1/send_sms?ID={tpl}"
MAX_ATTEMPTS = 5
VAR_MAX = 30                    # CircuitDigest limit per variable
FATAL_CODES = (400, 401, 403, 404)       # retrying cannot help
COOLDOWN_S = 0.75               # minimum gap between API calls

TPL_VOTER = "111"   # "The task {var1} has been successfully completed at {var2}."
TPL_ADMIN = "101"   # "Your {var1} is currently at {var2}."
TPL_ERROR = "107"   # "Error {var1} has been detected in {var2}."


def clean_var(s):
    """ASCII only, safe punctuation, 30 characters (the API limit)."""
    out = re.sub(r"[^A-Za-z0-9 .,\-_()#:/@&+]", "", str(s))
    return out[:VAR_MAX].rstrip()


def http_send(api_key, tpl, phone, var1, var2):
    """POST one message. Returns (http_code, response_text); code 0 = network failure."""
    body = json.dumps({"mobiles": phone, "var1": var1, "var2": var2}).encode()
    req = urllib.request.Request(API_URL.format(tpl=tpl), data=body, method="POST",
                                 headers={"Authorization": api_key, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read(400).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read(400).decode("utf-8", "replace")
    except Exception as e:                     # DNS, timeout, TLS ...
        return 0, "%s: %s" % (type(e).__name__, e)


class SmsQueue:
    def __init__(self, core, api_key, sender=http_send):
        self.core = core                       # provides .lock, .store, .evlog(), .rev
        self.api_key = api_key
        self.sender = sender
        self.sent = 0
        self.failed = 0
        self.last_resp = ""
        self._last_send = 0.0
        self._wake = threading.Event()
        self._stop = False
        self.thread = threading.Thread(target=self._run, name="sms", daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self._stop = True
        self._wake.set()

    # ---- called with core.lock held ----
    def enqueue(self, kind, phone, tpl, v1, v2):
        st = self.core.store
        st.sms_seq += 1
        enabled = bool(st.settings.get("sms")) and bool(self.api_key)
        job = {"i": st.sms_seq, "t": int(time.time()), "k": kind, "phone": phone, "tp": tpl,
               "v1": clean_var(v1), "v2": clean_var(v2),
               "s": "queued" if enabled else "skipped", "a": 0, "h": 0, "next": 0}
        st.sms.append(job)
        del st.sms[:-st.SMS_KEEP]
        self.core.rev["s"] += 1
        if not enabled:
            why = "SMS is turned off" if not st.settings.get("sms") else "no API key configured"
            self.core.evlog(0, "SMS not sent (%s): %s message" % (why, kind))
        st.save()
        self._wake.set()
        return job

    def pending_count(self):
        return sum(1 for j in self.core.store.sms if j["s"] in ("queued", "retry"))

    def resend(self, job_id):
        for j in self.core.store.sms:
            if j["i"] == job_id:
                if j["s"] in ("queued", "retry"):
                    return None, "Already queued"
                return self.enqueue(j["k"], j["phone"], j["tp"], j["v1"], j["v2"]), None
        return None, "Message not found"

    def clear_finished(self):
        st = self.core.store
        st.sms = [j for j in st.sms if j["s"] in ("queued", "retry")]
        self.core.rev["s"] += 1
        st.save()

    # ---- worker ----
    def _pick(self):
        now = time.time()
        with self.core.lock:
            due = [j for j in self.core.store.sms if j["s"] in ("queued", "retry") and j["next"] <= now]
            return dict(min(due, key=lambda j: j["i"])) if due else None

    def _run(self):
        while not self._stop:
            job = self._pick()
            if not job:
                self._wake.wait(1.0)
                self._wake.clear()
                continue
            gap = COOLDOWN_S - (time.monotonic() - self._last_send)
            if gap > 0:
                time.sleep(gap)
            code, resp = self.sender(self.api_key, job["tp"], job["phone"], job["v1"], job["v2"])
            self._last_send = time.monotonic()
            self._after(job["i"], code, resp)

    def _after(self, job_id, code, resp):
        with self.core.lock:
            job = next((j for j in self.core.store.sms if j["i"] == job_id), None)
            if job is None:
                return
            job["a"] += 1
            job["h"] = code
            self.last_resp = re.sub(r"\s+", " ", resp or "")[:120]
            if code == 200:
                job["s"] = "sent"
                self.sent += 1
                self.core.evlog(1, "SMS sent (%s, try %d)" % (job["k"], job["a"]))
            elif code in FATAL_CODES or job["a"] >= MAX_ATTEMPTS:
                job["s"] = "failed"
                self.failed += 1
                self.core.evlog(3, "SMS %s failed (HTTP %s)" % (job["k"], code))
            else:
                job["s"] = "retry"
                job["next"] = time.time() + 2.0 * job["a"] * job["a"]
                self.core.evlog(2, "SMS %s try %d failed (HTTP %s), retrying" % (job["k"], job["a"], code))
            self.core.rev["s"] += 1
            self.core.store.save()
