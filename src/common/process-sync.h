// yabridge: a Wine plugin bridge
// Copyright (C) 2020-2026 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#ifndef YABRIDGE_PE_HOST
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <ctime>
#endif

#include "audio-shm.h"
#include "communication/common.h"

/**
 * The layout of the start of the shared memory object used by
 * `ProcessSync`. All fields are 32-bit so the layout is the same for the 32-bit
 * Wine plugin host and the 64-bit native plugin, and they're only accessed
 * through atomic builtins.
 */
struct ProcessSyncHeader {
    uint32_t magic;
    uint32_t version;
    /**
     * Set to 1 by the Wine plugin host when it supports using this object
     * instead of the socket, see `plugin_ready`.
     */
    uint32_t host_ready;
    /**
     * Incremented by the native plugin after it has written a new request to
     * the payload. The Wine plugin host waits on this futex.
     */
    uint32_t request_seq;
    /**
     * Set to the request's `request_seq` value by the Wine plugin host after
     * it has processed that request. The native plugin waits on this futex.
     */
    uint32_t response_seq;
    /**
     * Set to 1 by the native plugin when it's shutting down, so the Wine plugin
     * host's audio thread can stop waiting for requests.
     */
    uint32_t shutdown;
    /**
     * The size of the serialized request in the payload, in bytes.
     */
    uint32_t request_size;
    /**
     * Set to 1 by the native plugin when it will use this object for audio
     * processing. It does this before it sends its configuration back to the
     * Wine plugin host in response to the plugin's `AEffect` struct, so the
     * Wine plugin host knows which of the two to use after receiving that
     * configuration. This way mismatched yabridge versions can't result in one
     * side using the socket while the other side waits on the futex.
     */
    uint32_t plugin_ready;
};

/**
 * A shared memory object with a futex based channel for audio processing
 * requests. This is used instead of a socket for audio processing with the
 * 32-bit Wine plugin host. That host can only use Wine's Winsock, and in Wine
 * waking up a thread that's blocked on a socket involves wineserver. That adds
 * a lot of latency, with occasional spikes of tens of milliseconds, which
 * causes xruns. The futexes are used directly through Linux system calls on
 * both sides, so wineserver is not involved.
 *
 * The protocol is simple, since there is only ever one request in flight:
 *
 * 1. The native plugin writes the serialized request to the payload, increments
 *    `request_seq`, and wakes up the futex.
 * 2. The Wine plugin host's audio thread waits until `request_seq` changes,
 *    processes the request, sets `response_seq` to the same value, and wakes
 *    up that futex. The audio buffers themselves are already stored in the
 *    regular `AudioShmBuffer`.
 * 3. The native plugin waits until `response_seq` matches its request.
 *
 * The Wine plugin host creates this object and sets `host_ready` before it
 * sends the plugin's `AEffect` struct to the native plugin. If the native
 * plugin finds the object, it sets `plugin_ready` before it sends its
 * configuration back. The Wine plugin host's audio thread only switches to this
 * object after it has received that configuration and `plugin_ready` has been
 * set. Otherwise both sides keep using the socket. Setting
 * `YABRIDGE_NO_FUTEX=1` on either side disables this.
 */
class ProcessSync {
   public:
    static constexpr uint32_t expected_magic = 0x79627073;  // 'ybps'
    static constexpr uint32_t expected_version = 1;
    static constexpr uint32_t total_size = 64 * 1024;

    /**
     * The name of the shared memory object for a plugin instance, based on the
     * name of the instance's socket directory. This is also what the instance's
     * `AudioShmBuffer` is called, with a suffix.
     */
    static std::string name_for(const std::string& instance_name) {
        return instance_name + "-process";
    }

    /**
     * Whether this has been disabled using `YABRIDGE_NO_FUTEX=1`.
     */
    static bool disabled() noexcept {
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        const char* disable = getenv("YABRIDGE_NO_FUTEX");
        return disable && std::string(disable) == "1";
    }

#ifdef YABRIDGE_PE_HOST
    /**
     * Create the shared memory object. Called by the Wine plugin host. This
     * does not yet set `host_ready`.
     *
     * @throw std::system_error If the object could not be created.
     */
    static ProcessSync create(const std::string& name) {
        ProcessSync sync(name);
        ProcessSyncHeader& header = sync.header();
        header.version = expected_version;
        __atomic_store_n(&header.magic, expected_magic, __ATOMIC_RELEASE);

        return sync;
    }
#else
    /**
     * Open the shared memory object if the Wine plugin host created it and
     * set `host_ready`. Called by the native plugin. Returns a nullopt
     * otherwise, in which case audio should be processed using the socket.
     */
    static std::optional<ProcessSync> open_if_ready(const std::string& name);
#endif

