#!/usr/bin/env python3
"""End-to-end enrolment test: real backend binary + a simulated UNO/R307S on a pseudo-terminal.

    python3 tests/e2e_enroll.py ./build/voting_backend

Scenarios: happy path, duplicate finger, orphan slot (auto-skip), corrupted template (CRC), terminal silent
(timeout), cancel, double enrolment of one voter, SCAN/VOTE after enrolment, enrolment closed once ballots exist.
"""
import hashlib, json, os, pty, sqlite3, subprocess, sys, tempfile, threading, time, tty, urllib.request, urllib.error

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/voting_backend"
PORT = 18080
BASE = f"http://127.0.0.1:{PORT}"
TOKEN = "test-token"

def crc16(b):
    crc = 0xFFFF
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

class FakeTerminal(threading.Thread):
    """Speaks the firmware's protocol. `mode` picks how the next ENROLL goes."""
    def __init__(self, fd):
        super().__init__(daemon=True)
        self.fd, self.mode, self.used_slots, self.templates, self.deleted = fd, "ok", set(), {}, []
        self.sent = []
        self.buf = b""; self.running = True

    def send(self, s):
        os.write(self.fd, (s + "\r\n").encode())

    def run(self):
        while self.running:
            try:
                d = os.read(self.fd, 256)
            except OSError:
                time.sleep(0.05); continue
            self.buf += d
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                self.handle(line.decode().strip())

    def handle(self, line):
        self.sent.append(line)
        if line.startswith("ENROLL:"):
            slot = int(line[7:])
            mode = self.mode
            if mode == "silent": return
            if slot in self.used_slots:
                return self.send("EN:ERR:SLOT_USED")
            self.mode = "ok"   # one-shot: consumed as soon as the command is read (no race with the test thread)
            self.send(f"EN:READY:{slot}"); self.send("EN:PLACE:1"); time.sleep(0.15)
            if mode == "duplicate": return self.send("EN:ERR:DUPLICATE")
            if mode == "timeout": return self.send("EN:ERR:TIMEOUT")
            self.send("EN:LIFT"); time.sleep(0.15); self.send("EN:PLACE:2"); time.sleep(0.15)
            self.send("EN:PROC"); self.send("EN:STORED")
            tpl = os.urandom(512)
            self.used_slots.add(slot); self.templates[slot] = tpl
            for i in range(0, 512, 24):
                self.send(f"T:{i // 24}:{tpl[i:i + 24].hex().upper()}")
            crc = crc16(tpl) ^ (0xBEEF if mode == "badcrc" else 0)
            self.send(f"T:END:512:{crc:X}")
        elif line.startswith("DEL:"):
            slot = int(line[4:]); self.deleted.append(slot); self.used_slots.discard(slot); self.send("EN:DELETED")
        elif line.startswith("SCAN:") or line.startswith("VOTE:"):
            pass  # backend replies; nothing to do

def call(method, path, data=None, token=None):
    body = None
    if data is not None:
        body = "&".join(f"{k}={v}" for k, v in data.items()).encode()
    req = urllib.request.Request(BASE + path, data=body, method=method)
    if token: req.add_header("X-Admin-Token", token)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")

def wait_final(timeout=15):
    t0 = time.time(); st = {}
    while time.time() - t0 < timeout:
        _, st = call("GET", "/api/enroll/status")
        if st["done"] or st["failed"]: return st
        time.sleep(0.1)
    raise AssertionError(f"enrolment never finished: {st}")

fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  [{extra}]" if extra and not cond else ""))
    if not cond: fails.append(name)

