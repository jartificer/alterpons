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

#include "group.h"

#ifdef YABRIDGE_PE_HOST
#include <fcntl.h>
#include <io.h>
#else
#include "../use-linux-asio.h"

#include <unistd.h>
#endif
#include <regex>

#include "../../common/communication/common.h"
#ifdef WITH_CLAP
#include "clap.h"
#endif
#include "vst2.h"
#ifdef WITH_VST3
#include "vst3.h"
#endif

// FIXME: `std::filesystem` is broken in wineg++, at least under Wine 5.8. Any
//        path operation will thrown an encoding related error
namespace fs = ghc::filesystem;

using namespace std::literals::chrono_literals;

/**
 * Listen on the specified endpoint if no process is already listening there,
 * otherwise throw. This is needed to handle these three situations:
 *
 * 1. The endpoint does not already exist, and we can simply create an endpoint.
 * 2. The endpoint already exists but it is stale and no process is currently
 *    listening. In this case we can remove the file and start listening.
 * 3. The endpoint already exists and another process is currently listening on
 *    it. In this situation we will throw immediately and we'll terminate this
 *    process.
 *
 * If anyone knows a better way to handle this, please let me know!
 *
 * @throw std::runtime_error If another process is already listening on the
 *        endpoint.
 */
SocketAcceptor create_acceptor_if_inactive(asio::io_context& io_context,
                                           const SocketEndpoint& endpoint);

/**
 * Create a logger prefix containing the group name based on the socket path.
 */
std::string create_logger_prefix(const fs::path& socket_path);

#ifdef YABRIDGE_PE_HOST
StdIoCapture::StdIoCapture(int file_descriptor,
                           Logger& logger,
                           std::string prefix)
    : target_fd_(file_descriptor),
      std_handle_id_(file_descriptor == STDERR_FILENO ? STD_ERROR_HANDLE
                                                      : STD_OUTPUT_HANDLE),
      original_std_handle_(GetStdHandle(std_handle_id_)),
      original_fd_copy_(_dup(file_descriptor)) {
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle_, &write_handle, nullptr, 0)) {
        std::cerr << "Could not create pipe" << std::endl;
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category());
    }

    // Flush anything that's still buffered before swapping out the stream
    fflush(file_descriptor == STDERR_FILENO ? stderr : stdout);

    // This takes ownership of `write_handle`. `_dup2()` duplicates the
    // underlying handle, so we can close the temporary file descriptor
    // afterwards.
    const int pipe_fd =
        _open_osfhandle(reinterpret_cast<intptr_t>(write_handle), _O_WRONLY);
    _dup2(pipe_fd, target_fd_);
    _close(pipe_fd);
    SetStdHandle(std_handle_id_,
                 reinterpret_cast<HANDLE>(_get_osfhandle(target_fd_)));

    reader_thread_ = Win32Thread([this, &logger, prefix = std::move(prefix)]() {
        set_current_thread_name("group-stdio");

        // Plugins may keep their own copies of the pipe's write handle, so we
        // can't rely on ever reaching the end of the stream. Instead we'll
        // poll the pipe so we can stop reading when this object gets dropped.
        std::string line_buffer;
        std::array<char, 4096> read_buffer{};
        while (!stop_reading_) {
            DWORD bytes_available = 0;
            if (!PeekNamedPipe(read_handle_, nullptr, 0, nullptr,
                               &bytes_available, nullptr)) {
                break;
            }
            if (bytes_available == 0) {
                Sleep(10);
                continue;
            }

            DWORD bytes_read = 0;
            if (!ReadFile(read_handle_, read_buffer.data(),
                          std::min<DWORD>(bytes_available, read_buffer.size()),
                          &bytes_read, nullptr)) {
                break;
            }

            line_buffer.append(read_buffer.data(), bytes_read);
            size_t newline_pos;
            while ((newline_pos = line_buffer.find('\n')) !=
                   std::string::npos) {
                std::string line = line_buffer.substr(0, newline_pos);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                logger.log(prefix + line);
                line_buffer.erase(0, newline_pos + 1);
            }
        }
    });
}

