#include "ICM20948Sensor.h"

#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && __has_include(<ICM_20948.h>)
#include "detect/ScanI2CTwoWire.h"
#if !defined(MESHTASTIC_EXCLUDE_SCREEN)

// screen is defined in main.cpp
extern std::unique_ptr<graphics::Screen> screen;
#endif

// Flag when an interrupt has been detected
volatile static bool ICM20948_IRQ = false;

// Interrupt service routine
void ICM20948SetInterrupt()
{
    ICM20948_IRQ = true;
}

// startupMagnetometer() configures the AK09916 for continuous 100 Hz measurements.
// The ICM20948 SLEEP bit does not change the AK09916 operating mode, so explicitly
// power the auxiliary magnetometer down while the IMU is asleep and restore its
// normal mode when the IMU wakes. This avoids leaving the compass measuring while
// the display is off and heading data is not being consumed.
static ICM_20948_Status_e setMagnetometerMode(ICM20948Singleton *sensor, AK09916_mode_e mode)
{
    AK09916_CNTL2_Reg_t reg{};
    reg.MODE = mode;
    return sensor->writeMag(AK09916_REG_CNTL2, reinterpret_cast<uint8_t *>(&reg));
}

ICM20948Sensor::ICM20948Sensor(ScanI2C::FoundDevice foundDevice) : MotionSensor::MotionSensor(foundDevice) {}

