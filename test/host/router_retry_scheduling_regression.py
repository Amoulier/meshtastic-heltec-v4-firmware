#!/usr/bin/env python3
"""Exercise production retry scheduling and expiry with deterministic router/pool doubles.

Router's RUN_SAME preserves concurrent wakes. NextHopRouter must install its pending
deadline before RX processing rather than treating that sentinel as a delay, and a
synchronous local NAK must not leave expiry reading an erased map iterator.
"""

import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    syntax = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
        lambda match: " " * len(match.group()),
        source,
        flags=re.S,
    )
    depth = 0
    for index in range(opening, len(source)):
        depth += (syntax[index] == "{") - (syntax[index] == "}")
        if depth == 0:
            return source[start : index + 1]
    raise AssertionError(signature)


PREFIX = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
using std::min;
using NodeNum = uint32_t;
using PacketId = uint32_t;
using ErrorCode = int;
#define HELTEC_V4_OLED 1
#define RUN_SAME -1
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
constexpr int ERRNO_OK = 0, ERRNO_SHOULD_RELEASE = 35;
constexpr int meshtastic_Routing_Error_MAX_RETRANSMIT = 5;
constexpr uint8_t NO_NEXT_HOP_PREFERENCE = 0;
uint32_t clockMs = 1000;
namespace Time {
uint32_t getMillis() { return clockMs; }
uint32_t stampMillis() { return clockMs ? clockMs : 1; }
}
struct Throttle {
static bool deadlinePassedAt(uint32_t now, uint32_t deadline) { return uint32_t(now-deadline) < 0x80000000u; }
};
struct Packet { uint32_t from=1, to=2, id=0; uint8_t channel=0, next_hop=3; };
bool isFromUs(const Packet* p) { return p->from==1; }
bool isBroadcast(uint32_t to) { return to==UINT32_MAX; }
uint32_t getFrom(const Packet* p) { return p->from; }
struct GlobalPacketId {
 uint32_t node,id;
 GlobalPacketId(uint32_t from,uint32_t id):node(from),id(id){}
 bool operator<(const GlobalPacketId& o) const { return node!=o.node ? node<o.node : id<o.id; }
};
struct PendingPacket { Packet* packet; uint32_t nextTxMsec; uint8_t numRetransmissions; bool mqttImplicitAckSeen; };
struct Pool {
 int live=0;
 Packet* allocCopy(const Packet& p) { ++live; return new Packet(p); }
 void release(Packet* p) { assert(live>0); --live; delete p; }
} packetPool;
struct meshtastic_NodeInfoLite { uint8_t next_hop=3; };
struct NodeDB {
 bool allowed=true, editing=false, destructive=false;
 bool isPreferenceEditTransactionActive() const { return editing; }
 bool isDestructiveStorageMutationActive() const { return destructive; }
 struct ExternalStateAccessScope {
  bool allowed;
  explicit ExternalStateAccessScope(NodeDB* n):allowed(n->allowed){}
  explicit operator bool() const { return allowed; }
 };
 meshtastic_NodeInfoLite node;
 meshtastic_NodeInfoLite* getMeshNode(uint32_t) { return &node; }
} db;
NodeDB* nodeDB=&db;
struct FloodingRouter {
 int interval=INT32_MAX, sent=0;
 std::function<void()> receive;
 virtual ~FloodingRouter() = default;
 virtual int32_t getIdleInterval() const { return INT32_MAX; }
 void setInterval(int32_t value) { interval=value; }
 void setReceivedMessage() { interval=0; }
 virtual int32_t runOnce() {
  setInterval(getIdleInterval());
  if(receive) receive();
  return RUN_SAME;
 }
 ErrorCode send(Packet* p) { ++sent; packetPool.release(p); return ERRNO_OK; }
};
struct NextHopRouter : FloodingRouter {
 std::map<GlobalPacketId,PendingPacket> pending;
 int naks=0, failures=0;
 int32_t runOnce() override;
 int32_t getIdleInterval() const override;
 int32_t doRetransmissions();
 void seed(uint32_t id,uint32_t delay,uint8_t retries=0,bool mqtt=false) {
  Packet p; p.id=id;
  pending.emplace(GlobalPacketId(1,id), PendingPacket{packetPool.allocCopy(p),clockMs+delay,retries,mqtt});
 }
 void stopRetransmission(GlobalPacketId key) {
  auto found=pending.find(key);
  if(found!=pending.end()) { packetPool.release(found->second.packet); pending.erase(found); }
 }
 void sendAckNak(int,uint32_t from,uint32_t id,uint8_t) {
  ++naks;
  stopRetransmission(GlobalPacketId(from,id));
 }
 void noteRouteFailure(uint32_t) { ++failures; }
 ErrorCode sendWithNextHop(Packet* p,bool) { return send(p); }
 void setNextTx(PendingPacket* p) { p->nextTxMsec=clockMs+500; setReceivedMessage(); }
 ~NextHopRouter() { while(!pending.empty()) stopRetransmission(pending.begin()->first); }
};
"""

TESTS = r"""
int main() {
 {
  NextHopRouter r; r.seed(1,200,1);
  assert(r.runOnce()==RUN_SAME); assert(r.interval==200); assert(r.sent==0);
  clockMs+=200;
  assert(r.runOnce()==RUN_SAME); assert(r.sent==1); assert(r.interval==500);
 }
 {
  NextHopRouter r; r.seed(2,700,1);
  r.receive=[&] { r.setReceivedMessage(); };
  assert(r.runOnce()==RUN_SAME); assert(r.interval==0);
 }
 {
  NextHopRouter r; r.seed(3,700,1);
  r.receive=[&] { r.seed(4,50,1); r.setReceivedMessage(); };
  assert(r.runOnce()==RUN_SAME); assert(r.interval==0);
  r.receive={}; assert(r.runOnce()==RUN_SAME); assert(r.interval==50);
 }
 {
  NextHopRouter r; r.seed(5,0,1); db.allowed=false;
  assert(r.runOnce()==RUN_SAME); assert(r.interval==250); assert(r.sent==0);
  assert(r.pending.begin()->second.numRetransmissions==1);
  db.allowed=true; assert(r.runOnce()==RUN_SAME); assert(r.sent==1);
 }
 {
  NextHopRouter r; r.seed(10,0,1); db.editing=true;
  assert(r.runOnce()==RUN_SAME); assert(r.sent==0); assert(r.interval==250);
  db.editing=false; db.destructive=true;
  assert(r.runOnce()==RUN_SAME); assert(r.sent==0);
  db.destructive=false;
 }
 {
  NextHopRouter r; r.seed(11,100,1);
  r.receive=[&] { clockMs+=100; db.editing=true; };
  assert(r.runOnce()==RUN_SAME); assert(r.sent==0);
  assert(r.pending.begin()->second.numRetransmissions==1);
  db.editing=false;
 }
 {
  NextHopRouter r; r.seed(6,0); r.seed(7,0);
  assert(r.runOnce()==RUN_SAME); assert(r.pending.empty()); assert(r.naks==2);
 }
 {
  NextHopRouter r; r.seed(8,0,1,true);
  r.runOnce(); assert(r.sent==1); assert(r.naks==0); assert(r.pending.size()==1);
  clockMs+=500; r.runOnce(); assert(r.pending.empty()); assert(r.naks==0);
 }
 {
  NextHopRouter r; clockMs=UINT32_MAX-20; r.seed(9,70,1);
  assert(r.runOnce()==RUN_SAME); assert(r.interval==70);
  clockMs+=70; r.runOnce(); assert(r.sent==1);
 }
 assert(packetPool.live==0);
 puts("Router retry scheduling: PASS (deadlines, wakes, fence, synchronous NAK and MQTT retry)");
}
"""


def main():
    router = (ROOT / "src/mesh/Router.cpp").read_text()
    router_run = body(router, "int32_t Router::runOnce()")
    assert router_run.index("setInterval(getIdleInterval())") < router_run.index(
        "fromRadioQueue.dequeuePtr"
    )
    source = (ROOT / "src/mesh/NextHopRouter.cpp").read_text()
    methods = "\n".join(
        body(source, signature)
        for signature in (
            "int32_t NextHopRouter::runOnce()",
            "int32_t NextHopRouter::getIdleInterval() const",
            "int32_t NextHopRouter::doRetransmissions()",
        )
    )
    with tempfile.TemporaryDirectory(prefix="router-retry-") as directory:
        cpp = Path(directory) / "test.cpp"
        exe = Path(directory) / ("test.exe" if os.name == "nt" else "test")
        cpp.write_text(PREFIX + methods + TESTS)
        command = shlex.split(os.environ.get("CXX", "g++"))
        command += ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
        sanitizer_flags = (
            "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie"
            if os.name == "posix"
            else ""
        )
        command += shlex.split(os.environ.get("RADIO_TEST_CXXFLAGS", sanitizer_flags))
        subprocess.run(command + [str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
