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
#include <cstdlib>
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
 * The `int 0x80` instructions used by `linux_syscall()`. The exception handler
 * uses these to make sure it only handles faults caused by our system calls.
 */
extern "C" const unsigned char yabridge_int80_instruction[];
extern "C" const unsigned char yabridge_int80_dispatch_instruction[];

/**
 * Perform the actual system call. These are defined in assembly below so we
 * know the exact address of the `int 0x80` instruction.
 */
extern "C" long __attribute__((cdecl)) yabridge_raw_int80(long number,
                                                          long arg1,
                                                          long arg2,
                                                          long arg3);

/**
 * The same as `yabridge_raw_int80()`, but this sets the byte at `selector` to
 * 0 (`SYSCALL_DISPATCH_FILTER_ALLOW`) right before the system call, and back
 * to 1 (`SYSCALL_DISPATCH_FILTER_BLOCK`) right after it. See
 * `get_syscall_dispatch_selector()`.
 */
extern "C" long __attribute__((cdecl))
yabridge_raw_int80_dispatch(long number,
                            long arg1,
                            long arg2,
                            long arg3,
                            volatile unsigned char* selector);

// cdecl: the arguments are on the stack, and `%ebx` and `%esi` must be
// preserved
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

    .globl _yabridge_raw_int80_dispatch
    .globl _yabridge_int80_dispatch_instruction
_yabridge_raw_int80_dispatch:
    pushl %ebx
    pushl %esi
    movl 12(%esp), %eax
    movl 16(%esp), %ebx
    movl 20(%esp), %ecx
    movl 24(%esp), %edx
    movl 28(%esp), %esi
    movb $0, (%esi)
_yabridge_int80_dispatch_instruction:
    int $0x80
    movb $1, (%esi)
    popl %esi
    popl %ebx
    ret
)");

/**
 * Wine on x86_64 Linux uses the kernel's syscall user dispatch feature to trap
 * system calls made directly from Windows code, which it then handles as
 * Windows NT system calls. In Wine's (new) WoW64 mode this also applies to
 * our 32-bit `int 0x80` system calls, which would then crash the process.
 * Wine turns this trapping on and off using a per-thread selector byte in its
 * private part of the 64-bit TEB. For 32-bit code, the 64-bit TEB can be found
 * using the `WowTebOffset` field in the 32-bit TEB.
 *
 * Returns a pointer to the selector if it's currently set to 1
 * (`SYSCALL_DISPATCH_FILTER_BLOCK`), meaning syscall user dispatch is active.
 * Returns a null pointer if there is no 64-bit TEB or if the selector is 0,
 * meaning that system calls can be made as is (old style WoW64, or a kernel
 * without syscall user dispatch). Returns a nullopt if the selector has some
 * unexpected value, in which case we should not make any system calls.
 */
std::optional<volatile unsigned char*> get_syscall_dispatch_selector() {
    // The offset of `WowTebOffset` in the 32-bit TEB, and the offset of
    // `amd64_thread_data()->syscall_dispatch` in Wine's 64-bit TEB. The
    // latter is checked using a `C_ASSERT()` in Wine's
    // `dlls/ntdll/unix/signal_x86_64.c`.
    constexpr size_t wow_teb_offset_offset = 0xfdc;
    constexpr size_t syscall_dispatch_offset = 0x340;

    // This is `NtCurrentTeb()`, but MinGW's version of that triggers a bogus
    // `-Warray-bounds` warning
    const unsigned char* teb32;
    asm("movl %%fs:0x18, %0" : "=r"(teb32));
    LONG wow_teb_offset;
    std::memcpy(&wow_teb_offset, teb32 + wow_teb_offset_offset,
                sizeof(wow_teb_offset));
    if (wow_teb_offset == 0) {
        return nullptr;
    }

    auto* selector = const_cast<volatile unsigned char*>(
        teb32 + wow_teb_offset + syscall_dispatch_offset);
    switch (*selector) {
        case 0:
            return nullptr;
        case 1:
            return selector;
        default:
            return std::nullopt;
    }
}

/**
 * Perform a system call, taking Wine's syscall user dispatch into account.
 * Returns a nullopt if we can't safely make the system call.
 */
std::optional<long> do_syscall(long number,
                               long arg1,
                               long arg2,
                               long arg3) noexcept {
    const std::optional<volatile unsigned char*> selector =
        get_syscall_dispatch_selector();
    if (!selector) {
        return std::nullopt;
    } else if (*selector) {
        return yabridge_raw_int80_dispatch(number, arg1, arg2, arg3, *selector);
    } else {
        return yabridge_raw_int80(number, arg1, arg2, arg3);
    }
}

/**
 * When the kernel does not support 32-bit system calls, then `int 0x80` will
 * result in a general protection fault, which Wine turns into an exception.
 * We'll skip over the instruction and return `-ENOSYS` instead.
 */
LONG CALLBACK int80_exception_handler(EXCEPTION_POINTERS* exception_info) {
    CONTEXT* context = exception_info->ContextRecord;
    if (context->Eip == reinterpret_cast<DWORD>(yabridge_int80_instruction) ||
        context->Eip ==
            reinterpret_cast<DWORD>(yabridge_int80_dispatch_instruction)) {
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
        // This can be used to opt out of this if it ever causes problems
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (const char* disable = getenv("YABRIDGE_NO_LINUX_SYSCALLS");
            disable && disable == std::string("1")) {
            return;
        }

        AddVectoredExceptionHandler(1, int80_exception_handler);

        const std::optional<long> pid = do_syscall(sys_getpid, 0, 0, 0);
        available = pid && *pid > 0;
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

    return do_syscall(number, arg1, arg2, arg3);
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
