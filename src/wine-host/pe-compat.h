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

// Helpers for the 32-bit Wine plugin host, which is built as a regular Windows
// (PE) executable using MinGW instead of as a Winelib application. See
// `SocketTransport` in `src/common/communication/transport.h` for why this is
// needed. These functions provide replacements for the bits of the Linux and
// Winelib APIs the Winelib host uses.

#ifndef YABRIDGE_PE_HOST
#error "This header should only be included in the PE (MinGW) host build"
#endif

#include <optional>
#include <string>

#include <windows.h>

/**
 * Wine's `wine_get_dos_file_name()`, which is exported from Wine's
 * `kernel32.dll`. This is resolved at runtime since MinGW's import libraries
 * don't know about it. Returns a buffer allocated on the process heap that
 * should be freed using `HeapFree()`, or a null pointer if the conversion
 * failed. As a convenience, paths that are not absolute Unix paths are returned
 * as is.
 */
WCHAR* wine_get_dos_file_name(const char* unix_path);

/**
 * Wine's `wine_get_unix_file_name()`, see above. Returns a buffer allocated on
 * the process heap that should be freed using `HeapFree()`, or a null pointer
 * if the conversion failed.
 */
char* wine_get_unix_file_name(const WCHAR* dos_path);

/**
 * Convert a Unix path to a DOS path (encoded as UTF-8) the Windows API and
 * `ghc::filesystem` can work with. Returns the original string if the path
 * could not be converted, or if it was not a Unix path to begin with.
 */
std::string unix_path_to_dos_path(const std::string& unix_path);

/**
 * Check whether a Linux process with the specified process ID is currently
 * running, by checking whether `/proc/<pid>/exe` exists.
 */
bool linux_pid_running(int pid);

/**
 * Perform a 32-bit Linux system call using `int 0x80`. This also works from
 * 32-bit code running under Wine's WoW64 mode on a 64-bit kernel, as long as
 * the kernel has 32-bit (IA32) emulation enabled. When that's not the case,
 * the instruction would fault. We install a vectored exception handler to
 * catch that, and this function will then return a nullopt.
 *
 * This is only used for things Wine does not provide an API for, like setting
 * realtime scheduling priorities. Returns the raw syscall result (a negative
 * value indicates an `-errno`).
 */
std::optional<long> linux_syscall(long number,
                                  long arg1 = 0,
                                  long arg2 = 0,
                                  long arg3 = 0) noexcept;

/**
 * Set or reset `SCHED_FIFO` scheduling for the calling thread through
 * `linux_syscall()`. Returns `false` if this failed.
 */
bool linux_set_thread_realtime(bool sched_fifo, int priority) noexcept;

/**
 * Get the calling thread's `SCHED_FIFO` priority through `linux_syscall()`, if
 * it has one.
 */
std::optional<int> linux_get_thread_realtime_priority() noexcept;
