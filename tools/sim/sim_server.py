"""Virtual Steam Controller over HTTP for the web pages (WebHID shim in webhid_shim.js).
Runs the real remapper at 1 kHz in real time. POST /set {"A":1,"lsx":20000}, GET /report,
POST /feature (hex), GET /events, POST /clear."""
import json, threading, time, struct, sys
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from simlib import Sim, RFMT, RFIELDS

sim = Sim()
lock = threading.Lock()
events = []

def loop():
    while True:
        t = time.time()
        with lock:
            e = sim.run(5)
            events.extend(e)
            del events[:-5000]
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
            with lock: r = sim.lib.sim_feature(req, rsp)
            self._send(json.dumps({"r": r, "rsp": rsp.raw.hex()}))
        elif self.path == "/set":
            with lock: sim.set(**json.loads(b))
            self._send("{}")
        elif self.path == "/clear":
            with lock: events.clear()
            self._send("{}")
        else: self.send_error(404)

threading.Thread(target=loop, daemon=True).start()
port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
print("virtual controller on", port, flush=True)
ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
