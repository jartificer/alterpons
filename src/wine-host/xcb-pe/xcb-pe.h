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

// The 32-bit Wine plugin host is a regular Windows executable (see
// `SocketTransport`), so it cannot link against the system's libxcb. Instead
// we implement the small subset of libxcb's API that yabridge's editor
// embedding and drag-and-drop support use ourselves, using libxcb's own
// headers for the protocol definitions. All X11 traffic goes through an
// authenticated TCP connection to the native plugin, which relays it to the
// X11 server (see `src/plugin/x11-relay.h`).
//
// Only the functions declared in libxcb's headers that are actually used by
// yabridge are implemented.

#ifndef YABRIDGE_PE_HOST
#error "This header should only be included in the PE (MinGW) host build"
#endif

#include <optional>
#include <string>

#include <xcb/xcb.h>

/**
 * The name of the X11 relay endpoint within a plugin's socket endpoint base
 * directory. Needs to match `x11_relay_endpoint_name` in
 * `src/plugin/x11-relay.h`.
 */
constexpr char x11_relay_endpoint_name[] = "x11-relay.sock";

/**
 * While an instance of this class is alive, `xcb_connect()` calls made on the
 * current thread will connect through the X11 relay of the plugin whose socket
 * endpoint base directory was passed to the constructor. This is used so that
 * the editor of a plugin uses that same plugin's relay. If `xcb_connect()` is
 * called while no relay is active on the current thread, then the most
 * recently used relay will be used instead.
 */
class ScopedX11Relay {
   public:
    explicit ScopedX11Relay(const std::string& endpoint_base_dir);
    ~ScopedX11Relay() noexcept;

    ScopedX11Relay(const ScopedX11Relay&) = delete;
    ScopedX11Relay& operator=(const ScopedX11Relay&) = delete;

   private:
    std::optional<std::string> previous_endpoint_;
};
