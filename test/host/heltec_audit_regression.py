#!/usr/bin/env python3
"""Compile the real changed C++ method bodies against deterministic hardware/pool doubles.

These are fault-injection host tests, not a physical SX1262 or a full device emulator.
No alternate implementation of sleep or ACK delivery is substituted under test.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def body(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    raise AssertionError(signature)


def build_run(name, cpp, extra=()):
    with tempfile.TemporaryDirectory(prefix="heltec-audit-") as d:
        source, exe = Path(d) / f"{name}.cpp", Path(d) / name
        source.write_text(cpp)
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-fno-pie",
                "-no-pie",
                "-pthread",
                f'-I{ROOT / "src"}',
                *extra,
                str(source),
                "-o",
                str(exe),
            ],
            check=True,
        )
        env = dict(
            os.environ,
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1",
        )
        subprocess.run([str(exe)], env=env, check=True, timeout=30)
    print(f"{name}: PASS (real production bodies, ASan+UBSan)", flush=True)


sx_prefix = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include "power/DeepSleepPolicy.h"
#define HAS_LORA_FEM 1
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
constexpr int RADIOLIB_ERR_NONE=0;
struct FEM { int slept=0; void setSleepModeEnable(){++slept;} void setTxModeEnable(){} void setRxModeEnable(){} } loraFEMInterface;
struct RadioLibInterface { void setStandby(){} void configHardwareForSend(){} };
struct Chip { int standbyResult=0, sleepResult=0, standbyCalls=0, sleepCalls=0;
 int standby(){++standbyCalls; return standbyResult;} int sleep(bool){++sleepCalls; return sleepResult;} };
template<class T> struct SX126xInterface: RadioLibInterface {
 std::atomic<bool> radioHardwareParked{false}, radioSleepConfirmed{false};
 std::atomic<uint32_t> configHeaderDetectedAt{0};
 bool isReceiving=false; int activeReceiveStart=0; T lora; int aborts=0;
 void disableInterrupt(){} void abortSending(){++aborts;} void checkNotification(){}
 void parkRadioHardware(); bool sleep(); int16_t trySetStandby();
 void configHardwareForSend(); void setTransmitEnable(bool txon);
};
"""
sx_methods = "\n\n".join(
    "template <typename T> " + body("src/mesh/SX126xInterface.cpp", sig)
    for sig in [
        "void SX126xInterface<T>::parkRadioHardware()",
        "bool SX126xInterface<T>::sleep()",
        "int16_t SX126xInterface<T>::trySetStandby()",
        "void SX126xInterface<T>::configHardwareForSend()",
        "void SX126xInterface<T>::setTransmitEnable(bool txon)",
    ]
)
sx_tests = r"""
int main(){
 SX126xInterface<Chip> r;
 r.configHeaderDetectedAt=123;
 r.parkRadioHardware(); assert(!r.sleep()); assert(r.lora.sleepCalls==0);
 assert(r.configHeaderDetectedAt==0);
 assert(shouldForceRadioResetForCriticalSleep(true,r.sleep()));
 assert(!shouldForceRadioResetForCriticalSleep(false,r.sleep()));
 r.radioHardwareParked=false; r.lora.standbyResult=-707;
 r.configHeaderDetectedAt=456;
 assert(!r.sleep()); assert(!r.sleep()); assert(r.lora.sleepCalls==0);
 assert(r.configHeaderDetectedAt==0);
 r.radioHardwareParked=false; r.lora.standbyResult=0; r.lora.sleepResult=-20;
 assert(!r.sleep()); int calls=r.lora.sleepCalls;
 assert(!r.sleep()); assert(r.lora.sleepCalls==calls);
 assert(shouldForceRadioResetForCriticalSleep(true,r.sleep()));
 r.radioHardwareParked=false; r.lora.sleepResult=0;
 assert(r.sleep()); calls=r.lora.sleepCalls;
 assert(r.sleep()); assert(r.lora.sleepCalls==calls);
 assert(!shouldForceRadioResetForCriticalSleep(true,r.sleep()));
 r.configHardwareForSend(); assert(!r.radioHardwareParked); assert(!r.radioSleepConfirmed);
 assert(r.sleep()); r.parkRadioHardware(); assert(!r.sleep());
 for(int n=0;n<1000;++n){
  r.configHardwareForSend(); r.lora.sleepResult=(n%3)?0:-20;
  const bool expected=(n%3)!=0; assert(r.sleep()==expected); assert(r.sleep()==expected);
 }
}
"""
for label, flags in [
    ("standard", ("-DHELTEC_V4_OLED=1",)),
    ("solar", ("-DHELTEC_V4_OLED=1", "-DHELTEC_V4_SOLAR_ROUTER_PROFILE=1")),
]:
    build_run(f"sx1262_{label}", sx_prefix + sx_methods + sx_tests, flags)

