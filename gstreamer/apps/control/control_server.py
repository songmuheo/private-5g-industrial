#!/usr/bin/env python3
"""Control channel of the gstreamer tree: newline-delimited JSON over TCP.

Independent re-implementation for this tree (no code shared with webrtc/apps/signaling). Runs on the
receiver side so that a sender behind the 5G UE only needs an outbound TCP connection. Carries what
plain RTP has no in-band way to negotiate, and what the edge will later push to the cameras:

  receiver -> server : {"type":"register","role":"receiver","session":S,"receiver":RID,
                        "rtp_port":P,"rtcp_port":Q[,"host":H]}
  sender   -> server : {"type":"register","role":"sender","session":S,"stream":ID,"to":RID}
  server   -> sender : {"type":"receiver-ready","receiver":RID,"rtp_port":P,"rtcp_port":Q[,"host":H]}
                        (for every registered receiver of S; H absent = use the control host)
  sender   -> server : {"type":"stream-start","to":RID,"stream":ID,"ssrc":..,"pt":..,"clock_rate":..,
                        "rtcp_port_local":R, ...profile fields...}
  server   -> receiver: the same + "sender_host": <peer address of the sender's TCP connection>
                        (initial RTCP destination sender_host:R; the receiver switches to the endpoint
                        the sender's RTCP actually arrives from)
  receiver -> server : {"type":"stream-ack","stream":ID,"ok":true|false[,"reason":..]}
  server   -> sender : the same, only if it comes from the receiver that got the stream-start
                        (the sender goes PLAYING only after ok=true)
  anyone   -> server : {"type":"profile","session":S,"stream":ID, "bitrate_kbps":N, ...}
  server   -> sender : the same (edge-issued profile; docs/SCENARIO_EDGE_PROFILES.md §4 path (1))
  anyone   -> server : {"type":"ping"}   ->  {"type":"pong","server":"p5g-gstreamer-control"}   (identity check)
"""
import argparse
import asyncio
import json
import logging

log = logging.getLogger("control")


class Session:
    def __init__(self, name):
        self.name = name
        self.senders = {}    # stream id -> writer
        self.receivers = {}  # receiver id -> (writer, info dict)
        self.pending_ack = {}  # stream id -> receiver id that was sent stream-start (the only valid ack origin)


class ControlServer:
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
        elif t == "stream-start":
            await self.route_stream_start(writer, msg)
        elif t == "stream-ack":
            await self.route_stream_ack(writer, msg)
        elif t == "profile":
            await self.route_profile(writer, msg)
        elif t == "ping":
            await self.send(writer, {"type": "pong", "server": "p5g-gstreamer-control"})
        else:
            log.warning("unknown message type %r", t)

    @staticmethod
    def ready_msg(rid, info):
        m = {"type": "receiver-ready", "receiver": rid, "rtp_port": info["rtp_port"], "rtcp_port": info["rtcp_port"]}
        if info.get("host"):
            m["host"] = info["host"]
        return m

    async def register(self, writer, msg):
        s = self.session(msg["session"])
        if msg["role"] == "sender":
            if msg["stream"] in s.senders:
                log.warning("[%s] sender %s re-registered while another connection holds that stream id "
                            "(two senders with the same --stream-id?)", s.name, msg["stream"])
            s.senders[msg["stream"]] = writer
            self.peers[writer] = (s.name, "sender", msg["stream"])
            log.info("[%s] sender %s registered (to %s)", s.name, msg["stream"], msg.get("to"))
            for rid, (_, info) in s.receivers.items():
                await self.send(writer, self.ready_msg(rid, info))
        elif msg["role"] == "receiver":
            rid = msg["receiver"]
            if rid in s.receivers:
                log.warning("[%s] receiver %s re-registered while another connection holds that id "
                            "(two receivers with the same --receiver-id?)", s.name, rid)
            info = {"rtp_port": int(msg["rtp_port"]), "rtcp_port": int(msg["rtcp_port"]), "host": msg.get("host")}
            s.receivers[rid] = (writer, info)
            self.peers[writer] = (s.name, "receiver", rid)
            log.info("[%s] receiver %s registered rtp=%d rtcp=%d host=%s", s.name, rid, info["rtp_port"],
                     info["rtcp_port"], info["host"] or "(control host)")
            for w in s.senders.values():
                await self.send(w, self.ready_msg(rid, info))

    async def route_stream_start(self, writer, msg):
        sname, _, stream = self.peers[writer]
        s = self.session(sname)
        rid = msg["to"]
        if rid not in s.receivers:
            log.warning("[%s] stream-start from %s to unknown receiver %s", sname, stream, rid)
            return
        out = dict(msg)
        out["stream"] = stream
        out["sender_host"] = writer.get_extra_info("peername")[0]
        s.pending_ack[stream] = rid
        await self.send(s.receivers[rid][0], out)
        log.info("[%s] stream-start %s -> %s (ssrc %s, %sx%s@%s %s kbps)", sname, stream, rid, msg.get("ssrc"),
                 msg.get("width"), msg.get("height"), msg.get("fps"), msg.get("bitrate_kbps"))

    async def route_stream_ack(self, writer, msg):
        if writer not in self.peers:
            log.warning("stream-ack from an unregistered connection -> dropped")
            return
        sname, role, rid = self.peers[writer]
        s = self.session(sname)
        stream = msg.get("stream")
        # Only the receiver that was handed this stream's stream-start may acknowledge it.
        if role != "receiver" or s.pending_ack.get(stream) != rid:
            log.warning("[%s] stream-ack for %s from %s %s is not the intended receiver (%s) -> dropped",
                        sname, stream, role, rid, s.pending_ack.get(stream))
            return
        if stream not in s.senders:
            log.warning("[%s] stream-ack from %s for unknown stream %s", sname, rid, stream)
            return
        s.pending_ack.pop(stream, None)
        await self.send(s.senders[stream], msg)
        log.info("[%s] stream-ack %s -> %s ok=%s %s", sname, rid, stream, msg.get("ok"), msg.get("reason", ""))

    async def route_profile(self, writer, msg):
        s = self.session(msg.get("session", self.peers.get(writer, ("s1",))[0]))
        stream = msg.get("stream")
        if stream not in s.senders:
            log.warning("[%s] profile for unknown stream %s", s.name, stream)
            return
        await self.send(s.senders[stream], msg)
        log.info("[%s] profile -> %s: %s", s.name, stream, {k: v for k, v in msg.items() if k not in ("type", "session", "stream")})

    async def unregister(self, writer):
        if writer not in self.peers:
            return
        sname, role, pid = self.peers.pop(writer)
        s = self.session(sname)
        if role == "sender":
            if s.senders.get(pid) is writer:
                s.senders.pop(pid, None)
        elif s.receivers.get(pid, (None,))[0] is writer:
            s.receivers.pop(pid, None)
        log.info("[%s] %s %s disconnected", sname, role, pid)


async def main():
    ap = argparse.ArgumentParser(description="p5g gstreamer control channel (TCP JSON lines)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    srv = ControlServer()
    server = await asyncio.start_server(srv.handle, args.host, args.port)
    log.info("listening on %s:%d", args.host, args.port)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
