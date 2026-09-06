#include "SafeFile.h"
#if defined(HELTEC_V4_OLED)
#include "Power.h"
#endif

#ifdef FSCom

// Only way to work on both esp32 and nrf52
static File openFile(const char *filename, bool fullAtomic, bool requireDestructivePower)
{
#if defined(HELTEC_V4_OLED)
    // This is the common boundary for auxiliary SafeFile users (sensor state,
    // high scores, probe caches, etc.) that do not pass through NodeDB.
    const bool powerSafe = requireDestructivePower ? heltecDestructiveStoragePowerIsSafe()
                                                   : heltecPreferenceStoragePowerIsSafe();
    if (!powerSafe) {
        LOG_WARN("Refusing SafeFile open for %s while fresh power is unsafe", filename);
        return File();
    }
#else
    (void)requireDestructivePower;
#endif
    concurrency::LockGuard g(spiLock);
    LOG_DEBUG("Opening %s, fullAtomic=%d", filename, fullAtomic);
    if (!fullAtomic) {
        FSCom.remove(filename); // Nuke the old file to make space (ignore if it !exists)
    }

    String filenameTmp = filename;
    filenameTmp += ".tmp";

    // FILE_O_WRITE appends on Adafruit_LittleFS (nRF52) and STM32 LittleFS, so a tmp left by an interrupted
    // write must go first. exists() guards it: a bare remove() of a missing file logs on Portduino.
    if (FSCom.exists(filenameTmp.c_str())) {
        LOG_DEBUG("Remove stale %s", filenameTmp.c_str());
        // Opening anyway would append to the stale bytes, and the XOR readback is 8 bits wide, so
        // polluted content has a real chance of verifying and being renamed over the good file.
        if (!FSCom.remove(filenameTmp.c_str())) {
            LOG_ERROR("Can't remove stale %s", filenameTmp.c_str());
            return File();
        }
    }

    // clear any previous LFS errors
    return FSCom.open(filenameTmp.c_str(), FILE_O_WRITE);
}

SafeFile::SafeFile(const char *_filename, bool fullAtomic, bool requireDestructivePower)
    : filename(_filename), f(openFile(_filename, fullAtomic, requireDestructivePower)), fullAtomic(fullAtomic),
      requireDestructivePower(requireDestructivePower)
{
}

size_t SafeFile::write(uint8_t ch)
{
    if (!f)
        return 0;

    crc = crc32Update(&ch, 1, crc);
    ++bytesExpected;
    return f.write(ch);
}

size_t SafeFile::write(const uint8_t *buffer, size_t size)
{
    if (!f)
        return 0;

    crc = crc32Update(buffer, size, crc);
    bytesExpected += size;
    return f.write((uint8_t const *)buffer, size); // This nasty cast is _IMPORTANT_ otherwise the correct adafruit method does
                                                   // not get used (they made a mistake in their typing)
}

/**
 * Atomically close the file (overwriting any old version) and readback the contents to confirm the hash matches
 *
 * @return false for failure
 */
bool SafeFile::close()
{
    if (!f)
        return false;

    spiLock->lock();
    f.close();
    spiLock->unlock();

    String filenameTmp = filename;
    filenameTmp += ".tmp";
    if (!testReadback(filenameTmp.c_str()))
        return false;

#if defined(HELTEC_V4_OLED)
    // Encoding/readback may be lengthy. Refresh authorization before the
    // rename that publishes the new generation.
    const bool powerSafe = requireDestructivePower ? heltecDestructiveStoragePowerIsSafe()
                                                   : heltecPreferenceStoragePowerIsSafe();
    if (!powerSafe) {
        LOG_WARN("Deferring SafeFile commit for %s because fresh power became unsafe", filename.c_str());
        return false;
    }
#endif

    // Rename or overwrite (atomic operation)
    if (!renameFile(filenameTmp.c_str(), filename.c_str())) {
        LOG_ERROR("Can't rename new pref file");
        return false;
    }

    // The rename is the persistence commit. Verify the committed pathname too,
    // not only its temporary predecessor, before callers clear recovery markers.
    if (!testReadback(filename.c_str())) {
        LOG_ERROR("Committed file failed readback: %s", filename.c_str());
        return false;
    }

    return true;
}

/// Read a closed file back in and compare both length and CRC32.
bool SafeFile::testReadback(const char *path)
{
    concurrency::LockGuard g(spiLock);

    auto f2 = FSCom.open(path, FILE_O_READ);
    if (!f2) {
        LOG_ERROR("Can't open %s for readback", path);
        return false;
    }

    int c = 0;
    uint32_t readbackCrc = CRC32_INITIAL;
    size_t bytesRead = 0;
    while ((c = f2.read()) >= 0) {
        const uint8_t byte = static_cast<uint8_t>(c);
        readbackCrc = crc32Update(&byte, 1, readbackCrc);
        ++bytesRead;
    }
    f2.close();

    if (bytesRead != bytesExpected || crc32Final(readbackCrc) != crc32Final(crc)) {
        LOG_ERROR("Readback mismatch for %s: expected %u bytes", path, static_cast<unsigned>(bytesExpected));
        return false;
    }

    return true;
}

#endif