bool ICM20948Sensor::init()
{
    // Initialise the sensor
    sensor = ICM20948Singleton::GetInstance();
    if (!sensor->init(device))
        return false;

    // Enable simple Wake on Motion
    bool wakeOnMotionOk = sensor->setWakeOnMotion();
    if (wakeOnMotionOk) {
        loadMagnetometerCalibration(compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
    }
    return wakeOnMotionOk;
}

#ifdef ICM_20948_INT_PIN

int32_t ICM20948Sensor::runOnce()
{
    // Wake on motion using hardware interrupts - this is the most efficient way to check for motion
    if (ICM20948_IRQ) {
        ICM20948_IRQ = false;
        sensor->clearInterrupts();
        wakeScreen();
    }
    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

#else

int32_t ICM20948Sensor::runOnce()
{
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    if (screen && !doCalibration && !screen->isScreenOn() && !config.display.wake_on_tap_or_motion &&
        !config.device.double_tap_as_button_press) {
        if (!isAsleep) {
            LOG_DEBUG("sleeping IMU");

            auto magStatus = setMagnetometerMode(sensor, AK09916_mode_power_down);
            if (magStatus != ICM_20948_Stat_Ok) {
                LOG_WARN("ICM20948 failed to power down magnetometer - %s", sensor->statusString(magStatus));
            }

            auto sleepStatus = sensor->sleep(true);
            if (sleepStatus != ICM_20948_Stat_Ok) {
                LOG_WARN("ICM20948 failed to enter sleep - %s", sensor->statusString(sleepStatus));
            }

            // Treat the low-power transition as active even if one command failed so
            // the wake path restores both devices instead of leaving the magnetometer
            // powered down after a partial transition.
            isAsleep = true;
        }
        return MOTION_SENSOR_SLEEP_CHECK_INTERVAL_MS;
    }
    if (isAsleep) {
        auto wakeStatus = sensor->sleep(false);
        if (wakeStatus != ICM_20948_Stat_Ok) {
            LOG_WARN("ICM20948 failed to leave sleep - %s", sensor->statusString(wakeStatus));
            return MOTION_SENSOR_CHECK_INTERVAL_MS;
        }

        auto magStatus = setMagnetometerMode(sensor, AK09916_mode_cont_100hz);
        if (magStatus != ICM_20948_Stat_Ok) {
            LOG_WARN("ICM20948 failed to restart magnetometer - %s", sensor->statusString(magStatus));
            return MOTION_SENSOR_CHECK_INTERVAL_MS;
        }

        isAsleep = false;
    }

    float magX = 0, magY = 0, magZ = 0;
    if (sensor->dataReady()) {
        sensor->getAGMT();
        magX = sensor->agmt.mag.axes.x;
        magY = sensor->agmt.mag.axes.y;
        magZ = sensor->agmt.mag.axes.z;
    }

    if (doCalibration) {
        beginCalibrationDisplay(showingScreen);
        updateCalibrationExtrema(magX, magY, magZ, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
        finishCalibrationIfExpired(showingScreen, compassCalibrationFileName, highestX, lowestX, highestY, lowestY, highestZ,
                                   lowestZ);
    }

    magX -= (highestX + lowestX) / 2;
    magY -= (highestY + lowestY) / 2;
    magZ -= (highestZ + lowestZ) / 2;
    FusionVector ga, ma;
    ga.axis.x = (sensor->agmt.acc.axes.x);
    ga.axis.y = -(sensor->agmt.acc.axes.y);
    ga.axis.z = -(sensor->agmt.acc.axes.z);
    ma.axis.x = magX;
    ma.axis.y = magY;
    ma.axis.z = magZ;

    // If we're set to one of the inverted positions
    if (config.display.compass_orientation > meshtastic_Config_DisplayConfig_CompassOrientation_DEGREES_270) {
        ma = FusionRemap(ma, FusionRemapAlignmentNXNYPZ);
        ga = FusionRemap(ga, FusionRemapAlignmentNXNYPZ);
    }

    float heading = FusionCompass(ga, ma, FusionConventionNed);

    heading = applyCompassOrientation(heading);
    if (screen)
        screen->setHeading(heading);
#endif

    // Wake on motion using polling  - this is not as efficient as using hardware interrupt pin (see above)
    auto status = sensor->setBank(0);
    if (sensor->status != ICM_20948_Stat_Ok) {
        LOG_DEBUG("ICM20948 isWakeOnMotion failed to set bank - %s", sensor->statusString());
        return MOTION_SENSOR_CHECK_INTERVAL_MS;
    }

    ICM_20948_INT_STATUS_t int_stat;
    status = sensor->read(AGB0_REG_INT_STATUS, (uint8_t *)&int_stat, sizeof(ICM_20948_INT_STATUS_t));
    if (status != ICM_20948_Stat_Ok) {
        LOG_DEBUG("ICM20948 isWakeOnMotion failed to read interrupts - %s", sensor->statusString());
        return MOTION_SENSOR_CHECK_INTERVAL_MS;
    }

    if (int_stat.WOM_INT != 0) {
        // Wake up!
        wakeScreen();
    }
    return MOTION_SENSOR_CHECK_INTERVAL_MS;
}

#endif

void ICM20948Sensor::calibrate(uint16_t forSeconds)
{
#if !defined(MESHTASTIC_EXCLUDE_SCREEN) && HAS_SCREEN
    LOG_DEBUG("ICM20948 cal start %is", forSeconds);
    if (sensor->dataReady()) {
        sensor->getAGMT();
        seedCalibrationExtrema(sensor->agmt.mag.axes.x, sensor->agmt.mag.axes.y, sensor->agmt.mag.axes.z, highestX, lowestX,
                               highestY, lowestY, highestZ, lowestZ);
    } else {
        seedCalibrationExtrema(0.0f, 0.0f, 0.0f, highestX, lowestX, highestY, lowestY, highestZ, lowestZ);
    }

    startCalibrationWindow(forSeconds);
#endif
}
// ----------------------------------------------------------------------
// ICM20948Singleton
// ----------------------------------------------------------------------

// Get a singleton wrapper for an Sparkfun ICM_20948_I2C
ICM20948Singleton *ICM20948Singleton::GetInstance()
{
    if (pinstance == nullptr) {
        pinstance = new ICM20948Singleton();
    }
    return pinstance;
}

ICM20948Singleton::ICM20948Singleton() {}

ICM20948Singleton::~ICM20948Singleton() {}

ICM20948Singleton *ICM20948Singleton::pinstance{nullptr};

// Initialise the ICM20948 Sensor
bool ICM20948Singleton::init(ScanI2C::FoundDevice device)
{
#ifdef ICM_20948_DEBUG
    // Set ICM_20948_DEBUG to enable helpful debug messages on Serial
    enableDebugging();
#endif

    // startup; the bus is resolved via the scanner: WIRE1 may be a bridged
    // bus rather than the local Wire1 (e.g. SenseCAP Indicator)
    TwoWire &bus = *ScanI2CTwoWire::fetchI2CBus(device.address);

    bool bAddr = (device.address.address == 0x69);
    delay(100);

    LOG_DEBUG("ICM20948 begin on addr 0x%02X (port=%d, bAddr=%d)", device.address.address, device.address.port, bAddr);

    ICM_20948_Status_e status = begin(bus, bAddr);
    if (status != ICM_20948_Stat_Ok) {
        LOG_DEBUG("ICM20948 init begin - %s", statusString());
        return false;
    }

    // SW reset to make sure the device starts in a known state
    if (swReset() != ICM_20948_Stat_Ok) {
        LOG_DEBUG("ICM20948 init reset - %s", statusString());
        return false;
    }
    delay(200);

    // Now wake the sensor up
    if (sleep(false) != ICM_20948_Stat_Ok) {
        LOG_DEBUG("ICM20948 init wake - %s", statusString());
        return false;
    }

    if (lowPower(false) != ICM_20948_Stat_Ok) {
        LOG_DEBUG("ICM20948 init high power - %s", statusString());
        return false;
    }

    if (startupMagnetometer(false) != ICM_20948_Stat_Ok) {
        LOG_DEBUG("ICM20948 init magnetometer - %s", statusString());
        return false;
    }

#ifdef ICM_20948_INT_PIN

    // Active low
    cfgIntActiveLow(true);
    LOG_DEBUG("ICM20948 init set cfgIntActiveLow - %s", statusString());

    // Push-pull
    cfgIntOpenDrain(false);
    LOG_DEBUG("ICM20948 init set cfgIntOpenDrain - %s", statusString());

    // If enabled, *ANY* read will clear the INT_STATUS register.
    cfgIntAnyReadToClear(true);
    LOG_DEBUG("ICM20948 init set cfgIntAnyReadToClear - %s", statusString());

    // Latch the interrupt until cleared
    cfgIntLatch(true);
    LOG_DEBUG("ICM20948 init set cfgIntLatch - %s", statusString());

    // Set up an interrupt pin with an internal pullup for active low
    pinMode(ICM_20948_INT_PIN, INPUT_PULLUP);

    // Set up an interrupt service routine
    attachInterrupt(ICM_20948_INT_PIN, ICM20948SetInterrupt, FALLING);

#endif
    return true;
}

#ifdef ICM_20948_DMP_IS_ENABLED

// Stub
bool ICM20948Sensor::initDMP()
{
    return false;
}

#endif

bool ICM20948Singleton::setWakeOnMotion()
{
    // Set WoM threshold in milli G's
    auto status = WOMThreshold(ICM_20948_WOM_THRESHOLD);
    if (status != ICM_20948_Stat_Ok)
        return false;

    // Enable WoM Logic mode 1 = Compare the current sample with the previous sample
    status = WOMLogic(true, 1);
    LOG_DEBUG("ICM20948 init set WOMLogic - %s", statusString());
    if (status != ICM_20948_Stat_Ok)
        return false;

    // Enable interrupts on WakeOnMotion
    status = intEnableWOM(true);
    LOG_DEBUG("ICM20948 init set intEnableWOM - %s", statusString());
    return status == ICM_20948_Stat_Ok;
}

#endif
