"""Offline end-to-end check of lib/rtpjpeg against FFmpeg, without hardware.

Builds tools/host_packetize.cpp, packetizes a JPEG N times, serves the packets
from a minimal RTSP server (TCP interleaved) and lets ffmpeg decode them.
Pass criteria: every frame decoded, decoded frames identical to a direct decode
of the source JPEG, and no 'overread' / mjpeg error in ffmpeg's log.

Usage: python tools/ffmpeg_loopback.py <file.jpg> [frames] [keepEoi 0|1]
Needs g++ and ffmpeg in PATH.
"""
import os
import socket
import struct
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / ".pio" / ("host_packetize.exe" if os.name == "nt" else "host_packetize")


def build():
    src = ROOT / "lib" / "rtpjpeg" / "src"
    cmd = ["g++", "-std=gnu++17", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{src}",
           str(ROOT / "tools" / "host_packetize.cpp"), *map(str, src.glob("*.cpp")), "-o", str(EXE)]
    EXE.parent.mkdir(exist_ok=True)
    subprocess.run(cmd, check=True)


def packets(jpeg, frames, keep_eoi):
    out = subprocess.run([str(EXE), jpeg, str(frames), keep_eoi], check=True, capture_output=True)
    sys.stdout.write(out.stderr.decode())
    data, pos, result = out.stdout, 0, []
    while pos < len(data):
        (n,) = struct.unpack(">H", data[pos:pos + 2])
        result.append(data[pos + 2:pos + 2 + n])
        pos += 2 + n
    return result


def serve(listener, pkts):
    conn, _ = listener.accept()
    buf = b""
    while True:
        while b"\r\n\r\n" not in buf:
            chunk = conn.recv(4096)
            if not chunk:
                return
            buf += chunk
        req, buf = buf.split(b"\r\n\r\n", 1)
        lines = req.decode().split("\r\n")
        method = lines[0].split()[0]
        cseq = next(l.split(":", 1)[1].strip() for l in lines if l.lower().startswith("cseq"))
        hdr = f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
        body = ""
        if method == "DESCRIBE":
            body = ("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=t\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
                    "m=video 0 RTP/AVP 26\r\na=control:track1\r\n")
            hdr += f"Content-Type: application/sdp\r\nContent-Length: {len(body)}\r\n"
        elif method == "SETUP":
            hdr += "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nSession: 1\r\n"
        elif method in ("PLAY", "TEARDOWN"):
            hdr += "Session: 1\r\n"
        conn.sendall((hdr + "\r\n" + body).encode())
        if method == "PLAY":
            for p in pkts:
                conn.sendall(b"$\x00" + struct.pack(">H", len(p)) + p)
            conn.close()
            return


def main():
    jpeg = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    keep_eoi = sys.argv[3] if len(sys.argv) > 3 else "1"
    build()
    pkts = packets(jpeg, frames, keep_eoi)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    threading.Thread(target=serve, args=(listener, pkts), daemon=True).start()

    ff = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "warning", "-rtsp_transport", "tcp",
                         "-i", f"rtsp://127.0.0.1:{port}/stream=0", "-f", "framemd5", "-"],
                        capture_output=True, timeout=60)
    ref = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", jpeg, "-f", "framemd5", "-"],
                         capture_output=True, timeout=60)
    md5 = lambda out: [l.split(",")[-1].strip() for l in out.decode().splitlines() if l and l[0] != "#"]
    got, want = md5(ff.stdout), md5(ref.stdout)
    log = ff.stderr.decode(errors="replace")
    bad = [l for l in log.splitlines() if "overread" in l or "mjpeg" in l.lower() or "error" in l.lower()]
    identical = sum(1 for m in got if want and m == want[0])
    print(f"packets={len(pkts)} decoded={len(got)}/{frames} identical={identical} problems={len(bad)}")
    for l in log.splitlines():
        print("  ffmpeg:", l)
    ok = len(got) == frames and identical == frames and not bad
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