StdIoCapture::~StdIoCapture() noexcept {
    fflush(target_fd_ == STDERR_FILENO ? stderr : stdout);

    // Restore the original handles. This also closes our copy of the pipe's
    // write handle.
    _dup2(original_fd_copy_, target_fd_);
    _close(original_fd_copy_);
    SetStdHandle(std_handle_id_, original_std_handle_);

    stop_reading_ = true;
    reader_thread_ = Win32Thread();
    CloseHandle(read_handle_);
}
#else
StdIoCapture::StdIoCapture(asio::io_context& io_context, int file_descriptor)
    : pipe_(io_context),
      target_fd_(file_descriptor),
      original_fd_copy_(dup(file_descriptor)) {
    // We'll use the second element of these two file descriptors to reopen
    // `file_descriptor`, and the first one to read the captured contents from
    if (pipe(pipe_fd_) != 0) {
        std::cerr << "Could not create pipe" << std::endl;
        throw std::system_error(errno, std::system_category());
    }

    // We've already created a copy of the original file descriptor, so we can
    // reopen it using the newly created pipe
    dup2(pipe_fd_[1], target_fd_);
    close(pipe_fd_[1]);

    pipe_.assign(pipe_fd_[0]);
}

StdIoCapture::~StdIoCapture() noexcept {
    // Restore the original file descriptor and close the pipe. The other wend
    // was already closed in the constructor.
    dup2(original_fd_copy_, target_fd_);
    close(original_fd_copy_);
    close(pipe_fd_[0]);
}
#endif  // YABRIDGE_PE_HOST

GroupBridge::GroupBridge(ghc::filesystem::path group_socket_path)
    : logger_(Logger::create_from_environment(
          create_logger_prefix(group_socket_path))),
      main_context_(),
      stdio_context_(),
#ifdef YABRIDGE_PE_HOST
      stdout_redirect_(STDOUT_FILENO, logger_, "[STDOUT] "),
      stderr_redirect_(STDERR_FILENO, logger_, "[STDERR] "),
#else
      stdout_redirect_(stdio_context_, STDOUT_FILENO),
      stderr_redirect_(stdio_context_, STDERR_FILENO),
#endif
      group_socket_endpoint_(group_socket_path.string()),
      group_socket_acceptor_(
          create_acceptor_if_inactive(main_context_.context_,
                                      group_socket_endpoint_)),
      shutdown_timer_(main_context_.context_) {
#ifndef YABRIDGE_PE_HOST
    // Write this process's original STDOUT and STDERR streams to the logger
    logger_.async_log_pipe_lines(stdout_redirect_.pipe_, stdout_buffer_,
                                 "[STDOUT] ");
    logger_.async_log_pipe_lines(stderr_redirect_.pipe_, stderr_buffer_,
                                 "[STDERR] ");

    stdio_handler_ = Win32Thread([&]() {
        set_current_thread_name("group-stdio");

        stdio_context_.run();
    });
#endif
}

GroupBridge::~GroupBridge() noexcept {
    // Our fancy `Vst2Sockets` and `Vst3Sockets` clean up after themselves, but
    // here we need to do it manually
    // TODO: Encapsulate this, destructors are evil
    group_socket_endpoint_.remove();

    stdio_context_.stop();
}

bool GroupBridge::is_event_loop_inhibited() noexcept {
    std::lock_guard lock(active_plugins_mutex_);

    for (auto& [parameters, value] : active_plugins_) {
        auto& [thread, bridge] = value;
        if (bridge->inhibits_event_loop()) {
            return true;
        }
    }

    return false;
}

void GroupBridge::handle_plugin_run(size_t plugin_id, HostBridge* bridge) {
    // Blocks this thread until the plugin shuts down
    bridge->run();

    // FIXME: This results in a spurious compiler warning if you inline
    //        `plugin_path`:
    //        https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105329
    const std::string plugin_path = bridge->plugin_path_.string();
    logger_.log("'" + plugin_path + "' has exited");

    // After the plugin has exited we'll remove this thread's plugin from the
    // active plugins. This is done within the IO context because the call to
    // `FreeLibrary()` has to be done from the main thread, or else we'll
    // potentially corrupt our heap. This way we can also properly join the
    // thread again. If no active plugins remain, then we'll terminate the
    // process.
    main_context_.schedule_task([this, plugin_id]() {
        std::lock_guard lock(active_plugins_mutex_);

        // The join is implicit because we're using Win32Thread (which mimics
        // std::jthread)
        active_plugins_.erase(plugin_id);
    });

    // Defer actually shutting down the process to allow for fast plugin
    // scanning by allowing plugins to reuse the same group host process
    maybe_schedule_shutdown(4s);
}