# Exercise the actual FEM conversion together with RadioInterface's normalization boundary.
fem_prefix = r"""
#include <cassert>
#include <cstdint>
#define HAS_LORA_FEM 1
#define HELTEC_V4 1
#define LOG_INFO(...) ((void)0)
struct LoRaFEMInterface { enum Type {NONE,GC1109_PA,KCT8103L_PA}; Type fem_type=KCT8103L_PA;
 int8_t powerConversion(int8_t); } loraFEMInterface;
struct {struct {bool is_licensed=false;} owner;} devicestate;
struct Region {uint8_t powerLimit=30;} region;
struct RadioInterface {int8_t power=0,requestedPower=0; Region* myRegion=&region; void limitPower(int8_t);};
"""
fem_impl = (
    body("src/mesh/LoRaFEMInterface.cpp", "int8_t LoRaFEMInterface::powerConversion(")
    + "\n"
    + body("src/mesh/RadioInterface.cpp", "void RadioInterface::limitPower(")
)
fem_tests = r"""
int main(){
 RadioInterface r;
 for(auto fem: {LoRaFEMInterface::GC1109_PA,LoRaFEMInterface::KCT8103L_PA}) {
  loraFEMInterface.fem_type=fem;
  for(int limit=10;limit<=30;++limit) {
   region.powerLimit=limit;
   for(int request=-9;request<=30;++request) {
    r.requestedPower=request; r.power=request;
    r.limitPower(22); const int expected=r.power;
    for(int recovery=0;recovery<20;++recovery) {r.limitPower(22);assert(r.power==expected);assert(r.requestedPower==request);}
   }
  }
 }
 region.powerLimit=30; loraFEMInterface.fem_type=LoRaFEMInterface::KCT8103L_PA;
 r.requestedPower=30; r.limitPower(22);assert(r.power==22);
 r.requestedPower=22; r.limitPower(22);assert(r.power==9);r.limitPower(22);assert(r.power==9);
 devicestate.owner.is_licensed=true;r.requestedPower=30;r.limitPower(22);assert(r.power==22);
 loraFEMInterface.fem_type=LoRaFEMInterface::NONE;devicestate.owner.is_licensed=false;
 r.requestedPower=20;region.powerLimit=17;r.limitPower(22);assert(r.power==17);
}
"""
build_run(
    "fem_recovery_idempotence",
    "#include <initializer_list>\n" + fem_prefix + fem_impl + fem_tests,
)

mqtt_src = (ROOT / "src/mqtt/MQTT.cpp").read_text()
start = mqtt_src.index("static MqttAckHistory mqttAckHistory;")
end = mqtt_src.index("inline bool shouldDropMqttDownlink", start)
mqtt_impl = mqtt_src[start:end]
mqtt_prefix = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "mqtt/MqttAckHistory.h"
using NodeNum=uint32_t; using PacketId=uint32_t; using ChannelIndex=uint8_t; using ErrorCode=int;
constexpr int ERRNO_OK=0, ERRNO_SHOULD_RELEASE=35, ERRNO_UNKNOWN=32;
constexpr int meshtastic_Routing_Error_NONE=0, meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT=2;
namespace concurrency {
 struct Lock {std::mutex mutex;};
 struct LockGuard {Lock* l; explicit LockGuard(Lock* l):l(l){l->mutex.lock();} ~LockGuard(){l->mutex.unlock();}};
}
struct Packet {int transport_mechanism=0;};
struct Pool {std::atomic<int> live{0}; void release(Packet* p){assert(p); --live; delete p;}} packetPool;
struct Routing {bool fail=false; Packet* allocAckNak(int,NodeNum,PacketId,ChannelIndex){if(fail)return nullptr; ++packetPool.live; return new Packet;}} routing;
Routing* routingModule=&routing;
struct Router {int result=ERRNO_SHOULD_RELEASE; std::atomic<int> calls{0}; std::function<void()> nested;
 int sendLocal(Packet* p){assert(p->transport_mechanism==2); ++calls; if(nested)nested();
  if(result!=ERRNO_SHOULD_RELEASE){packetPool.release(p);}
  return result;}} routingRouter;
