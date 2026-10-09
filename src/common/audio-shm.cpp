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

#include "audio-shm.h"

#include <iostream>

#include "logging/common.h"

using namespace std::literals::string_literals;

#ifdef YABRIDGE_PE_HOST
#include "../wine-host/pe-compat.h"

namespace {

/**
 * The path to the shared memory object in `/dev/shm`, as a wide DOS path. This
 * matches what glibc's `shm_open()` does on the native side.
 */
std::wstring shm_path(const std::string& name) {
    std::string unix_path = "/dev/shm/";
    unix_path += name.starts_with('/') ? name.substr(1) : name;

    const std::string dos_path = unix_path_to_dos_path(unix_path);
    const int size = MultiByteToWideChar(CP_UTF8, 0, dos_path.c_str(), -1,
                                         nullptr, 0);
    std::wstring result(size > 0 ? size - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dos_path.c_str(), -1, result.data(), size);

    return result;
}

}  // namespace

AudioShmBuffer::AudioShmBuffer(const Config& config)
    : config_(config),
      shm_file_(CreateFileW(shm_path(config.name).c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr,
                            OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr)) {
    if (shm_file_ == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            std::error_code(static_cast<int>(GetLastError()),
                            std::system_category()),
            "Could not open shared memory object " + config_.name);
    }

    setup_mapping();
}

AudioShmBuffer::~AudioShmBuffer() noexcept {
    // If either side drops this object then the buffer should always be
    // removed, so we'll do it on both sides to reduce the chance that we leak
    // shared memory
    if (!is_moved_) {
        if (shm_bytes_) {
            UnmapViewOfFile(shm_bytes_);
        }
        if (shm_mapping_) {
            CloseHandle(shm_mapping_);
        }
        CloseHandle(shm_file_);
        DeleteFileW(shm_path(config_.name).c_str());
    }
}
#else
AudioShmBuffer::AudioShmBuffer(const Config& config)
    : config_(config),
      shm_fd_(shm_open(config.name.c_str(), O_RDWR | O_CREAT, 0600)) {
    if (shm_fd_ == -1) {
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "Could not create shared memory object " + config_.name);
    }

    setup_mapping();
}

AudioShmBuffer::~AudioShmBuffer() noexcept {
    // If either side drops this object then the buffer should always be
    // removed, so we'll do it on both sides to reduce the chance that we leak
    // shared memory
    if (!is_moved_) {
        munmap(shm_bytes_, config_.size);
        close(shm_fd_);
        shm_unlink(config_.name.c_str());
    }
}
#endif  // YABRIDGE_PE_HOST

AudioShmBuffer::AudioShmBuffer(AudioShmBuffer&& o) noexcept
    : config_(std::move(o.config_)),
#ifdef YABRIDGE_PE_HOST
      shm_file_(std::move(o.shm_file_)),
      shm_mapping_(std::move(o.shm_mapping_)),
#else
      shm_fd_(std::move(o.shm_fd_)),
#endif
      shm_bytes_(std::move(o.shm_bytes_)),
      shm_size_(std::move(o.shm_size_)) {
    o.is_moved_ = true;
}

AudioShmBuffer& AudioShmBuffer::operator=(AudioShmBuffer&& o) noexcept {
    config_ = std::move(o.config_);
#ifdef YABRIDGE_PE_HOST
    shm_file_ = std::move(o.shm_file_);
    shm_mapping_ = std::move(o.shm_mapping_);
#else
    shm_fd_ = std::move(o.shm_fd_);
#endif
    shm_bytes_ = std::move(o.shm_bytes_);
    shm_size_ = std::move(o.shm_size_);
    o.is_moved_ = true;

    return *this;
}

void AudioShmBuffer::resize(const Config& new_config) {
    if (new_config.name != config_.name) {
        throw std::invalid_argument("Expected buffer configuration for \"" +
                                    config_.name + "\", got \"" +
                                    new_config.name + "\"");
    }

    config_ = new_config;
    setup_mapping();
}

#ifdef YABRIDGE_PE_HOST
void AudioShmBuffer::setup_mapping() {
    if (shm_bytes_) {
        UnmapViewOfFile(shm_bytes_);
        shm_bytes_ = nullptr;
    }
    if (shm_mapping_) {
        CloseHandle(shm_mapping_);
        shm_mapping_ = nullptr;
    }

    // Mapping a zero sized file is not allowed
    if (config_.size > 0) {
        // The native side resizes the file using `ftruncate()`. Creating a
        // mapping that's larger than the file will also grow the file, so we
        // don't need to do anything special here.
        const uint64_t size = config_.size;
        shm_mapping_ = CreateFileMappingW(
            shm_file_, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
            static_cast<DWORD>(size & 0xffffffff), nullptr);
        if (!shm_mapping_) {
            throw std::system_error(
                std::error_code(static_cast<int>(GetLastError()),
                                std::system_category()),
                "Could not create a file mapping for " + config_.name);
        }

        shm_bytes_ = static_cast<uint8_t*>(
            MapViewOfFile(shm_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, size));
        if (!shm_bytes_) {
            throw std::system_error(
                std::error_code(static_cast<int>(GetLastError()),
                                std::system_category()),
                "Could not map shared memory");
        }

        // This is the Win32 equivalent of `MAP_LOCKED`. Like on the native
        // side this is optional, and failing to lock the memory is not fatal.
        VirtualLock(shm_bytes_, size);
    }

    shm_size_ = config_.size;
}
#else
void AudioShmBuffer::setup_mapping() {
    // Apparently you get a `Resource temporarily unavailable` when calling
    // `ftruncate()` with a size of 0 on shared memory
    if (config_.size > 0) {
        // I don't think this can fail
        assert(ftruncate(shm_fd_, config_.size) == 0);

        // But this can, if the user does not have permissions to use (enough)
        // locked emmory, we'll try it without locking memory and show a big
        // obnoxious warning and try again without locking the memory.
        uint8_t* old_shm_bytes = shm_bytes_;
        shm_bytes_ = static_cast<uint8_t*>(
            old_shm_bytes
                ? mremap(old_shm_bytes, shm_size_, config_.size, MREMAP_MAYMOVE)
                : mmap(nullptr, config_.size, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_LOCKED, shm_fd_, 0));
        if (shm_bytes_ == MAP_FAILED) {
            Logger logger = Logger::create_exception_logger();

            logger.log("");
            logger.log("ERROR: Could not map shared memory. This means that");
            logger.log("       your user's memory locking limit has been");
            logger.log("       reached. Check your distro's documentation or");
            logger.log("       wiki for instructions on how to set up");
            logger.log("       realtime privileges and memlock limits.");
            logger.log("");

            // Growing into a size that we cannot lock sounds like a super rare
            // edge case, but let's handle it anyways
            if (old_shm_bytes) {
                assert(munmap(old_shm_bytes, shm_size_) == 0);
            }
            shm_bytes_ = static_cast<uint8_t*>(mmap(nullptr, config_.size,
                                                    PROT_READ | PROT_WRITE,
                                                    MAP_SHARED, shm_fd_, 0));
            if (shm_bytes_ == MAP_FAILED) {
                throw std::system_error(
                    std::error_code(errno, std::system_category()),
                    "Could not map shared memory");
            }
        }
    }

    shm_size_ = config_.size;
}
#endif  // YABRIDGE_PE_HOST