void GroupBridge::handle_incoming_connections() {
    accept_requests();
    async_handle_events();

    // If we don't get a request to host a plugin within five seconds, we'll
    // shut the process down again.
    maybe_schedule_shutdown(5s);

    logger_.log(
        "Group host is up and running, now accepting incoming connections");
    main_context_.run();
}

void GroupBridge::accept_requests() {
    group_socket_acceptor_.async_accept(
        [&](const std::error_code& error, Socket socket) {
            std::lock_guard lock(active_plugins_mutex_);

            // Stop the whole process when the socket gets closed unexpectedly
            if (error) {
                logger_.log("Error while listening for incoming connections:");
                logger_.log(error.message());

                main_context_.stop();
            }

            // Read the parameters, and then host the plugin in this process
            // just like if we would be hosting the plugin individually through
            // `yabridge-hsot.exe`. We will reply with this process's PID so the
            // yabridge plugin will be able to tell if the plugin has caused
            // this process to crash during its initialization to prevent
            // waiting indefinitely on the sockets to be connected to.
            // With TCP sockets the other side first has to authenticate
            // itself
            try {
                group_socket_endpoint_.finish_accept(socket);
            } catch (const std::system_error& error) {
                logger_.log("Dropping connection: " +
                            std::string(error.what()));
                accept_requests();
                return;
            }

            auto request = read_object<HostRequest>(socket);
            write_object(socket, HostResponse{.pid = get_current_pid()});

            // The native plugin sends us Linux paths
            request.plugin_path = to_host_path(request.plugin_path);
            request.endpoint_base_dir = to_host_path(request.endpoint_base_dir);

            // The plugin has to be initiated on the IO context's thread because
            // this has to be done on the same thread that's handling messages,
            // and all window messages have to be handled from the same thread.
            logger_.log("Received request to host " +
                        plugin_type_to_string(request.plugin_type) +
                        " plugin at '" + request.plugin_path +
                        "' using socket endpoint base directory '" +
                        request.endpoint_base_dir + "'");
            try {
                // Cancel the (initial) shutdown timer, since the plugin may
                // take longer to initialize if it is new
                shutdown_timer_.cancel();

                std::unique_ptr<HostBridge> bridge = nullptr;
                switch (request.plugin_type) {
                    case PluginType::clap:
#ifdef WITH_CLAP
                        bridge = std::make_unique<ClapBridge>(
                            main_context_, request.plugin_path,
                            request.endpoint_base_dir, request.parent_pid);
#else
                        throw std::runtime_error(
                            "This version of yabridge has not been compiled "
                            "with CLAP support");
#endif
                        break;
                    case PluginType::vst2:
                        bridge = std::make_unique<Vst2Bridge>(
                            main_context_, request.plugin_path,
                            request.endpoint_base_dir, request.parent_pid);
                        break;
                    case PluginType::vst3:
#ifdef WITH_VST3
                        bridge = std::make_unique<Vst3Bridge>(
                            main_context_, request.plugin_path,
                            request.endpoint_base_dir, request.parent_pid);
#else
                        throw std::runtime_error(
                            "This version of yabridge has not been compiled "
                            "with VST3 support");
#endif
                        break;
                    case PluginType::unknown:
                        throw std::runtime_error(
                            "Invalid plugin host request received, how did you "
                            "even manage to do this?");
                        break;
                }

                logger_.log("Finished initializing '" + request.plugin_path +
                            "'");

                // Start listening for dispatcher events sent to the plugin's
                // socket on another thread. Parts of the actual event handling
                // will still be posted to this IO context so that any events
                // that potentially interact with the Win32 message loop are
                // handled from the main thread. We also pass a raw pointer to
                // the plugin so we don't have to immediately look the instance
                // up in the map again, as this would require us to immediately
                // lock the map again. This could otherwise result in a deadlock
                // when using the Spitfire plugins, as they will block the
                // message loop until `effOpen()` has been called and thus
                // prevent this lock from happening.
                const size_t plugin_id = next_plugin_id_.fetch_add(1);
                active_plugins_[plugin_id] = std::pair(
                    Win32Thread([this, plugin_id, plugin_ptr = bridge.get()]() {
                        const std::string thread_name =
                            "worker-" + std::to_string(plugin_id);
                        set_current_thread_name(thread_name.c_str());

                        handle_plugin_run(plugin_id, plugin_ptr);
                    }),
                    std::move(bridge));
            } catch (const std::exception& error) {
                logger_.log("Error while initializing '" + request.plugin_path +
                            "':");
                logger_.log(error.what());

                maybe_schedule_shutdown(5s);
            }

            accept_requests();
        });
}

