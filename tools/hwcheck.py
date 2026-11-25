#!/usr/bin/env python3
"""Hardware check for sc26re-1kHz: report rate and per-range gyro calibration.

usage:  python3 tools/hwcheck.py [--seconds 10] [--link cable|puck] [--report-id 1] [--skip-cal]

Run it from the repo root with the controller on the cable (or on the Puck, then use
--link puck; the Puck must be the only thing talking to the controller). Needs hidapi
(pip install hidapi, as in requirements.txt). Keep the controller lying completely still
on a table while it runs. Paste the whole output back.

What it checks
  1. Report rate: reports/s, new gyro samples/s (distinct IMU timestamps), gap p50/p90/p99.
  2. Gyro calibration per range: calibrate at 2000, calibrate at 250, then switch between the
     two several times. The still gyro must stay near zero after every switch (the v51.5+
     per-range slots) and the controller must report the saved result.
It changes the "gyro_fsr" setting (until power-off, never saved) and runs a gyro calibration at
each range (saved). The original range is put back at the end.
"""
import argparse, os, statistics, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import flash  # noqa: E402  (the repo's own HID helpers)

STATE_ID = 0x42
OFF_TS, OFF_GX = 29, 39  # offsets in the report body (after the report id)
GYRO_CAL_NAMES = {0: "idle", 1: "measuring", 2: "saved", 3: "moved", 4: "save failed"}


class Out:
    ok = True
    @staticmethod
    def line(status, text):
        print(f"[{status}] {text}")
        if status == "FAIL":
            Out.ok = False


def open_controller():
    import hid
    for pid in (flash.PID_TRITON_USB, flash.PID_TRITON_BLE, flash.PID_PROTEUS_USB):
        for info in hid.enumerate(flash.VALVE_VID, pid):
            yield info


def tagged(payload):
    """sc26re replies start with a path tag byte (v44+)."""
    return payload[1:] if payload else b""


def read_cfg(h, rid, name):
    p = flash.read_feature_setting(h, rid, "sc26re/" + name)
    p = tagged(p)
    return struct.unpack("<h", p[:2])[0] if len(p) >= 2 else None


def set_cfg(h, rid, name, value):
    flash.stage_feature_setting(h, rid, "sc26re/" + name, struct.pack("<h", value))
    time.sleep(0.05)


def gyro_cal_state(h, rid):
    p = tagged(flash.read_feature_setting(h, rid, "sc26re/gyro_cal"))
    return p[0] if p else None


def collect(h, seconds):
    """(host_time, imu_timestamp, gx, gy, gz) for every state report for `seconds`."""
    rows, end = [], time.monotonic() + seconds
    h.set_nonblocking(0)
    while time.monotonic() < end:
        d = h.read(64, 50)
        if not d or d[0] != STATE_ID:
            continue
        body = bytes(d[1:])
        if len(body) < OFF_GX + 6:
            continue
        ts = struct.unpack_from("<I", body, OFF_TS)[0]
        g = struct.unpack_from("<hhh", body, OFF_GX)
        rows.append((time.monotonic(), ts, *g))
    return rows


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


def test_rate(h, rid, seconds, link):
    rows = collect(h, seconds)
    if len(rows) < 100:
        Out.line("FAIL", f"only {len(rows)} state reports read; is the gyro on (start a calibration or open Steam)?")
        return
    span = rows[-1][0] - rows[0][0]
    rps = (len(rows) - 1) / span
    new = len({r[1] for r in rows}) / span
    gaps = [(b[0] - a[0]) * 1000 for a, b in zip(rows, rows[1:])]
    print(f"    {rps:.0f} reports/s, {new:.0f} new gyro samples/s, gap ms p50 {pct(gaps,.5):.2f} "
          f"p90 {pct(gaps,.9):.2f} p99 {pct(gaps,.99):.2f} max {max(gaps):.1f}")
    want_rps, want_new = (950, 940) if link == "cable" else (950, 880)
    Out.line("PASS" if rps >= want_rps else "FAIL", f"reports/s {rps:.0f} (want >= {want_rps})")
    Out.line("PASS" if new >= want_new else "FAIL", f"new gyro samples/s {new:.0f} (want >= {want_new})")
    # host timing adds its own jitter, so the gap is advice only
    Out.line("PASS" if pct(gaps, .99) < 3.0 else "WARN", f"p99 gap {pct(gaps,.99):.2f} ms (host timing included)")


