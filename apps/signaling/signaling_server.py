#!/usr/bin/env python3
"""WebRTC signaling relay: newline-delimited JSON over TCP.

Pure SDP relay, independent of the media stack. Runs on the receiver side (data network host) so
that the sender behind the 5G UE only needs an outbound TCP connection.

Protocol (one JSON object per line):
  sender   -> server : {"type":"register","role":"sender","session":S,"stream":ID}
  receiver -> server : {"type":"register","role":"receiver","session":S,"receiver":RID}
  server   -> sender : {"type":"receiver-ready","receiver":RID}          (for every receiver in S)
  sender   -> server : {"type":"offer","to":RID,"stream":ID,"sdp":...}
  server   -> receiver: {"type":"offer","stream":ID,"sdp":...}
  receiver -> server : {"type":"answer","stream":ID,"sdp":...}
  server   -> sender : {"type":"answer","receiver":RID,"sdp":...}

SDP is exchanged non-trickle (complete SDP including ICE candidates, once per side).
"""
import argparse
import asyncio
import json
import logging

log = logging.getLogger("signaling")


class Session:
    def __init__(self, name):
        self.name = name
        self.senders = {}    # stream id -> writer
        self.receivers = {}  # receiver id -> writer


class SignalingServer:
    def __init__(self):
        self.sessions = {}
        self.peers = {}  # writer -> (session, role, id)

    def session(self, name):
        return self.sessions.setdefault(name, Session(name))

    async def send(self, writer, msg):
        try:
            writer.write((json.dumps(msg) + "\n").encode())
            await writer.drain()
        except (ConnectionError, RuntimeError) as e:
            log.warning("send failed: %s", e)

    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername")
        log.info("connection from %s", peer)
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    log.warning("bad json from %s: %r", peer, line[:80])
                    continue
                await self.dispatch(writer, msg)
        finally:
            await self.unregister(writer)
            writer.close()

    async def dispatch(self, writer, msg):
        t = msg.get("type")
        if t == "register":
            await self.register(writer, msg)
        elif t == "offer":
            await self.route_offer(writer, msg)
        elif t == "answer":
            await self.route_answer(writer, msg)
        else:
            log.warning("unknown message type %r", t)

    async def register(self, writer, msg):
        s = self.session(msg["session"])
        if msg["role"] == "sender":
            s.senders[msg["stream"]] = writer
            self.peers[writer] = (s.name, "sender", msg["stream"])
            log.info("[%s] sender %s registered", s.name, msg["stream"])
            for rid in s.receivers:
                await self.send(writer, {"type": "receiver-ready", "receiver": rid})
        elif msg["role"] == "receiver":
            s.receivers[msg["receiver"]] = writer
            self.peers[writer] = (s.name, "receiver", msg["receiver"])
            log.info("[%s] receiver %s registered", s.name, msg["receiver"])
            for w in s.senders.values():
                await self.send(w, {"type": "receiver-ready", "receiver": msg["receiver"]})

    async def route_offer(self, writer, msg):
        sname, _, stream = self.peers[writer]
        s = self.session(sname)
        rid = msg["to"]
        if rid not in s.receivers:
            log.warning("[%s] offer from %s to unknown receiver %s", sname, stream, rid)
            return
        await self.send(s.receivers[rid], {"type": "offer", "stream": stream, "sdp": msg["sdp"]})
        log.info("[%s] offer %s -> %s", sname, stream, rid)

    async def route_answer(self, writer, msg):
        sname, _, rid = self.peers[writer]
        s = self.session(sname)
        stream = msg["stream"]
        if stream not in s.senders:
            log.warning("[%s] answer from %s for unknown stream %s", sname, rid, stream)
            return
        await self.send(s.senders[stream], {"type": "answer", "receiver": rid, "sdp": msg["sdp"]})
        log.info("[%s] answer %s -> %s", sname, rid, stream)

    async def unregister(self, writer):
        if writer not in self.peers:
            return
        sname, role, pid = self.peers.pop(writer)
        s = self.session(sname)
        table = s.senders if role == "sender" else s.receivers
        if table.get(pid) is writer:
            table.pop(pid, None)
        log.info("[%s] %s %s disconnected", sname, role, pid)


async def main():
    ap = argparse.ArgumentParser(description="p5g WebRTC signaling relay (TCP JSON lines)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    srv = SignalingServer()
    server = await asyncio.start_server(srv.handle, args.host, args.port)
    log.info("listening on %s:%d", args.host, args.port)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