    ProcessSyncHeader& header() noexcept {
        return *reinterpret_cast<ProcessSyncHeader*>(buffer_.data());
    }

    /**
     * Serialize a request to the payload. Does not touch `request_seq`.
     *
     * @throw std::system_error If the serialized request does not fit.
     */
    template <typename T>
    void write_request(const T& object, SerializationBufferBase& buffer) {
        const size_t size =
            bitsery::quickSerialization<OutputAdapter<SerializationBufferBase>>(
                buffer, object);
        if (size > payload_capacity) [[unlikely]] {
            throw std::system_error(
                std::make_error_code(std::errc::message_size),
                "Audio processing request too large");
        }

        std::memcpy(payload(), buffer.data(), size);
        __atomic_store_n(&header().request_size, static_cast<uint32_t>(size),
                         __ATOMIC_RELAXED);
    }

    /**
     * Deserialize the request from the payload.
     *
     * @throw std::runtime_error If the request could not be deserialized.
     */
    template <typename T>
    T& read_request(T& object, SerializationBufferBase& buffer) {
        const uint32_t size =
            __atomic_load_n(&header().request_size, __ATOMIC_RELAXED);
        if (size > payload_capacity) [[unlikely]] {
            throw std::runtime_error("Invalid audio processing request size");
        }

        buffer.resize(size);
        std::memcpy(buffer.data(), payload(), size);
        auto [_, success] = bitsery::quickDeserialization<
            InputAdapter<SerializationBufferBase>>({buffer.begin(), size},
                                                   object);
        if (!success) [[unlikely]] {
            throw std::runtime_error(
                "Could not deserialize audio processing request");
        }

        return object;
    }

   private:
    static constexpr size_t payload_offset = 64;
    static constexpr size_t payload_capacity = total_size - payload_offset;
    static_assert(sizeof(ProcessSyncHeader) <= payload_offset);

    explicit ProcessSync(const std::string& name)
        : buffer_(AudioShmBuffer::Config{.name = name,
                                         .size = total_size,
                                         .input_offsets = {},
                                         .output_offsets = {}}) {}

    uint8_t* payload() noexcept { return buffer_.data() + payload_offset; }

    AudioShmBuffer buffer_;
};

#ifndef YABRIDGE_PE_HOST
inline std::optional<ProcessSync> ProcessSync::open_if_ready(
    const std::string& name) {
    if (disabled()) {
        return std::nullopt;
    }

    // `AudioShmBuffer` would create the object if it doesn't exist yet
    const int fd = shm_open(name.c_str(), O_RDWR, 0600);
    if (fd == -1) {
        return std::nullopt;
    }
    close(fd);

    try {
        ProcessSync sync(name);
        ProcessSyncHeader& header = sync.header();
        if (__atomic_load_n(&header.magic, __ATOMIC_ACQUIRE) !=
                expected_magic ||
            header.version != expected_version ||
            __atomic_load_n(&header.host_ready, __ATOMIC_ACQUIRE) != 1) {
            return std::nullopt;
        }

        return sync;
    } catch (const std::system_error&) {
        return std::nullopt;
    }
}

/**
 * Wait on a futex shared with the Wine plugin host for at most `timeout_ms`
 * milliseconds. Returns 0 or the negated `errno`.
 */
inline long futex_wait_shared(uint32_t* address,
                              uint32_t expected,
                              uint32_t timeout_ms) noexcept {
    const timespec timeout{
        .tv_sec = static_cast<time_t>(timeout_ms / 1000),
        .tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000)};
    const long result =
        syscall(SYS_futex, address, FUTEX_WAIT, expected, &timeout, nullptr, 0);

    return result == -1 ? -errno : result;
}

/**
 * Wake up the waiter on a futex shared with the Wine plugin host.
 */
inline void futex_wake_shared(uint32_t* address) noexcept {
    syscall(SYS_futex, address, FUTEX_WAKE, 1, nullptr, nullptr, 0);
}
#endif
