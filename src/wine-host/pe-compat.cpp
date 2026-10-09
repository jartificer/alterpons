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

#include "pe-compat.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#ifndef __i386__
#error "The PE host is only supported as a 32-bit (i686) executable"
#endif

namespace {

// 32-bit x86 Linux system call numbers, from
// `arch/x86/entry/syscalls/syscall_32.tbl`
constexpr long sys_getpid = 20;
constexpr long sys_sched_getparam = 155;
constexpr long sys_sched_setscheduler = 156;
constexpr long sys_sched_getscheduler = 157;

constexpr long linux_sched_other = 0;
constexpr long linux_sched_fifo = 1;
constexpr long linux_enosys = 38;

/**
 * The `int 0x80` instruction used by `linux_syscall()`. The exception handler
 * uses this to make sure it only handles faults caused by our system calls.
 */
extern "C" const unsigned char yabridge_int80_instruction[];

/**
 * Perform the actual system call. This is defined in assembly below so we know
 * the exact address of the `int 0x80` instruction.
 */
extern "C" long __attribute__((cdecl)) yabridge_raw_int80(long number,
                                                          long arg1,
                                                          long arg2,
                                                          long arg3);

// cdecl: the arguments are on the stack, and `%ebx` must be preserved
asm(R"(
    .text
    .globl _yabridge_raw_int80
    .globl _yabridge_int80_instruction
_yabridge_raw_int80:
    pushl %ebx
    movl 8(%esp), %eax
    movl 12(%esp), %ebx
    movl 16(%esp), %ecx
    movl 20(%esp), %edx
_yabridge_int80_instruction:
    int $0x80
    popl %ebx
    ret
)");

/**
 * When the kernel does not support 32-bit system calls, then `int 0x80` will
 * result in a general protection fault, which Wine turns into an exception.
 * We'll skip over the instruction and return `-ENOSYS` instead.
 */
LONG CALLBACK int80_exception_handler(EXCEPTION_POINTERS* exception_info) {
    CONTEXT* context = exception_info->ContextRecord;
    if (context->Eip ==
        reinterpret_cast<DWORD>(yabridge_int80_instruction)) {
        context->Eip += 2;
        context->Eax = static_cast<DWORD>(-linux_enosys);

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/**
 * Whether `int 0x80` system calls work on this system. Checked once using
 * `getpid()`.
 */
bool linux_syscalls_available() noexcept {
    static std::once_flag once;
    static bool available = false;

    std::call_once(once, []() {
        AddVectoredExceptionHandler(1, int80_exception_handler);

        const long pid = yabridge_raw_int80(sys_getpid, 0, 0, 0);
        available = pid > 0;
    });

    return available;
}

template <typename F>
F get_kernel32_function(const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name)));
}

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) {
        return std::wstring();
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, str.data(),
                                         static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                        result.data(), size);

    return result;
}

std::string wide_to_utf8(const WCHAR* str) {
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, str, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }

    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str, -1, result.data(), size, nullptr,
                        nullptr);

    return result;
}

}  // namespace

WCHAR* wine_get_dos_file_name(const char* unix_path) {
    using wine_get_dos_file_name_t = WCHAR*(CDECL*)(const char*);
    static const auto wine_function =
        get_kernel32_function<wine_get_dos_file_name_t>(
            "wine_get_dos_file_name");

    if (unix_path && unix_path[0] == '/' && wine_function) {
        return wine_function(unix_path);
    }

    // This is either not a Unix path or we're not running under Wine, so we'll
    // just return a copy of the path
    const std::wstring wide_path = utf8_to_wide(unix_path ? unix_path : "");
    WCHAR* result = static_cast<WCHAR*>(HeapAlloc(
        GetProcessHeap(), 0, (wide_path.size() + 1) * sizeof(WCHAR)));
    if (result) {
        std::memcpy(result, wide_path.c_str(),
                    (wide_path.size() + 1) * sizeof(WCHAR));
    }

    return result;
}

char* wine_get_unix_file_name(const WCHAR* dos_path) {
    using wine_get_unix_file_name_t = char*(CDECL*)(const WCHAR*);
    static const auto wine_function =
        get_kernel32_function<wine_get_unix_file_name_t>(
            "wine_get_unix_file_name");

    return wine_function ? wine_function(dos_path) : nullptr;
}

std::string unix_path_to_dos_path(const std::string& unix_path) {
    if (!unix_path.starts_with('/')) {
        return unix_path;
    }

    WCHAR* dos_path = wine_get_dos_file_name(unix_path.c_str());
    if (!dos_path) {
        return unix_path;
    }

    std::string result = wide_to_utf8(dos_path);
    HeapFree(GetProcessHeap(), 0, dos_path);

    // Wine returns `\\?\unix\...` paths for files that aren't reachable
    // through any drive letter. Those work fine with the Windows API, but some
    // parts of the C runtime don't like them, so we'll prefer the drive letter
    // version whenever possible (which is the default since Wine maps `Z:` to
    // `/`).
    return result.empty() ? unix_path : result;
}

bool linux_pid_running(int pid) {
    // We can't distinguish between running processes and zombies this way
    // like we do in `pid_running()`, but the native plugin host is never going
    // to be a zombie while yabridge is still running anyways.
    const std::wstring proc_path =
        utf8_to_wide(unix_path_to_dos_path("/proc/" + std::to_string(pid)));
    const DWORD attributes = GetFileAttributesW(proc_path.c_str());

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::optional<long> linux_syscall(long number,
                                  long arg1,
                                  long arg2,
                                  long arg3) noexcept {
    if (!linux_syscalls_available()) {
        return std::nullopt;
    }

    return yabridge_raw_int80(number, arg1, arg2, arg3);
}

bool linux_set_thread_realtime(bool sched_fifo, int priority) noexcept {
    // This matches `struct sched_param` from the kernel
    struct {
        int sched_priority;
    } params{.sched_priority = sched_fifo ? priority : 0};

    // A PID of 0 means the calling thread
    const std::optional<long> result = linux_syscall(
        sys_sched_setscheduler, 0,
        sched_fifo ? linux_sched_fifo : linux_sched_other,
        reinterpret_cast<long>(&params));

    return result && *result == 0;
}

std::optional<int> linux_get_thread_realtime_priority() noexcept {
    const std::optional<long> policy =
        linux_syscall(sys_sched_getscheduler, 0);
    if (!policy || *policy != linux_sched_fifo) {
        return std::nullopt;
    }

    struct {
        int sched_priority;
    } params{};
    const std::optional<long> result =
        linux_syscall(sys_sched_getparam, 0, reinterpret_cast<long>(&params));
    if (result && *result == 0 && params.sched_priority > 0) {
        return params.sched_priority;
    } else {
        return std::nullopt;
    }
}
