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

#include <cstdint>
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
 * Whether `int 0x80` system calls work on this system. Checked once using
 * `getpid()`. Always false when `YABRIDGE_NO_LINUX_SYSCALLS=1` is set.
 */
bool linux_syscalls_available() noexcept;

/**
 * Perform a 32-bit Linux system call using `int 0x80`. This also works from
 * 32-bit code running under Wine's WoW64 mode on a 64-bit kernel, as long as
 * the kernel has 32-bit (IA32) emulation enabled. When that's not the case,
 * the instruction would fault. We install a vectored exception handler to
 * catch that, and this function will then return a nullopt.
 *
 * Wine on x86_64 traps system calls made from Windows code using syscall user
 * dispatch, so this temporarily disables that for the duration of the system
 * call. Because a signal handler could re-enable trapping, and the kernel
 * restarts interrupted system calls by executing the instruction again, this
 * should only be used for short, non-blocking system calls.
 *
 * This is only used for things Wine does not provide an API for, like setting
 * realtime scheduling priorities. Returns the raw syscall result (a negative
 * value indicates an `-errno`). Setting `YABRIDGE_NO_LINUX_SYSCALLS=1`
 * disables this.
 */
std::optional<long> linux_syscall(long number,
                                  long arg1 = 0,
                                  long arg2 = 0,
                                  long arg3 = 0) noexcept;

/**
 * Wait on a Linux futex shared with another process (so not a
 * `FUTEX_PRIVATE_FLAG` futex) using a raw system call, for at most
 * `timeout_ms` milliseconds. Returns the raw result, so 0 when woken up, and
 * `-EAGAIN` if `*address` no longer contained `expected`, `-ETIMEDOUT` on
 * timeout, or `-EINTR` when interrupted. Returns a nullopt if Linux system
 * calls cannot be made.
 *
 * Unlike `linux_syscall()`, this is a blocking system call, and it's safe
 * because of the timeout. A futex wait with a timeout returns
 * `-ERESTART_RESTARTBLOCK` when it gets interrupted by a signal. When a signal
 * handler ran, which is when Wine's handlers would have re-enabled syscall
 * trapping, the kernel turns that into `-EINTR` instead of restarting the
 * system call. When no handler ran, the kernel restarts the system call, but
 * then nothing has touched the syscall dispatch selector. A futex wait without
 * a timeout would instead return `-ERESTARTSYS`, which the kernel restarts
 * after running Wine's `SA_RESTART` signal handlers, at which point the restart
 * would be trapped by Wine. Callers should simply wait again on `-EINTR`.
 */
std::optional<long> linux_futex_wait_timeout(volatile uint32_t* address,
                                             uint32_t expected,
                                             uint32_t timeout_ms) noexcept;

/**
 * Wake up to `count` waiters on a Linux futex shared with another process.
 * This is a short, non-blocking system call. Returns a nullopt if Linux system
 * calls cannot be made.
 */
std::optional<long> linux_futex_wake(volatile uint32_t* address,
                                     int count) noexcept;

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