def still_mean(h, seconds=3.0):
    rows = collect(h, seconds)
    if len(rows) < 50:
        return None
    ax = list(zip(*[(r[2], r[3], r[4]) for r in rows]))
    return [statistics.fmean(a) for a in ax], [statistics.pstdev(a) for a in ax]


def calibrate(h, rid, label):
    flash.hid_exchange(h, rid, 0xB5, b"", expect_response=False)  # CALIBRATE_GYRO (Steam + X)
    end = time.monotonic() + 8
    st = None
    while time.monotonic() < end:
        time.sleep(0.25)
        st = gyro_cal_state(h, rid)
        if st in (2, 3, 4):
            break
    ok = st == 2
    Out.line("PASS" if ok else "FAIL", f"calibration at {label}: {GYRO_CAL_NAMES.get(st, st)}"
             + ("" if ok else " (keep the controller completely still)"))
    return ok


def test_ranges(h, rid, tol):
    original = read_cfg(h, rid, "gyro_fsr")
    print(f"    gyro_fsr is {original} dps")
    results = {}
    try:
        for fsr in (2000, 250):
            set_cfg(h, rid, "gyro_fsr", fsr)
            time.sleep(1.5)
            if not calibrate(h, rid, f"{fsr} dps"):
                return
            time.sleep(0.5)
            m = still_mean(h)
            results[fsr] = m
            print(f"    after calibration at {fsr}: mean {[round(x,1) for x in m[0]]} std {[round(x,1) for x in m[1]]} counts")
        for i in range(4):
            for fsr in (2000, 250, 2000, 250):
                set_cfg(h, rid, "gyro_fsr", fsr)
                time.sleep(1.5)  # the sensor re-seeds and the stream restarts
                m = still_mean(h)
                worst = max(abs(x) for x in m[0])
                Out.line("PASS" if worst <= tol else "FAIL",
                         f"switch {i+1} to {fsr} dps: still mean {[round(x,1) for x in m[0]]} counts (tolerance {tol})")
    finally:
        if original:
            set_cfg(h, rid, "gyro_fsr", original)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--link", choices=("cable", "puck"), default="cable")
    ap.add_argument("--report-id", type=int, default=1)
    ap.add_argument("--tolerance", type=float, default=6.0, help="allowed still gyro mean in report counts")
    ap.add_argument("--skip-cal", action="store_true")
    a = ap.parse_args()

    try:
        import hid  # noqa: F401
    except ImportError:
        sys.exit("hidapi is missing: pip install hidapi")
    h = None
    for info in open_controller():
        try:
            cand = flash.open_hid_device(info["path"])
            ver = tagged(flash.read_feature_setting(cand, a.report_id, "sc26re/version"))
            if ver:
                h = cand
                print("controller:", info.get("product_string"), "sc26re", ver[4:].decode(errors="replace") if len(ver) > 4 else ver)
                break
            flash.close_device(cand)
        except Exception:
            continue
    if h is None:
        sys.exit("no sc26re controller found (cable connected? Steam and the report meter closed?)")
    try:
        print("1) report rate, keep the controller still")
        # a calibration start also switches the gyro on
        flash.hid_exchange(h, a.report_id, 0xB5, b"", expect_response=False)
        time.sleep(2.5)
        test_rate(h, a.report_id, a.seconds, a.link)
        if not a.skip_cal:
            print("2) gyro calibration per range")
            test_ranges(h, a.report_id, a.tolerance)
    finally:
        flash.close_device(h)
    print("RESULT:", "ALL PASSED" if Out.ok else "SOME CHECKS FAILED")
    sys.exit(0 if Out.ok else 1)


if __name__ == "__main__":
    main()
