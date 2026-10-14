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
#include <functional>
#include <utility>
#include "power/DeepSleepPolicy.h"
#define HAS_LORA_FEM 1
#define INTERRUPT_ATTR
#define LOG_DEBUG(...) ((void)0)
#define LOG_DEBUG_RADIO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
constexpr int RADIOLIB_ERR_NONE=0, RADIOLIB_ERR_SPI_CMD_TIMEOUT=-705;
constexpr int TRANSMIT_DELAY_COMPLETED=3;
struct FEM { int slept=0, received=0; void setSleepModeEnable(){++slept;} void setTxModeEnable(){} void setRxModeEnable(){++received;} } loraFEMInterface;
struct NodeDB { bool editing=false, destructive=false;
 bool isPreferenceEditTransactionActive(){return editing;}
 bool isDestructiveStorageMutationActive(){return destructive;}
} database;
NodeDB* nodeDB=&database;
struct RadioLibInterface {
 int sequenceDepth=0;
 struct RadioSequence { RadioLibInterface* radio;
  explicit RadioSequence(RadioLibInterface* radio):radio(radio){++radio->sequenceDepth;}
  ~RadioSequence(){--radio->sequenceDepth;}
 };
 void setStandby(){} void configHardwareForSend(){}
};
struct Chip { int standbyResult=0, sleepResult=0, standbyCalls=0, sleepCalls=0;
 std::function<void()> duringStandby;
 int standby(){++standbyCalls; if(duringStandby){auto hook=std::move(duringStandby);hook();} return standbyResult;}
 int sleep(bool){++sleepCalls; return sleepResult;} };
