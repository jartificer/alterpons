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

#include <atomic>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#ifdef __WINE__
#include "../../wine-host/use-linux-asio.h"
#endif
#include <asio/buffer.hpp>
#include <asio/generic/stream_protocol.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>

/**
 * How the native plugin and the Wine plugin host talk to each other.
 *
 * The 64-bit Winelib host (`yabridge-host.exe`) is a Linux program and it can
 * use Unix domain sockets directly. The 32-bit host (`yabridge-host-32.exe`)
 * is a plain 32-bit Windows (PE) executable built with MinGW. This is required
 * because modern Wine builds, like the `wine` and `wine-staging` packages on
 * Arch Linux, use Wine's 'new WoW64' mode where there is no 32-bit Linux side
 * anymore, so 32-bit Winelib applications cannot run there. A PE executable
 * can only use the Windows API, and Wine's Winsock implementation does not
 * support `AF_UNIX` sockets (outside of an experimental wine-staging patch
 * set). So for the 32-bit host we use TCP sockets bound to the loopback
 * interface instead.
 *
 * Unix domain sockets are protected by filesystem permissions, while anyone on
 * the machine can connect to a loopback TCP port. To make up for that, every
 * TCP connection starts with a secret token that is only known to the user
 * running yabridge. See `get_tcp_auth_token()`.
 */
enum class SocketTransport : uint8_t {
    unix_domain,
    tcp_loopback,
};

#ifdef YABRIDGE_PE_HOST
constexpr SocketTransport default_socket_transport =
    SocketTransport::tcp_loopback;
#else
constexpr SocketTransport default_socket_transport =
    SocketTransport::unix_domain;
#endif

#ifdef YABRIDGE_PE_HOST
/**
 * A stream socket with a read-ahead buffer. In Wine every Winsock operation
 * involves at least one round trip to wineserver, which makes them a lot more
 * expensive than regular Linux system calls. `read_object()` reads an object's
 * size and then the object itself, and since `write_object()` sends both in a
 * single write, reading as much as possible at once lets us receive most
 * messages with a single `recv()` call. Any bytes that have been read ahead are
 * stored in the socket object itself, so they won't be lost.
 */
class BufferedSocket : public asio::generic::stream_protocol::socket {
   public:
    using Base = asio::generic::stream_protocol::socket;
    using Base::Base;

    // NOLINTNEXTLINE(google-explicit-constructor)
    BufferedSocket(Base&& socket) : Base(std::move(socket)) {}

    BufferedSocket(BufferedSocket&&) = default;
    BufferedSocket& operator=(BufferedSocket&&) = default;

    template <typename MutableBufferSequence>
    size_t read_some(const MutableBufferSequence& buffers,
                     std::error_code& ec) {
        ec = std::error_code();
        const size_t requested_size = asio::buffer_size(buffers);
        if (requested_size == 0) {
            return 0;
        }

        if (read_ahead_start_ == read_ahead_end_) {
            // Large reads bypass the buffer entirely
            if (requested_size >= read_ahead_buffer_size) {
                return Base::read_some(buffers, ec);
            }

            if (read_ahead_.empty()) {
                read_ahead_.resize(read_ahead_buffer_size);
            }
            read_ahead_start_ = 0;
            read_ahead_end_ = Base::read_some(asio::buffer(read_ahead_), ec);
            if (ec) {
                read_ahead_end_ = 0;
                return 0;
            }
        }

        const size_t bytes_copied = asio::buffer_copy(
            buffers, asio::buffer(read_ahead_.data() + read_ahead_start_,
                                  read_ahead_end_ - read_ahead_start_));
        read_ahead_start_ += bytes_copied;

        return bytes_copied;
    }

    template <typename MutableBufferSequence>
    size_t read_some(const MutableBufferSequence& buffers) {
        std::error_code ec;
        const size_t bytes_read = read_some(buffers, ec);
        if (ec) {
            throw std::system_error(ec, "read_some");
        }

        return bytes_read;
    }

    size_t available() {
        return (read_ahead_end_ - read_ahead_start_) + Base::available();
    }

   private:
    static constexpr size_t read_ahead_buffer_size = 64 * 1024;

    std::vector<uint8_t> read_ahead_;
    size_t read_ahead_start_ = 0;
    size_t read_ahead_end_ = 0;
};

/**
 * The socket type used for all communication. In the PE host this is a
 * buffered TCP socket, see above.
 */