void GroupBridge::async_handle_events() {
    main_context_.async_handle_events(
        [&]() {
            std::lock_guard lock(active_plugins_mutex_);

            // Keep the loop responsive by not handling too many events at once.
            // All X11 events are handled from a Win32 timer so they'll still be
            // handled even when the GUI is blocked.
            //
            // For some reason the Melda plugins run into a seemingly infinite
            // timer loop for a little while after opening a second editor.
            // Without this limit everything will get blocked indefinitely. How
            // could this be fixed?
            HostBridge::handle_events();
        },
        [&]() { return !is_event_loop_inhibited(); });
}

SocketAcceptor create_acceptor_if_inactive(asio::io_context& io_context,
                                           const SocketEndpoint& endpoint) {
#ifdef YABRIDGE_PE_HOST
    // With TCP sockets we can't detect stale endpoints by looking at whether
    // the endpoint is still in use, since another (unrelated) process may have
    // reused a stale port. Instead, the process that hosts the group holds an
    // exclusive handle to a lock file for as long as it's running. Wine
    // enforces Win32 sharing modes across processes, and the handle gets closed
    // automatically when this process exits.
    static HANDLE lock_handle = INVALID_HANDLE_VALUE;
    if (lock_handle == INVALID_HANDLE_VALUE) {
        const std::wstring lock_path =
            fs::path(endpoint.path() + ".lock").wstring();
        lock_handle =
            CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock_handle == INVALID_HANDLE_VALUE) {
            throw std::system_error(
                std::make_error_code(std::errc::address_in_use),
                "Could not lock '" + endpoint.path() + ".lock'");
        }
    }

    return endpoint.listen(io_context);
#else
    // First try to listen on the endpoint normally
    try {
        return endpoint.listen(io_context);
    } catch (const std::system_error&) {
        // If this failed, then either there is a stale socket file or another
        // process is already is already listening. In the last case we will
        // simply throw so the other process can handle the request.
        std::ifstream open_sockets("/proc/net/unix");
        const std::string& endpoint_path = endpoint.path();
        for (std::string line; std::getline(open_sockets, line);) {
            if (line.size() < endpoint_path.size()) {
                continue;
            }

            std::string file = line.substr(line.size() - endpoint_path.size());
            if (file == endpoint_path) {
                // Another process is already listening, so we don't have to do
                // anything
                throw;
            }
        }

        // At this point we can remove the stale socket and start listening
        endpoint.remove();
        return endpoint.listen(io_context);
    }
#endif
}

void GroupBridge::maybe_schedule_shutdown(
    std::chrono::steady_clock::duration delay) {
    std::lock_guard lock(shutdown_timer_mutex_);

    shutdown_timer_.expires_after(delay);
    shutdown_timer_.async_wait([this](const std::error_code& error) {
        // A previous timer gets canceled automatically when another plugin
        // exits
        if (error) {
            return;
        }

        std::lock_guard lock(active_plugins_mutex_);
        if (active_plugins_.size() == 0) {
            logger_.log(
                "All plugins have exited, shutting down the group process");

            // The whole process will exit in `group-host.cpp` because of this
            main_context_.stop();
        }
    });
}

std::string create_logger_prefix(const fs::path& socket_path) {
    // The group socket filename will be in the format
    // '/tmp/yabridge-group-<group_name>-<wine_prefix_id>-<architecture>.sock',
    // where Wine prefix ID is just Wine prefix ran through `std::hash` to
    // prevent collisions without needing complicated filenames. We want to
    // extract the group name.
    std::string socket_name =
        socket_path.filename().replace_extension().string();

    std::smatch group_match;
    std::regex group_regexp("^yabridge-group-(.*)-[^-]+-[^-]+$",
                            std::regex::ECMAScript);
    if (std::regex_match(socket_name, group_match, group_regexp)) {
        socket_name = group_match[1].str();

#ifdef __i386__
        // Mark 32-bit versions to avoid potential confusion caused by 32-bit
        // and regular 64-bit group processes with the same name running
        // alongside eachother
        socket_name += "-x32";
#endif
    }

    return "[" + socket_name + "] ";
}
