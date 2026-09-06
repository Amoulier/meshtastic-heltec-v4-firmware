/**
 * @file xmodem.cpp
 * @brief Implementation of XMODEM protocol for Meshtastic devices.
 *
 * This file contains the implementation of the XMODEM protocol for Meshtastic devices. It is based on the XMODEM implementation
 * by Georges Menie (www.menie.org) and has been adapted for protobuf encapsulation.
 *
 * The XMODEM protocol is used for reliable transmission of binary data over a serial connection. This implementation supports
 * both sending and receiving of data.
 *
 * The XModemAdapter class provides the main functionality for the protocol, including CRC calculation, packet handling, and
 * control signal sending.
 *
 * @copyright Copyright (c) 2001-2019 Georges Menie
 * @author
 * @author
 * @date
 */
/***********************************************************************************************************************
 * based on XMODEM implementation by Georges Menie (www.menie.org)
 ***********************************************************************************************************************
 * Copyright 2001-2019 Georges Menie (www.menie.org)
 * All rights reserved.
 *
 * Adapted for protobuf encapsulation. this is not really Xmodem any more.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University of California, Berkeley nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS AND CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **********************************************************************************************************************/

#include "xmodem.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "concurrency/LockGuard.h"
#include <cstring>

#ifdef FSCom

XModemAdapter xModem;

XModemAdapter::XModemAdapter() {}

bool XModemAdapter::isBusy()
{
    concurrency::LockGuard guard(&transactionLock);
    return isReceiving || isTransmitting;
}

bool XModemAdapter::beginExclusiveStorageMutation(bool cancelActiveTransfer)
{
    concurrency::LockGuard guard(&transactionLock);
    if (storageMutationBlocked)
        return false;
    if (isReceiving || isTransmitting) {
        if (!cancelActiveTransfer)
            return false;

        // The full-reset path has already fenced PhoneAPI/HTTP readers. Close
        // the abandoned handle and remove only a partial upload; a download's
        // source is never deleted.
        const bool removePartialReceive = isReceiving;
        concurrency::LockGuard spiGuard(spiLock);
        if (isReceiving)
            file.flush();
        file.close();
        if (removePartialReceive && filename[0] != '\0')
            FSCom.remove(filename);
        isReceiving = false;
        isTransmitting = false;
        isEOT = false;
        xmodemStore = meshtastic_XModem_init_zero;
        LOG_WARN("XModem: abandoned transfer cancelled for full recovery");
    }
    storageMutationBlocked = true;
    return true;
}

void XModemAdapter::endExclusiveStorageMutation()
{
    concurrency::LockGuard guard(&transactionLock);
    storageMutationBlocked = false;
}

#if defined(HELTEC_V4_OLED)
static bool isProtectedHeltecStoragePath(const char *name)
{
    if (!name)
        return true;
    while ((name[0] == '.' && (name[1] == '/' || name[1] == '\\')) || name[0] == '/' || name[0] == '\\') {
        name += (name[0] == '.') ? 2 : 1;
    }
    const char *separator = strpbrk(name, "/\\");
    const size_t componentLength = separator ? static_cast<size_t>(separator - name) : strlen(name);
    const bool preferencesTree = componentLength == strlen("prefs") && memcmp(name, "prefs", componentLength) == 0;
    const bool backupTree = componentLength == strlen("backups") && memcmp(name, "backups", componentLength) == 0;
    const bool resetMarker = !separator &&
                             (strcmp(name, "heltec-nvs-reset.pending") == 0 ||
                              strcmp(name, "heltec-nvs-reset.pending.tmp") == 0);
    return preferencesTree || backupTree || resetMarker;
}
#endif

