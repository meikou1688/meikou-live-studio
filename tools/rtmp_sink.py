#!/usr/bin/env python3
"""最小的 RTMP 收流伺服器，給自動巡檢用。

只做到「接受推流、統計收到的影音資料」：
握手 → connect → createStream → publish，之後計算收到的影像／聲音封包與位元組數。
結束時（收到 SIGTERM、或 --seconds 到期、或推流端斷線）把結果寫成 JSON。

用法：python3 rtmp_sink.py --port 1935 --report sink.json [--seconds 60]
"""
import argparse
import json
import os
import signal
import socket
import struct
import sys
import threading
import time

stats = {
    "connected": False,
    "app": "",
    "stream_key_received": False,
    "published": False,
    "video_packets": 0,
    "audio_packets": 0,
    "video_bytes": 0,
    "audio_bytes": 0,
    "avc_sequence_header": False,
    "aac_sequence_header": False,
    "keyframes": 0,
    "metadata": False,
    "first_media_time": None,
    "last_media_time": None,
    "errors": [],
}
lock = threading.Lock()


# ---------------------------------------------------------------- AMF0

def amf_read(buf, pos):
    t = buf[pos]
    pos += 1
    if t == 0x00:
        return struct.unpack(">d", buf[pos:pos + 8])[0], pos + 8
    if t == 0x01:
        return buf[pos] != 0, pos + 1
    if t == 0x02:
        n = struct.unpack(">H", buf[pos:pos + 2])[0]
        return buf[pos + 2:pos + 2 + n].decode("utf-8", "replace"), pos + 2 + n
    if t in (0x03, 0x08):
        if t == 0x08:
            pos += 4
        obj = {}
        while True:
            n = struct.unpack(">H", buf[pos:pos + 2])[0]
            pos += 2
            if n == 0 and buf[pos] == 0x09:
                return obj, pos + 1
            key = buf[pos:pos + n].decode("utf-8", "replace")
            pos += n
            obj[key], pos = amf_read(buf, pos)
    if t in (0x05, 0x06):
        return None, pos
    if t == 0x0A:
        cnt = struct.unpack(">I", buf[pos:pos + 4])[0]
        pos += 4
        arr = []
        for _ in range(cnt):
            v, pos = amf_read(buf, pos)
            arr.append(v)
        return arr, pos
    raise ValueError("unsupported AMF0 type %d" % t)


def amf_decode_all(buf):
    out, pos = [], 0
    while pos < len(buf):
        v, pos = amf_read(buf, pos)
        out.append(v)
    return out


def amf_write(v):
    if v is None:
        return b"\x05"
    if isinstance(v, bool):
        return b"\x01" + (b"\x01" if v else b"\x00")
    if isinstance(v, (int, float)):
        return b"\x00" + struct.pack(">d", float(v))
    if isinstance(v, str):
        b = v.encode("utf-8")
        return b"\x02" + struct.pack(">H", len(b)) + b
    if isinstance(v, dict):
        out = b"\x03"
        for k, val in v.items():
            kb = k.encode("utf-8")
            out += struct.pack(">H", len(kb)) + kb + amf_write(val)
        return out + b"\x00\x00\x09"
    raise TypeError(type(v))


# ---------------------------------------------------------------- 連線

class Conn:
    def __init__(self, sock):
        self.s = sock
        self.in_chunk = 128
        self.out_chunk = 4096
        self.prev = {}  # csid -> dict(ts, length, type, sid, ts_delta)
        self.partial = {}  # csid -> bytearray
        self.buf = bytearray()

    def recv_exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise EOFError()
            self.buf += d
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def handshake(self):
        c0c1 = self.recv_exact(1 + 1536)
        if c0c1[0] != 3:
            raise ValueError("bad RTMP version %d" % c0c1[0])
        s1 = struct.pack(">II", int(time.time()) & 0xFFFFFFFF, 0) + os.urandom(1528)
        self.s.sendall(b"\x03" + s1 + c0c1[1:])  # S0 + S1 + S2（回送 C1）
        self.recv_exact(1536)  # C2

    def send_msg(self, csid, mtype, payload, sid=0, ts=0):
        out = bytearray()
        first = True
        pos = 0
        while first or pos < len(payload):
            if first:
                out += bytes([(0 << 6) | csid])
                out += struct.pack(">I", ts)[1:] + struct.pack(">I", len(payload))[1:] + bytes([mtype])
                out += struct.pack("<I", sid)
                first = False
            else:
                out += bytes([(3 << 6) | csid])
            out += payload[pos:pos + self.out_chunk]
            pos += self.out_chunk
        self.s.sendall(bytes(out))

    def send_cmd(self, *values, sid=0):
        self.send_msg(3, 20, b"".join(amf_write(v) for v in values), sid=sid)

    def read_message(self):
        while True:
            b0 = self.recv_exact(1)[0]
            fmt, csid = b0 >> 6, b0 & 0x3F
            if csid == 0:
                csid = 64 + self.recv_exact(1)[0]
            elif csid == 1:
                b = self.recv_exact(2)
                csid = 64 + b[0] + b[1] * 256
            p = self.prev.setdefault(csid, {"ts": 0, "length": 0, "type": 0, "sid": 0, "delta": 0})
            if fmt == 0:
                h = self.recv_exact(11)
                ts = int.from_bytes(h[0:3], "big")
                p["length"] = int.from_bytes(h[3:6], "big")
                p["type"] = h[6]
                p["sid"] = struct.unpack("<I", h[7:11])[0]
                if ts == 0xFFFFFF:
                    ts = struct.unpack(">I", self.recv_exact(4))[0]
                p["ts"] = ts
                p["delta"] = 0
            elif fmt == 1:
                h = self.recv_exact(7)
                d = int.from_bytes(h[0:3], "big")
                p["length"] = int.from_bytes(h[3:6], "big")
                p["type"] = h[6]
                if d == 0xFFFFFF:
                    d = struct.unpack(">I", self.recv_exact(4))[0]
                p["delta"] = d
                if not self.partial.get(csid):
                    p["ts"] += d
            elif fmt == 2:
                h = self.recv_exact(3)
                d = int.from_bytes(h, "big")
                if d == 0xFFFFFF:
                    d = struct.unpack(">I", self.recv_exact(4))[0]
                p["delta"] = d
                if not self.partial.get(csid):
                    p["ts"] += d
            else:
                if not self.partial.get(csid):
                    p["ts"] += p["delta"]
            part = self.partial.setdefault(csid, bytearray())
            need = p["length"] - len(part)
            part += self.recv_exact(min(need, self.in_chunk))
            if len(part) >= p["length"]:
                payload = bytes(part)
                self.partial[csid] = bytearray()
                return p["type"], p["sid"], p["ts"], payload