Router* router=&routingRouter;
"""
mqtt_tests = r"""
int main(){
 assert(!sendMqttImplicitAck(1,0,0));
 routing.fail=true; assert(!sendMqttImplicitAck(1,1,0)); routing.fail=false;
 assert(sendMqttImplicitAck(1,1,0)); assert(!sendMqttImplicitAck(1,1,0));
 routingRouter.result=ERRNO_UNKNOWN; assert(!sendMqttImplicitAck(1,2,0));
 routingRouter.result=ERRNO_SHOULD_RELEASE; assert(sendMqttImplicitAck(1,2,0));
 routingRouter.result=ERRNO_OK; assert(sendMqttImplicitAck(1,3,0)); assert(!sendMqttImplicitAck(1,3,0));
 routingRouter.result=ERRNO_SHOULD_RELEASE;
 router=nullptr; assert(!sendMqttImplicitAck(1,4,0)); router=&routingRouter;
 assert(sendMqttImplicitAck(1,4,0));
 routingRouter.nested=[] {assert(!sendMqttImplicitAck(1,5,0));};
 assert(sendMqttImplicitAck(1,5,0)); routingRouter.nested={};
 std::atomic<int> admitted{0}; std::vector<std::thread> ts;
 for(int i=0;i<32;++i)ts.emplace_back([&]{if(sendMqttImplicitAck(1,6,0))++admitted;});
 for(auto& t:ts){t.join();}
 assert(admitted==1); assert(packetPool.live==0);
 MqttAckHistory h;
 for(uint32_t i=1;i<=h.capacity;++i)assert(h.reserve(7,i));
 assert(!h.reserve(7,17)); h.finish(7,1,false); assert(h.reserve(7,17));
 // No unrelated success/failure can evict in-flight reservations.
 for(uint32_t i=2;i<=h.capacity;++i)assert(!h.reserve(7,i));
 h.finish(7,17,true); assert(!h.reserve(7,17));
 h.finish(7,2,false); assert(h.reserve(7,2)); h.finish(7,2,true);
 assert(!h.reserve(7,2)); assert(h.reserve(8,2));
}
"""
build_run("mqtt_ack_admission", mqtt_prefix + mqtt_impl + mqtt_tests)

# Integration wiring: production paths, not just the policy helpers, must retain the fix.
sx = (ROOT / "src/mesh/SX126xInterface.cpp").read_text()
for m in re.finditer(
    r"radioHardwareParked\.store\(false, std::memory_order_release\)", sx
):
    assert "radioSleepConfirmed.store(false" in sx[max(0, m.start() - 100) : m.start()]
for path in [
    "src/graphics/Screen.cpp",
    "src/graphics/Screen.h",
    "src/graphics/draw/MessageRenderer.cpp",
]:
    text = (ROOT / path).read_text()
    assert "textMessageFrameShown" not in text and "isTextMessageFrameShown" not in text
assert (
    "std::atomic<bool> displayDisabled" in (ROOT / "src/graphics/Screen.h").read_text()
)
assert "getMinFreeHeap" not in (ROOT / "src/memGet.h").read_text()
assert "getMaxAllocHeap" not in (ROOT / "src/memGet.h").read_text()
admin = (ROOT / "src/modules/AdminModule.cpp").read_text()
reject = admin.index("sendWarning(LOW_ENTROPY_REJECT_WARNING)")
mutate = admin.index("config.security = incoming;", reject)
assert "return false;" in admin[reject:mutate]
assert (
    "current identity and node number were preserved"
    in (ROOT / "src/mesh/NodeDB.h").read_text()
)
assert "if (!deliverLocal(p, src))" in body(
    "src/mesh/Router.cpp", "ErrorCode Router::sendLocal("
)
print("production_wiring_and_exclusions: PASS")

subprocess.run(
    [sys.executable, str(ROOT / "test/host/banner_callback_regression.py")], check=True
)
subprocess.run(
    [sys.executable, str(ROOT / "test/host/radio_quiesce_regression.py")], check=True
)