bool XModemAdapter::isValidFilename(const char *name)
{
    if (!name || name[0] == '\0')
        return false;
    const bool driveLetter = (name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z');
    if (driveLetter && name[1] == ':')
        return false;
    // Reject any ".." path component. Absolute paths and subdirectories are fine; they stay within
    // the filesystem root, so only traversal out of it needs blocking.
    for (const char *seg = name; *seg;) {
        const char *slash = strpbrk(seg, "/\\");
        const size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.')
            return false;
        if (!slash)
            break;
        seg = slash + 1;
    }
    return true;
}

/**
 * Calculates the CRC-16 CCITT checksum of the given buffer.
 *
 * @param buffer The buffer to calculate the checksum for.
 * @param length The length of the buffer.
 * @return The calculated checksum.
 */
unsigned short XModemAdapter::crc16_ccitt(const pb_byte_t *buffer, int length)
{
    unsigned short crc16 = 0;
    while (length != 0) {
        crc16 = (unsigned char)(crc16 >> 8) | (crc16 << 8);
        crc16 ^= *buffer;
        crc16 ^= (unsigned char)(crc16 & 0xff) >> 4;
        crc16 ^= (crc16 << 8) << 4;
        crc16 ^= ((crc16 & 0xff) << 4) << 1;
        buffer++;
        length--;
    }

    return crc16;
}

/**
 * Calculates the checksum of the given buffer and compares it to the given
 * expected checksum. Returns 1 if the checksums match, 0 otherwise.
 *
 * @param buf The buffer to calculate the checksum of.
 * @param sz The size of the buffer.
 * @param tcrc The expected checksum.
 * @return 1 if the checksums match, 0 otherwise.
 */
int XModemAdapter::check(const pb_byte_t *buf, int sz, unsigned short tcrc)
{
    return crc16_ccitt(buf, sz) == tcrc;
}

void XModemAdapter::sendControl(meshtastic_XModem_Control c)
{
    xmodemStore = meshtastic_XModem_init_zero;
    xmodemStore.control = c;
    LOG_DEBUG("XModem: Notify Send control %d", c);
    packetReady.notifyObservers(packetno);
}

meshtastic_XModem XModemAdapter::getForPhone()
{
    return xmodemStore;
}

void XModemAdapter::resetForPhone()
{
    xmodemStore = meshtastic_XModem_init_zero;
}

void XModemAdapter::handlePacket(meshtastic_XModem xmodemPacket)
{
    concurrency::LockGuard transactionGuard(&transactionLock);
#if defined(HELTEC_V4_OLED)
    if (!shouldUseFilesystemPersistence(fsIsMounted())) {
        LOG_WARN("XModem: filesystem unavailable");
        sendControl(meshtastic_XModem_Control_NAK);
        return;
    }
    if (storageMutationBlocked) {
        LOG_WARN("XModem: storage transaction in progress");
        sendControl(meshtastic_XModem_Control_NAK);
        return;
    }
    if (nodeDB && nodeDB->requiresConfigRecovery()) {
        // Recovery exposes only the narrowly allowlisted Admin/BLE-OTA path.
        // Do not permit config/key/backup reads or writes through XMODEM.
        LOG_WARN("XModem: disabled while configuration recovery is required");
        sendControl(meshtastic_XModem_Control_NAK);
        return;
    }
#endif

    switch (xmodemPacket.control) {
    case meshtastic_XModem_Control_SOH:
    case meshtastic_XModem_Control_STX:
        if ((xmodemPacket.seq == 0) && !isReceiving && !isTransmitting) {
            // NULL packet has the destination filename
            strncpy(filename, (const char *)xmodemPacket.buffer.bytes, sizeof(filename) - 1);
            filename[sizeof(filename) - 1] = '\0';

            // The filename is attacker-controlled; refuse a ".." that could write/read/delete
            // outside the filesystem root (real host paths on the posix daemon).
            if (!isValidFilename(filename)) {
                LOG_WARN("XModem: rejecting unsafe filename");
                sendControl(meshtastic_XModem_Control_NAK);
                break;
            }

#if defined(HELTEC_V4_OLED)
            // XMODEM has no per-connection authorization on this target and is
            // reachable through the HTTP PhoneAPI transport. Neither upload
            // nor download may expose/replace private keys, channel PSKs,
            // network credentials, backups or recovery intent.
            if (isProtectedHeltecStoragePath(filename)) {
                LOG_WARN("XModem: transfer rejected for protected/recovery path");
                sendControl(meshtastic_XModem_Control_NAK);
                break;
            }
#endif

            if (xmodemPacket.control == meshtastic_XModem_Control_SOH) { // Receive this file and put to Flash
                // FILE_O_WRITE on Adafruit_LittleFS is append, not truncate - remove first.
                spiLock->lock();
                const bool oldFileRemoved = !FSCom.exists(filename) || (FSCom.remove(filename) && !FSCom.exists(filename));
                if (oldFileRemoved)
                    file = FSCom.open(filename, FILE_O_WRITE);
                spiLock->unlock();
                if (oldFileRemoved && file) {
                    LOG_INFO("XModem: receiving %s", filename);
                    sendControl(meshtastic_XModem_Control_ACK);
                    isReceiving = true;
                    isEOT = false;
                    retrans = MAXRETRANS;
                    packetno = 1;
                    break;
                }
                LOG_WARN("XModem: open(%s, WRITE) failed", filename);
                sendControl(meshtastic_XModem_Control_NAK);
                isReceiving = false;
                break;
            } else { // Transmit this file from Flash
                LOG_INFO("XModem: Transmit file %s", filename);
                spiLock->lock();
                file = FSCom.open(filename, FILE_O_READ);
                spiLock->unlock();
                if (file) {
                    packetno = 1;
                    isTransmitting = true;
                    xmodemStore = meshtastic_XModem_init_zero;
                    xmodemStore.control = meshtastic_XModem_Control_SOH;
                    xmodemStore.seq = packetno;
                    spiLock->lock();
                    xmodemStore.buffer.size = file.read(xmodemStore.buffer.bytes, sizeof(meshtastic_XModem_buffer_t::bytes));
                    spiLock->unlock();
                    xmodemStore.crc16 = crc16_ccitt(xmodemStore.buffer.bytes, xmodemStore.buffer.size);
                    LOG_DEBUG("XModem: STX Notify Send packet %d, %d Bytes", packetno, xmodemStore.buffer.size);
                    if (xmodemStore.buffer.size < sizeof(meshtastic_XModem_buffer_t::bytes)) {
                        isEOT = true;
                        // send EOT on next Ack
                    }
                    packetReady.notifyObservers(packetno);
                    break;
                }
                sendControl(meshtastic_XModem_Control_NAK);
                isTransmitting = false;
                break;
            }
        } else {
            if (isReceiving) {
                // normal file data packet
                if ((xmodemPacket.seq == packetno) &&
                    check(xmodemPacket.buffer.bytes, xmodemPacket.buffer.size, xmodemPacket.crc16)) {
                    // valid packet
                    spiLock->lock();
                    size_t written = file.write(xmodemPacket.buffer.bytes, xmodemPacket.buffer.size);
                    spiLock->unlock();
                    if (written != xmodemPacket.buffer.size) {
                        LOG_WARN("XModem: short write seq=%d expected=%d wrote=%d (LittleFS partition full?)",
                                 (int)xmodemPacket.seq, (int)xmodemPacket.buffer.size, (int)written);
                        spiLock->lock();
                        file.close();
                        FSCom.remove(filename);
                        spiLock->unlock();
                        isReceiving = false;
                        sendControl(meshtastic_XModem_Control_CAN);
                        break;
                    }
                    sendControl(meshtastic_XModem_Control_ACK);
                    packetno++;
                    break;
                }
                // invalid packet
                sendControl(meshtastic_XModem_Control_NAK);
                break;
            } else if (isTransmitting) {
                // just received something weird.
                sendControl(meshtastic_XModem_Control_CAN);
                spiLock->lock();
                file.close();
                spiLock->unlock();
                isTransmitting = false;
                break;
            }
        }
        break;
    case meshtastic_XModem_Control_EOT:
        // End of transmission
        if (!isReceiving) {
            sendControl(meshtastic_XModem_Control_NAK);
            break;
        }
        sendControl(meshtastic_XModem_Control_ACK);
        spiLock->lock();
        file.flush();
        file.close();
        spiLock->unlock();
        isReceiving = false;
        break;
    case meshtastic_XModem_Control_CAN: {
        // Cancel a receive and remove only its partial destination. Cancelling a
        // download must never delete the source file.
        sendControl(meshtastic_XModem_Control_ACK);
        const bool removePartialReceive = isReceiving;
        spiLock->lock();
        if (isReceiving)
            file.flush();
        file.close();
        if (removePartialReceive)
            FSCom.remove(filename);
        spiLock->unlock();
        isReceiving = false;
        isTransmitting = false;
        isEOT = false;
        break;
    }
    case meshtastic_XModem_Control_ACK:
        // Acknowledge Send the next packet
        if (isTransmitting) {
            if (isEOT) {
                sendControl(meshtastic_XModem_Control_EOT);
                spiLock->lock();
                file.close();
                spiLock->unlock();
                LOG_INFO("XModem: Finished send file %s", filename);
                isTransmitting = false;
                isEOT = false;
                break;
            }
            retrans = MAXRETRANS; // reset retransmit counter
            packetno++;
            xmodemStore = meshtastic_XModem_init_zero;
            xmodemStore.control = meshtastic_XModem_Control_SOH;
            xmodemStore.seq = packetno;
            spiLock->lock();
            xmodemStore.buffer.size = file.read(xmodemStore.buffer.bytes, sizeof(meshtastic_XModem_buffer_t::bytes));
            spiLock->unlock();
            xmodemStore.crc16 = crc16_ccitt(xmodemStore.buffer.bytes, xmodemStore.buffer.size);
            LOG_DEBUG("XModem: ACK Notify Send packet %d, %d Bytes", packetno, xmodemStore.buffer.size);
            if (xmodemStore.buffer.size < sizeof(meshtastic_XModem_buffer_t::bytes)) {
                isEOT = true;
                // send EOT on next Ack
            }
            packetReady.notifyObservers(packetno);
        } else {
            // just received something weird.
            sendControl(meshtastic_XModem_Control_CAN);
        }
        break;
    case meshtastic_XModem_Control_NAK:
        // Negative acknowledge. Send the same buffer again
        if (isTransmitting) {
            if (--retrans <= 0) {
                sendControl(meshtastic_XModem_Control_CAN);
                spiLock->lock();
                file.close();
                spiLock->unlock();
                LOG_INFO("XModem: Retransmit timeout, cancel file %s", filename);
                isTransmitting = false;
                break;
            }
            xmodemStore = meshtastic_XModem_init_zero;
            xmodemStore.control = meshtastic_XModem_Control_SOH;
            xmodemStore.seq = packetno;
            spiLock->lock();
            file.seek((packetno - 1) * sizeof(meshtastic_XModem_buffer_t::bytes));

            xmodemStore.buffer.size = file.read(xmodemStore.buffer.bytes, sizeof(meshtastic_XModem_buffer_t::bytes));
            spiLock->unlock();
            xmodemStore.crc16 = crc16_ccitt(xmodemStore.buffer.bytes, xmodemStore.buffer.size);
            LOG_DEBUG("XModem: NAK Notify Send packet %d, %d Bytes", packetno, xmodemStore.buffer.size);
            if (xmodemStore.buffer.size < sizeof(meshtastic_XModem_buffer_t::bytes)) {
                isEOT = true;
                // send EOT on next Ack
            }
            packetReady.notifyObservers(packetno);
        } else {
            // just received something weird.
            sendControl(meshtastic_XModem_Control_CAN);
        }
        break;
    default:
        // Unknown control character
        break;
    }
}
#endif
