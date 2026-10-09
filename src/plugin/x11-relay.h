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

#include <thread>

#include <asio/io_context.hpp>

#include "../common/communication/transport.h"
#include "../common/logging/common.h"

/**
 * The name of the endpoint within a plugin's socket base directory the 32-bit
 * Wine plugin host connects to for its X11 connections. This needs to match
 * the name used in `src/wine-host/xcb-pe/xcb-pe.cpp`.
 */
constexpr char x11_relay_endpoint_name[] = "x11-relay.sock";

/**
 * The plugin editor embedding in the Wine plugin host talks to the X11 server
 * directly to reparent the Wine window into the host's window, handle focus,
 * and for drag-and-drop support. The 32-bit Wine plugin host is a regular
 * Windows executable that cannot open the X11 server's Unix domain socket (see
 * `SocketTransport` for why). To work around that, the native plugin library
 * listens on an authenticated loopback TCP socket, and it then forwards all
 * traffic from every incoming connection to a new connection to the X11
 * server. The X11 protocol itself, including authentication with the X11
 * server, is handled entirely by the Wine plugin host. This is only used for
 * the 32-bit Wine plugin host.
 */
class X11Relay {
   public:
    /**
     * Start listening for incoming connections on `endpoint` on a new thread.
     *
     * @throw std::system_error If we could not listen on the endpoint.
     */
    X11Relay(SocketEndpoint endpoint, Logger& logger);

    /**
     * Stop accepting connections, close all active connections, and join the
     * relay thread.
     */
    ~X11Relay() noexcept;

    X11Relay(const X11Relay&) = delete;
    X11Relay& operator=(const X11Relay&) = delete;

   private:
    void accept_connections();

    Logger& logger_;
    SocketEndpoint endpoint_;
    asio::io_context io_context_;
    SocketAcceptor acceptor_;
    std::jthread relay_thread_;
};

/**
 * Connect a socket to the X11 server described by `$DISPLAY`. This supports
 * the usual `:0`, `unix:0` and `hostname:0` display names, as well as display
 * names that point directly to a socket.
 *
 * @throw std::system_error If we could not connect to the X11 server.
 */
void connect_to_x11_server(Socket& socket);
