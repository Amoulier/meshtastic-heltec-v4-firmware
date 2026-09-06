#include "configuration.h"

#if defined(ARCH_PORTDUINO)
#include "sleep.h"

// The pinned Portduino adapter predates the Heltec sleep-policy arguments.
// Delegate to its original no-hardware implementation without altering shared
// production code or pretending that a native run can exercise physical sleep.
void cpuDeepSleep(uint32_t msecToWake);
void cpuDeepSleep(uint32_t msecToWake, DeepSleepWakePolicy, bool)
{
    cpuDeepSleep(msecToWake);
}
#endif
