"""Persistent data: voters, tally, settings and the SMS log (one JSON file, written atomically).

Ballot secrecy: only per-candidate totals and a voted flag per voter are stored.
Nothing records who voted for whom.
"""
import copy
import json
import os
import shutil
import time

NUM_CAND = 4

DEFAULT_SETTINGS = {
    "cand": ["Candidate A", "Candidate B", "Candidate C", "Candidate D"],
    "admin": "",              # admin phone, 12 digits with country code (or empty)
    "sms": 1, "smsV": 1, "smsA": 1, "smsR": 1, "seeC": 0,
    "vt": 30, "mc": 40, "sec": 2,
    "open": 0,
}


class Store:
    def __init__(self, data_dir):
        self.dir = data_dir
        os.makedirs(self.dir, exist_ok=True)
        self.path = os.path.join(self.dir, "voting_data.json")
        self.voters = {}                       # id -> {name, age, phone, address, voted, voted_at}
        self.votes = [0] * NUM_CAND
        self.settings = copy.deepcopy(DEFAULT_SETTINGS)
        self.sms = []                          # SMS jobs (newest last), trimmed to SMS_KEEP
        self.sms_seq = 0
        self.history = []                      # [epoch, total_votes] samples for the turnout chart
        self.warning = None                    # set when a corrupt file had to be set aside
        self.fresh = not os.path.exists(self.path)
        self._load()

    SMS_KEEP = 150
    HISTORY_KEEP = 2000

    def _load(self):
        if not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                d = json.load(f)
            self.voters = {int(k): v for k, v in d.get("voters", {}).items()}
            self.votes = [int(x) for x in d.get("votes", self.votes)][:NUM_CAND]
            self.votes += [0] * (NUM_CAND - len(self.votes))
            self.settings.update(d.get("settings", {}))
            self.sms = d.get("sms", [])
            self.sms_seq = int(d.get("sms_seq", 0))
            self.history = d.get("history", [])
        except (ValueError, OSError, TypeError) as e:
            bad = self.path + ".corrupt-" + time.strftime("%Y%m%d-%H%M%S")
            try:
                shutil.copy2(self.path, bad)
            except OSError:
                pass
            self.warning = "Data file was unreadable (%s). A copy was kept as %s and a fresh database started." % (e, os.path.basename(bad))

    def save(self):
        d = {
            "voters": {str(k): v for k, v in self.voters.items()},
            "votes": self.votes,
            "settings": self.settings,
            "sms": self.sms[-self.SMS_KEEP:],
            "sms_seq": self.sms_seq,
            "history": self.history[-self.HISTORY_KEEP:],
        }
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(d, f, ensure_ascii=False, indent=1)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.path)             # atomic: a power cut leaves the old or the new file, never half

    # ---- helpers ----
    def counts(self):
        reg = len(self.voters)
        voted = sum(1 for v in self.voters.values() if v.get("voted"))
        return reg, voted

    def total_votes(self):
        return sum(self.votes)