using Socket = BufferedSocket;
#else
/**
 * The socket type used for all communication. This is a protocol-agnostic
 * stream socket that can hold either a Unix domain socket or a TCP socket.
 */
using Socket = asio::generic::stream_protocol::socket;
#endif
using SocketAcceptor =
    asio::basic_socket_acceptor<asio::generic::stream_protocol>;

/**
 * The environment variable used to pass the TCP authentication token to the
 * Wine plugin host. Environment variables can only be read by the user that
 * owns the process, unlike command line arguments.
 */
constexpr char tcp_auth_token_env_var[] = "YABRIDGE_TCP_TOKEN";

/**
 * The length of the TCP authentication token, in bytes. The token consists of
 * lowercase hexadecimal characters.
 */
constexpr size_t tcp_auth_token_length = 64;

/**
 * Get the secret token that has to be sent at the start of every TCP
 * connection.
 *
 * On the native plugin side this token is stored in a file only readable by
 * the current user in the temporary directory, and it's created when it does
 * not yet exist. That way all yabridge instances for a user share the same
 * token, which is needed for plugin groups since a group host process can be
 * shared by multiple yabridge plugin instances. On the Wine side the token is
 * read from the `YABRIDGE_TCP_TOKEN` environment variable that's set by the
 * native plugin when launching the host.
 *
 * @throw std::runtime_error If the token cannot be read or created.
 */
const std::string& get_tcp_auth_token();

/**
 * A socket endpoint. With Unix domain sockets this is simply the path to the
 * socket. With TCP sockets the listening side binds to a random port on the
 * loopback interface and writes that port number to a file at `path`. The
 * connecting side reads that file to find out which port to connect to. This
 * way the rest of yabridge can keep using the same path-based endpoints
 * regardless of the transport.
 */
class SocketEndpoint {
   public:
    // NOLINTNEXTLINE(google-explicit-constructor)
    SocketEndpoint(std::string path,
                   SocketTransport transport = default_socket_transport);

    // NOLINTNEXTLINE(google-explicit-constructor)
    SocketEndpoint(const char* path,
                   SocketTransport transport = default_socket_transport);

    const std::string& path() const noexcept { return path_; }
    SocketTransport transport() const noexcept { return transport_; }

    /**
     * Connect `socket` to this endpoint. With TCP sockets this will also send
     * the authentication token.
     *
     * @throw std::system_error If the connection could not be made. This
     *   includes the situation where nobody is listening on the endpoint (yet).
     */
    void connect(Socket& socket) const;

    /**
     * Start listening on this endpoint. With TCP sockets this will (atomically)
     * create or overwrite the port file at `path()`. The parent directory
     * should already exist.
     *
     * Every socket accepted through the returned acceptor should be passed to
     * `finish_accept()` before it's used.
     *
     * @throw std::system_error If we could not listen on the endpoint.
     */
    SocketAcceptor listen(asio::io_context& io_context) const;

    /**
     * Should be called on every newly accepted socket. With TCP sockets this
     * will read and verify the authentication token and disable Nagle's
     * algorithm.
     *
     * @throw std::system_error If the socket did not authenticate itself. The
     *   socket will have been closed.
     */
    void finish_accept(Socket& socket) const;

    /**
     * Accept a single connection on `acceptor` and finish the handshake. Any
     * connection that fails to authenticate is dropped, and we'll wait for the
     * next connection instead.
     *
     * @param abort If this is set and it becomes `true` while we're waiting for
     *   a connection, then this will stop waiting and throw a
     *   `std::system_error` with `std::errc::operation_canceled`. Used during
     *   startup so we don't wait forever when the Wine plugin host has died.
     */
    void accept(SocketAcceptor& acceptor,
                Socket& socket,
                const std::atomic_bool* abort = nullptr) const;

    /**
     * Remove the socket or port file, if it exists. Never throws.
     */
    void remove() const noexcept;

    /**
     * Check whether some process is currently accepting connections on this
     * endpoint. Used for plugin groups to detect stale endpoints.
     */
    bool is_active(asio::io_context& io_context) const;

   private:
    /**
     * The port stored in the port file at `path_`.
     *
     * @throw std::system_error If the port file does not exist or if it's
     *   invalid.
     */
    uint16_t read_port() const;

    std::string path_;
    SocketTransport transport_;
};

/**
 * Configure a freshly connected or accepted socket for low latency
 * communication. This disables Nagle's algorithm for TCP sockets.
 */
void configure_socket(Socket& socket, SocketTransport transport);