def main():
    tmp = tempfile.mkdtemp(prefix="bvs_e2e_")
    dbp = os.path.join(tmp, "v.db")
    master, slave = pty.openpty(); tty.setraw(master)
    term = FakeTerminal(master); term.start()
    env = dict(os.environ, VOTING_ADMIN_TOKEN=TOKEN)
    backend = subprocess.Popen([BIN, "--port", str(PORT), "--db", dbp, "--serial", os.ttyname(slave), "--seed-demo"],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(100):
            try:
                _, s = call("GET", "/api/auth/session")
                if s.get("hardware_connected"): break
            except Exception: pass
            time.sleep(0.2)
        else: raise SystemExit("backend never reported the terminal as connected")

        # 1. happy path
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "VTR100001", "name": "Priya%20Patil", "finger": "Right%20Thumb"})
        check("start accepted (202)", c == 202, r)
        st = wait_final()
        check("enrolment done", st["done"], st)
        sha = hashlib.sha256(term.templates[1]).hexdigest()
        check("API sha256 equals sha256 of the bytes the sensor produced", st.get("template_sha256") == sha)
        db = sqlite3.connect(dbp)
        row = db.execute("select v.sensor_id,v.name,v.voter_ref,v.has_voted,b.template,b.template_sha256,b.finger from voters v join biometric_templates b using(sensor_id)").fetchone()
        check("DB row holds the exact 512 template bytes", row and row[4] == term.templates[1], row[:4] if row else None)
        check("DB voter fields", row and row[:4] == (1, "Priya Patil", "VTR100001", 0) and row[6] == "Right Thumb")
        c, r = call("GET", "/api/enroll/voter?voter_ref=VTR100001")
        check("voter lookup says enrolled, no template bytes leaked", r.get("enrolled") and "template" not in r and r["sensor_id"] == 1, r)
        check("audit trigger logged VOTER_ENROLLED", db.execute("select count(*) from audit_log where event_type='VOTER_ENROLLED'").fetchone()[0] == 1)

        # 2. same voter again
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "VTR100001", "name": "Priya%20Patil", "finger": "Right%20Thumb"})
        check("re-enrolling the same voter is refused (409)", c == 409, (c, r))

        # 3. duplicate finger
        term.mode = "duplicate"
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "VTR100002", "name": "Amit%20Jadhav", "finger": "Left%20Index"})
        st = wait_final()
        check("duplicate finger fails with DUPLICATE", st["failed"] and st["error_code"] == "DUPLICATE", st)
        check("no DB rows written for the duplicate", db.execute("select count(*) from voters").fetchone()[0] == 1)

        # 4. orphan template on slot 2 -> backend skips to slot 3
        term.used_slots.add(2)
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "VTR100002", "name": "Amit%20Jadhav", "finger": "Left%20Index"})
        st = wait_final()
        check("orphan slot skipped, enrolled in slot 3", st["done"] and st["sensor_id"] == 3, st)

        # 5. corrupted template
        term.mode = "badcrc"
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "VTR100003", "name": "Sunita%20More", "finger": "Right%20Index"})
        st = wait_final()
        check("bad CRC rejected as DOWNLOAD error", st["failed"] and st["error_code"] == "DOWNLOAD", st)
        time.sleep(0.3)
        check("host told the sensor to delete the unsaved slot", any(l.startswith("DEL:") for l in term.sent))
        check("nothing stored for the corrupted capture", db.execute("select count(*) from voters where voter_ref='VTR100003'").fetchone()[0] == 0)

        # 6. cancel
        term.mode = "silent"
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "VTR100003", "name": "Sunita%20More", "finger": "Right%20Index"})
        time.sleep(0.3)
        c2, _ = call("POST", "/api/enroll/cancel", {})
        _, st = call("GET", "/api/enroll/status")
        check("cancel works and CANCEL reached the terminal", st["failed"] and st["error_code"] == "CANCELLED" and "CANCEL" in term.sent, st)

        # 7. busy / validation
        term.mode = "silent"
        call("POST", "/api/enroll/start", {"voter_ref": "VTR100003", "name": "Sunita%20More", "finger": "Right%20Index"})
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "VTR100004", "name": "X", "finger": "Right%20Index"})
        check("second concurrent enrolment refused (409)", c == 409, (c, r))
        call("POST", "/api/enroll/cancel", {})
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "bad%20ref!", "name": "X", "finger": "Thumb"})
        check("invalid voter_ref rejected (400)", c == 400, (c, r))

        # 8. admin registry needs the token
        c, _ = call("GET", "/api/enroll/registry")
        c2, r2 = call("GET", "/api/enroll/registry", token=TOKEN)
        check("registry: 401 without token, metadata with token", c == 401 and c2 == 200 and len(r2["enrolments"]) == 2 and all("template" not in e for e in r2["enrolments"]), (c, c2))

        # 9. election check uses the enrolled row
        term.send("SCAN:1"); time.sleep(0.4)
        _, s = call("GET", "/api/auth/session")
        check("SCAN of enrolled slot opens a voting session", s["session_active"], s)
        term.send("VOTE:ALICE"); time.sleep(0.5)
        check("vote committed to the ledger", db.execute("select count(*) from votes").fetchone()[0] == 1)
        term.send("SCAN:1"); time.sleep(0.4)
        _, s = call("GET", "/api/auth/session")
        check("second SCAN by the same voter is denied", not s["session_active"], s)
        term.send("SCAN:99"); time.sleep(0.4)
        _, s = call("GET", "/api/auth/session")
        check("SCAN of a never-enrolled slot is denied", not s["session_active"], s)

        # 10. enrolment closes once ballots exist; delete locked for a voter who voted
        c, r = call("POST", "/api/enroll/start", {"voter_ref": "VTR100009", "name": "Late%20Voter", "finger": "Right%20Thumb"})
        check("enrolment refused after the first ballot (409)", c == 409, (c, r))
        c, r = call("POST", "/api/enroll/delete", {"voter_ref": "VTR100001"}, token=TOKEN)
        check("deleting an enrolment of a voter who voted is blocked", c == 409, (c, r))
        c, r = call("POST", "/api/enroll/delete", {"voter_ref": "VTR100002"}, token=TOKEN)
        time.sleep(0.3)
        check("admin delete removes rows and clears the sensor slot", c == 200 and 3 in term.deleted and
              db.execute("select count(*) from biometric_templates where sensor_id=3").fetchone()[0] == 0, (c, r, term.deleted))
        db.close()
    finally:
        backend.terminate()
        try: out = backend.communicate(timeout=5)[0]
        except Exception: out = ""
        term.running = False
        if fails: print("\n--- backend log ---\n" + out)
    print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED: ' + ', '.join(fails)}")
    sys.exit(1 if fails else 0)

main()
