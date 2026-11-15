"""End-to-end check of a running camera with FFmpeg.

Runs N parallel ffmpeg RTSP clients (TCP and/or UDP) plus optional HTTP MJPEG
readers for a fixed duration, then reports per client: decoded frames, fps,
and every warning ffmpeg printed. Pass = no 'overread' / mjpeg / RTP error
and fps close to the target.

Usage: python tools/rtsp_check.py <ip> [--seconds 60] [--tcp 2] [--udp 1] [--http 1] [--fps 10]
"""
import argparse
import re
import subprocess
import threading
import time
import urllib.request

BAD = re.compile(r"overread|mjpeg|error|corrupt|missing|invalid|RTP: (?:dropped|PT=)|max delay", re.I)


def run_ffmpeg(name, url, transport, seconds, results):
    cmd = ["ffmpeg", "-hide_banner", "-nostats", "-loglevel", "warning", "-rtsp_transport", transport,
           "-i", url, "-t", str(seconds), "-f", "framemd5", "-"]
    start = time.time()
    p = subprocess.run(cmd, capture_output=True, timeout=seconds + 30)
    elapsed = time.time() - start
    frames = sum(1 for line in p.stdout.decode(errors="replace").splitlines() if line and line[0] != "#")
    log = [l for l in p.stderr.decode(errors="replace").splitlines() if l.strip()]
    results[name] = {"frames": frames, "elapsed": elapsed, "log": log, "rc": p.returncode}


def run_http(name, url, seconds, results):
    frames, size, log = 0, 0, []
    start = time.time()
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
            buf = b""
            while time.time() - start < seconds:
                chunk = r.read(8192)
                if not chunk:
                    log.append("stream closed by server")
                    break
                buf += chunk
                while True:
                    s = buf.find(b"\xff\xd8")
                    e = buf.find(b"\xff\xd9", s + 2) if s >= 0 else -1
                    if s < 0 or e < 0:
                        break
                    frames += 1
                    size += e + 2 - s
                    buf = buf[e + 2:]
                if len(buf) > 1 << 20:
                    log.append("no JPEG boundary found in 1 MiB")
                    break
    except Exception as exc:  # report, do not crash the whole check
        log.append(f"HTTP error: {exc}")
    elapsed = time.time() - start
    results[name] = {"frames": frames, "elapsed": elapsed, "log": log, "rc": 0,
                     "avg": size // frames if frames else 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--tcp", type=int, default=2)
    ap.add_argument("--udp", type=int, default=1)
    ap.add_argument("--http", type=int, default=1)
    ap.add_argument("--fps", type=float, default=10.0)
    args = ap.parse_args()

    url = f"rtsp://{args.ip}:8554/stream=0"
    results, threads = {}, []
    for i in range(args.tcp):
        threads.append(threading.Thread(target=run_ffmpeg, args=(f"rtsp-tcp-{i}", url, "tcp", args.seconds, results)))
    for i in range(args.udp):
        threads.append(threading.Thread(target=run_ffmpeg, args=(f"rtsp-udp-{i}", url, "udp", args.seconds, results)))
    for i in range(args.http):
        threads.append(threading.Thread(target=run_http,
                                        args=(f"http-{i}", f"http://{args.ip}/mjpeg", args.seconds, results)))
    for t in threads:
        t.start()
        time.sleep(0.5)
    for t in threads:
        t.join()

    ok = True
    for name in sorted(results):
        r = results[name]
        fps = r["frames"] / args.seconds
        bad = [l for l in r["log"] if BAD.search(l)]
        good = not bad and fps >= args.fps * 0.8 and r["rc"] == 0
        ok &= good
        extra = f" avg={r['avg']} B" if "avg" in r else ""
        print(f"{'PASS' if good else 'FAIL'} {name}: {r['frames']} frames, {fps:.1f} fps, "
              f"{len(r['log'])} log line(s), {len(bad)} problem(s), rc={r['rc']}{extra}")
        for line in r["log"][:10]:
            print("    ", line)
    print("OVERALL", "PASS" if ok else "FAIL")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
