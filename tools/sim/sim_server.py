"""Virtual Steam Controller over HTTP for the web pages (WebHID shim in webhid_shim.js).
Runs the real remapper at 1 kHz in real time. POST /set {"A":1,"lsx":20000}, GET /report (the struct),
GET /report42 (the pad report body as the firmware sends it), GET /reports42?after=n (every report
since n, in order), GET /raw (sc26re/raw for tests), GET /truth (how the controller turned),
GET /reply, /reply_prev (the controller's shared feature reply),
POST /feature (hex), GET /events, POST /clear."""
import json, threading, time, struct, sys, collections
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from simlib import Sim, RFMT, RFIELDS, tag_of

sim = Sim()
lock = threading.Lock()
events = []
# the controller keeps one reply for GET_FEATURE, the answer to the last SET_FEATURE from anyone
# (another page, Steam): every page reads the latest. prev_reply: the one before (the Puck and
# Bluetooth hand that out when the page asks before the new reply arrived)
last_reply = prev_reply = b""
blocked = {}   # path -> time until which the controller does not answer it (a test makes native reads stop)
legacy = {"on": False, "taken": [0.0, 0.0, 0.0]}   # answer sc26re/raw as v51 test 42 did (40 bytes, the turn since the last read)

reports = collections.deque(maxlen=2000)  # every pad report (one per 5 ms step, 10 s), numbered
nrep = 0

def loop():
    global nrep
    while True:
        t = time.time()
        with lock:
            e = sim.run(5)
            events.extend(e)
            del events[:-5000]
            nrep += 1
            reports.append((nrep, sim.report42()))
        time.sleep(max(0, 0.005 - (time.time() - t)))

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, body, ctype="application/json"):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(200); self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b))); self.end_headers()
        self.wfile.write(b)
    def do_OPTIONS(self):
        self.send_response(204); self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*"); self.send_header("Access-Control-Allow-Methods", "*"); self.end_headers()
    def body(self):
        n = int(self.headers.get("Content-Length", 0)); return self.rfile.read(n).decode()
    def do_GET(self):
        if self.path == "/report":
            with lock: o = struct.pack(RFMT, *[sim.out[k] for k in RFIELDS])
            self._send(o.hex(), "text/plain")
        elif self.path == "/reply":
            with lock: self._send(last_reply.hex(), "text/plain")
        elif self.path == "/reply_prev":
            with lock: self._send(prev_reply.hex(), "text/plain")
        elif self.path == "/truth":
            with lock: self._send(json.dumps({"deg": sim.truth(), "q": sim.truth_q()}))
        elif self.path == "/raw":
            # the overlay's native data for a test, without touching the reply buffer
            with lock: self._send((sim.read("sc26re/raw") or b"").hex(), "text/plain")
        elif self.path.startswith("/reports42"):
            # the pad reports after number n, in order, as WebHID hands every one to the page
            n = int(self.path.split("=")[1]) if "=" in self.path else -1
            with lock:
                new = [(k, r) for k, r in reports if k > n]
                if n < 0 or (new and new[0][0] != n + 1): new = new[-1:]   # start, or fell behind: the newest
                self._send(json.dumps({"n": new[-1][0] if new else n, "r": [r.hex() for k, r in new]}))
        elif self.path == "/report42":
            with lock: self._send(sim.report42().hex(), "text/plain")
        elif self.path == "/events":
            with lock: self._send(json.dumps(events))
        elif self.path == "/fsr":
            with lock: self._send(json.dumps(sim.lib.sim_gyro_fsr()))
        elif self.path == "/state":
            with lock: self._send(json.dumps({"out": sim.out, "in": sim.state}))
        else: self.send_error(404)
    def do_POST(self):
        b = self.body()
        if self.path == "/feature":
            req = bytes.fromhex(b)
            import ctypes
            rsp = ctypes.create_string_buffer(64)
            global last_reply, prev_reply
            path = req[2:2 + req[1]].split(b"\0")[0].decode(errors="replace")
            with lock:
                if req[0] == 0xED and blocked.get(path, 0) > time.time():
                    r = 2; rsp = ctypes.create_string_buffer(b"\xed" + bytes(63), 64)
                elif req[0] == 0xED and path == "sc26re/raw" and legacy["on"]:
                    new = sim.read("sc26re/raw"); deg = sim.truth(); d = bytearray(new[:40])
                    for i in range(3):
                        t = int((deg[i] - legacy["taken"][i]) * 100); t = max(-32767, min(32767, t))
                        legacy["taken"][i] += t / 100; struct.pack_into("<h", d, 28 + 2 * i, t)
                    r = 2; rsp = ctypes.create_string_buffer(bytes([0xED, 41, tag_of(path)]) + bytes(d) + bytes(21), 64)
                else:
                    r = sim.lib.sim_feature(req, rsp)
                prev_reply, last_reply = last_reply, rsp.raw
            self._send(json.dumps({"r": r, "rsp": rsp.raw.hex()}))
        elif self.path == "/set":
            with lock: sim.set(**json.loads(b)); target = nrep + 2
            # answer once the controller has run with the new input (a test reading /state next sees it)
            t0 = time.time()
            while nrep < target and time.time() - t0 < 0.2: time.sleep(0.001)
            self._send("{}")
        elif self.path == "/restart":
            # the controller restarts (switched off and on) while the Puck keeps the page connected:
            # its clock and orientation start over, the saved profiles stay
            with lock: sim.lib.sim_reset_link(); sim.lib.sim_boot()
            self._send("{}")
        elif self.path == "/legacy":
            with lock: legacy["on"] = bool(json.loads(b)["on"]); legacy["taken"] = list(sim.truth())
            self._send("{}")
        elif self.path == "/block":
            j = json.loads(b); blocked[j["path"]] = time.time() + j["ms"] / 1000
            self._send("{}")
        elif self.path == "/xbox":
            # 1: an Xbox pad at the PC takes the Xbox outputs, 0: none (they reach Steam)
            with lock: sim.lib.sim_xbox(int(json.loads(b)["on"]))
            self._send("{}")
        elif self.path == "/clear":
            with lock: events.clear()
            self._send("{}")
        else: self.send_error(404)

threading.Thread(target=loop, daemon=True).start()
port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
print("virtual controller on", port, flush=True)
ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
