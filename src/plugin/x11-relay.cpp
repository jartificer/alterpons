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

#include "x11-relay.h"

#include <array>
#include <memory>

#include <asio/ip/tcp.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/write.hpp>

namespace {

/**
 * A single relayed connection. Data is copied in both directions until either
 * side closes its connection, at which point both connections get closed.
 */
class RelaySession : public std::enable_shared_from_this<RelaySession> {
   public:
    RelaySession(Socket client, Socket server)
        : client_(std::move(client)), server_(std::move(server)) {}

    void start() {
        pump(client_, server_, client_to_server_buffer_);
        pump(server_, client_, server_to_client_buffer_);
    }

   private:
    using Buffer = std::array<uint8_t, 16384>;

    void pump(Socket& from, Socket& to, Buffer& buffer) {
        from.async_read_some(
            asio::buffer(buffer),
            [this, self = shared_from_this(), &from, &to, &buffer](
                const std::error_code& error, size_t bytes_read) {
                if (error) {
                    shutdown();
                    return;
                }

                asio::async_write(to, asio::buffer(buffer, bytes_read),
                                  [this, self, &from, &to, &buffer](
                                      const std::error_code& error, size_t) {
                                      if (error) {
                                          shutdown();
                                          return;
                                      }

                                      pump(from, to, buffer);
                                  });
            });
    }

    void shutdown() {
        std::error_code err;
        client_.shutdown(Socket::shutdown_both, err);
        client_.close(err);
        server_.shutdown(Socket::shutdown_both, err);
        server_.close(err);
    }

    Socket client_;
    Socket server_;
    Buffer client_to_server_buffer_{};
    Buffer server_to_client_buffer_{};
};

}  // namespace

void connect_to_x11_server(Socket& socket) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* display_env = getenv("DISPLAY");
    const std::string display = display_env ? display_env : "";
    if (display.empty()) {
        throw std::system_error(
            std::make_error_code(std::errc::no_such_device_or_address),
            "DISPLAY has not been set");
    }

    // Some systems use the full socket path as the display name
    if (display.starts_with('/')) {
        const std::string socket_path = display.substr(0, display.rfind(':'));
        socket.connect(asio::generic::stream_protocol::endpoint(
            asio::local::stream_protocol::endpoint(socket_path)));
        return;
    }

    // Otherwise we have `[protocol/][host]:display[.screen]`
    const size_t colon_pos = display.rfind(':');
    if (colon_pos == std::string::npos) {
        throw std::system_error(
            std::make_error_code(std::errc::invalid_argument),
            "Invalid DISPLAY '" + display + "'");
    }

    std::string host = display.substr(0, colon_pos);
    if (const size_t slash_pos = host.find('/');
        slash_pos != std::string::npos) {
        host = host.substr(slash_pos + 1);
    }
    const std::string display_number_str = display.substr(
        colon_pos + 1, display.find('.', colon_pos) - colon_pos - 1);
    const int display_number = std::stoi(display_number_str);

    if (host.empty() || host == "unix") {
        // Modern X11 servers listen on both an abstract socket and a socket
        // file. Abstract sockets work regardless of whether `/tmp` is shared,
        // so we'll try that first.
        const std::string socket_path =
            "/tmp/.X11-unix/X" + std::to_string(display_number);
        try {
            socket.connect(asio::generic::stream_protocol::endpoint(
                asio::local::stream_protocol::endpoint(std::string(1, '\0') +
                                                       socket_path)));
        } catch (const std::system_error&) {
            socket = Socket(socket.get_executor());
            socket.connect(asio::generic::stream_protocol::endpoint(
                asio::local::stream_protocol::endpoint(socket_path)));
        }
    } else {
        asio::ip::tcp::resolver resolver(socket.get_executor());
        const auto results =
            resolver.resolve(host, std::to_string(6000 + display_number),
                             asio::ip::tcp::resolver::numeric_service);

        std::error_code err = std::make_error_code(std::errc::host_unreachable);
        for (const auto& result : results) {
            socket = Socket(socket.get_executor());
            socket.connect(
                asio::generic::stream_protocol::endpoint(result.endpoint()),
                err);
            if (!err) {
                socket.set_option(asio::ip::tcp::no_delay(true));
                return;
            }
        }

        throw std::system_error(err, "Could not connect to '" + display + "'");
    }
}

X11Relay::X11Relay(SocketEndpoint endpoint, Logger& logger)
    : logger_(logger),
      endpoint_(std::move(endpoint)),
      io_context_(),
      acceptor_(endpoint_.listen(io_context_)) {
    accept_connections();

    relay_thread_ = std::jthread([this]() {
        pthread_setname_np(pthread_self(), "x11-relay");

        io_context_.run();
    });
}

X11Relay::~X11Relay() noexcept {
    io_context_.stop();
    if (relay_thread_.joinable()) {
        relay_thread_.join();
    }

    std::error_code err;
    acceptor_.close(err);
    endpoint_.remove();
}

void X11Relay::accept_connections() {
    acceptor_.async_accept([this](const std::error_code& error, Socket client) {
        if (error) {
            if (error != asio::error::operation_aborted) {
                logger_.log("X11 relay: failure while accepting connections: " +
                            error.message());
            }

            return;
        }

        try {
            endpoint_.finish_accept(client);

            Socket server(io_context_);
            connect_to_x11_server(server);

            std::make_shared<RelaySession>(std::move(client), std::move(server))
                ->start();
        } catch (const std::exception& error) {
            logger_.log("X11 relay: " + std::string(error.what()));
        }

        accept_connections();
    });
}