def handle(sock):
    c = Conn(sock)
    c.handshake()
    while True:
        mtype, sid, ts, payload = c.read_message()
        if mtype == 1:  # Set Chunk Size
            c.in_chunk = struct.unpack(">I", payload[:4])[0] & 0x7FFFFFFF
        elif mtype == 20:  # AMF0 命令
            vals = amf_decode_all(payload)
            name = vals[0] if vals else ""
            tx = vals[1] if len(vals) > 1 else 0
            if name == "connect":
                with lock:
                    stats["connected"] = True
                    stats["app"] = (vals[2] or {}).get("app", "") if isinstance(vals[2], dict) else ""
                c.send_msg(2, 5, struct.pack(">I", 2500000))          # Window Ack Size
                c.send_msg(2, 6, struct.pack(">IB", 2500000, 2))      # Set Peer Bandwidth
                c.send_msg(2, 1, struct.pack(">I", c.out_chunk))      # Set Chunk Size
                c.send_cmd("_result", tx, {"fmsVer": "FMS/3,0,1,123", "capabilities": 31},
                           {"level": "status", "code": "NetConnection.Connect.Success",
                            "description": "Connection succeeded.", "objectEncoding": 0})
            elif name in ("releaseStream", "FCPublish"):
                c.send_cmd("_result", tx, None, None)
            elif name == "createStream":
                c.send_cmd("_result", tx, None, 1)
            elif name == "publish":
                with lock:
                    stats["published"] = True
                    stats["stream_key_received"] = len(vals) > 3 and bool(vals[3])
                c.send_cmd("onStatus", 0, None,
                           {"level": "status", "code": "NetStream.Publish.Start", "description": "Publishing"},
                           sid=1)
            elif name in ("FCUnpublish", "deleteStream", "closeStream"):
                pass
        elif mtype == 18:
            with lock:
                stats["metadata"] = True
        elif mtype in (8, 9) and payload:
            now = time.time()
            with lock:
                if stats["first_media_time"] is None:
                    stats["first_media_time"] = now
                stats["last_media_time"] = now
                if mtype == 9:
                    stats["video_packets"] += 1
                    stats["video_bytes"] += len(payload)
                    if payload[0] >> 4 == 1:
                        stats["keyframes"] += 1
                    if len(payload) > 1 and (payload[0] & 0x0F) == 7 and payload[1] == 0:
                        stats["avc_sequence_header"] = True
                else:
                    stats["audio_packets"] += 1
                    stats["audio_bytes"] += len(payload)
                    if len(payload) > 1 and (payload[0] >> 4) == 10 and payload[1] == 0:
                        stats["aac_sequence_header"] = True


def write_report(path):
    with lock:
        out = dict(stats)
        if out["first_media_time"] and out["last_media_time"]:
            dur = max(0.001, out["last_media_time"] - out["first_media_time"])
            out["media_seconds"] = round(dur, 2)
            out["video_kbps"] = round(out["video_bytes"] * 8 / 1000 / dur, 1)
        out.pop("first_media_time", None)
        out.pop("last_media_time", None)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1935)
    ap.add_argument("--report", default="rtmp_sink.json")
    ap.add_argument("--seconds", type=float, default=600)
    a = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port))
    srv.listen(4)
    srv.settimeout(0.5)
    deadline = time.time() + a.seconds
    stop = {"flag": False}

    def on_term(*_):
        stop["flag"] = True

    signal.signal(signal.SIGTERM, on_term)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, on_term)

    def serve(conn):
        try:
            handle(conn)
        except EOFError:
            pass
        except Exception as e:  # noqa: BLE001
            with lock:
                stats["errors"].append(repr(e))
        finally:
            conn.close()
            write_report(a.report)

    print("rtmp_sink listening on 127.0.0.1:%d" % a.port, flush=True)
    while not stop["flag"] and time.time() < deadline:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            write_report(a.report)
            continue
        threading.Thread(target=serve, args=(conn,), daemon=True).start()
    write_report(a.report)


if __name__ == "__main__":
    sys.exit(main())