template<class T> struct SX126xInterface: RadioLibInterface {
 std::atomic<bool> radioHardwareParked{false}, radioSleepConfirmed{false};
 std::atomic<uint32_t> configHeaderDetectedAt{0};
 bool isReceiving=false, rxArmedContinuous=false, cadParamsValid=false; int activeReceiveStart=0; T lora; int aborts=0;
 std::function<void()> duringDetach;
 void disableInterrupt(){if(duringDetach){auto hook=std::move(duringDetach);hook();}}
 void abortSending(){++aborts;} void checkNotificationExcept(int){}
 void parkRadioHardware(); bool sleep(); int16_t trySetStandby();
 void configHardwareForSend(); void setTransmitEnable(bool txon);
#ifdef MESHTASTIC_RX_READOUT_TASK
 enum RearmState : uint8_t {REARM_NONE,REARM_PENDING,REARM_ARMED,REARM_FAILED};
 uint8_t rearmState=REARM_NONE; int16_t rearmErr=0;
 bool rxArmedBeforeTxDone=false, rxRearmFromTaskPending=false;
 int rxStarts=0;
 bool requestRearmFromIsr(){rxRearmFromTaskPending=true;return true;}
 bool rearmReceiveFromIsr(); void rearmReceiveFromTask();
 int16_t startRxCommand(){++rxStarts;return RADIOLIB_ERR_NONE;}
 static void isrRxLevel0(){} void enableInterrupt(void(*)()){}
 void drainReadoutTask(){assert(sequenceDepth==0);if(rxRearmFromTaskPending){rxRearmFromTaskPending=false;rearmReceiveFromTask();}}
#endif
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
sx_rearm_methods = "\n\n".join(
    "template <typename T> " + body("src/mesh/SX126xInterface.cpp", sig)
    for sig in [
        "bool INTERRUPT_ATTR SX126xInterface<T>::rearmReceiveFromIsr()",
        "void SX126xInterface<T>::rearmReceiveFromTask()",
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
#ifdef MESHTASTIC_RX_READOUT_TASK
 for(int standbyResult : {0,-707}) {
  SX126xInterface<Chip> late;
  late.lora.standbyResult=standbyResult;
  late.lora.duringStandby=[&]{assert(late.sequenceDepth>0);assert(late.rearmReceiveFromIsr());};
  assert(late.sleep()==(standbyResult==0));
  assert(late.rearmState==late.REARM_NONE);
  const int femRx=loraFEMInterface.received;
  late.drainReadoutTask();
  assert(late.rxStarts==0);assert(loraFEMInterface.received==femRx);
  assert(late.rearmState==late.REARM_NONE);assert(!late.rxArmedBeforeTxDone);
  assert(late.radioHardwareParked);assert(late.radioSleepConfirmed==(standbyResult==0));
 }
 SX126xInterface<Chip> parked;
 parked.duringDetach=[&]{assert(parked.sequenceDepth>0);assert(parked.rearmReceiveFromIsr());};
 parked.parkRadioHardware();assert(parked.rearmState==parked.REARM_NONE);parked.drainReadoutTask();
 assert(parked.rxStarts==0);assert(parked.rearmState==parked.REARM_NONE);assert(!parked.sleep());
 // An ISR already in flight can publish after detach; its task must still leave a parked chip alone.
 assert(parked.rearmReceiveFromIsr());parked.drainReadoutTask();assert(parked.rxStarts==0);
 // Explicit activation still admits the next TX_DONE's fast RX handoff.
 parked.configHardwareForSend();
 assert(parked.rearmReceiveFromIsr());parked.drainReadoutTask();
 assert(parked.rxStarts==1);assert(parked.rearmState==parked.REARM_ARMED);
 assert(parked.rxArmedBeforeTxDone);assert(!parked.radioHardwareParked);assert(!parked.radioSleepConfirmed);
 for(int fence=0;fence<2;++fence) {
  parked.configHardwareForSend();database.editing=fence==0;database.destructive=fence==1;
  assert(parked.rearmReceiveFromIsr());parked.drainReadoutTask();assert(parked.rxStarts==1);
  database.editing=false;database.destructive=false;
 }
#endif
}
"""
for label, flags in [
    ("standard", ("-DHELTEC_V4_OLED=1",)),
    ("solar", ("-DHELTEC_V4_OLED=1", "-DHELTEC_V4_SOLAR_ROUTER_PROFILE=1")),
]:
    build_run(f"sx1262_{label}", sx_prefix + sx_methods + sx_tests, flags)
    build_run(
        f"sx1262_{label}_readout_sleep",
        sx_prefix + sx_methods + sx_rearm_methods + sx_tests,
        (*flags, "-DMESHTASTIC_RX_READOUT_TASK=1"),
    )

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
screen_header = (ROOT / "src/graphics/Screen.h").read_text()
assert "std::atomic<bool> textMessageFrameShown" in screen_header
assert "std::atomic<bool> displayDisabled" in screen_header
snapshot_reader = body(
    "src/graphics/Screen.cpp", "bool Screen::isTextMessageFrameShown()"
)
assert (
    "textMessageFrameShown.load()" in snapshot_reader
    and "getUiState" not in snapshot_reader
)
power_off = body("src/graphics/Screen.cpp", "void Screen::handleSetOn(")
assert power_off.index("textMessageFrameShown = false") < power_off.index(
    "enabled = false"
)
screen_run = body("src/graphics/Screen.cpp", "int32_t Screen::runOnce()")
disabled = screen_run.index("if (isDisplayDisabled())")
assert (
    "textMessageFrameShown = false"
    in screen_run[disabled : screen_run.index("return RUN_SAME", disabled)]
)
message_received = body(
    "src/graphics/draw/MessageRenderer.cpp", "void handleNewMessage("
)
assert "screen->isTextMessageFrameShown()" in message_received
assert (
    "shouldWakeOnReceivedMessage()" in message_received
    and "getUiState" not in message_received
)

wake_prefix = r"""
#include <cassert>
#define IS_ONE_OF(value, a, b, c, d) ((value)==(a)||(value)==(b)||(value)==(c)||(value)==(d))
enum {meshtastic_Config_DeviceConfig_Role_CLIENT, meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE,
 meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN, meshtastic_Config_DeviceConfig_Role_CLIENT_BASE, ROUTER};
struct ScreenStub { bool disabled=false; bool isDisplayDisabled() {return disabled;} } screenState;
ScreenStub* screen=&screenState;
struct { struct { bool enabled=false; } external_notification; } moduleConfig;
struct { struct { int role=meshtastic_Config_DeviceConfig_Role_CLIENT; } device; } config;
struct PowerStatusStub { int battery=100; int getBatteryChargePercent() {return battery;} } powerState;
PowerStatusStub* powerStatus=&powerState;
"""
wake_tests = r"""
int main() {
 assert(shouldWakeOnReceivedMessage());
 screenState.disabled=true; assert(!shouldWakeOnReceivedMessage());
 screenState.disabled=false; assert(shouldWakeOnReceivedMessage());
 moduleConfig.external_notification.enabled=true; assert(!shouldWakeOnReceivedMessage());
 moduleConfig.external_notification.enabled=false;
 config.device.role=ROUTER; assert(!shouldWakeOnReceivedMessage());
 config.device.role=meshtastic_Config_DeviceConfig_Role_CLIENT;
 powerState.battery=9; assert(!shouldWakeOnReceivedMessage());
 powerState.battery=10; assert(shouldWakeOnReceivedMessage());
}
"""
build_run(
    "heltec_display_wake",
    wake_prefix
    + body("src/graphics/Screen.cpp", "bool shouldWakeOnReceivedMessage()")
    + wake_tests,
    ("-DHELTEC_V4_OLED=1",),
)
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
subprocess.run(
    [sys.executable, str(ROOT / "test/host/router_retry_scheduling_regression.py")],
    check=True,
)
