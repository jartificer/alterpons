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

#include "xcb-pe.h"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <ghc/filesystem.hpp>

#include "../../common/communication/transport.h"
#include "../../common/utils.h"

namespace fs = ghc::filesystem;

// X11 protocol opcodes for the requests we implement, see
// https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html#Encoding::Requests
namespace opcode {
constexpr uint8_t create_window = 1;
constexpr uint8_t change_window_attributes = 2;
constexpr uint8_t destroy_window = 4;
constexpr uint8_t reparent_window = 7;
constexpr uint8_t map_window = 8;
constexpr uint8_t configure_window = 12;
constexpr uint8_t get_geometry = 14;
constexpr uint8_t query_tree = 15;
constexpr uint8_t intern_atom = 16;
constexpr uint8_t change_property = 18;
constexpr uint8_t delete_property = 19;
constexpr uint8_t get_property = 20;
constexpr uint8_t set_selection_owner = 22;
constexpr uint8_t send_event = 25;
constexpr uint8_t grab_key = 33;
constexpr uint8_t ungrab_key = 34;
constexpr uint8_t query_pointer = 38;
constexpr uint8_t translate_coordinates = 40;
constexpr uint8_t set_input_focus = 42;
constexpr uint8_t get_input_focus = 43;
constexpr uint8_t get_keyboard_mapping = 101;
}  // namespace opcode

constexpr uint8_t x11_error = 0;
constexpr uint8_t x11_reply = 1;
constexpr uint8_t x11_generic_event = 35;

/**
 * A helper to build the binary representation of an X11 request.
 */
class RequestBuilder {
   public:
    RequestBuilder(uint8_t major_opcode, uint8_t data = 0) {
        card8(major_opcode);
        card8(data);
        // The length gets filled in by `finish()`
        card16(0);
    }

    RequestBuilder& card8(uint8_t value) {
        bytes_.push_back(value);
        return *this;
    }

    RequestBuilder& card16(uint16_t value) {
        append(&value, sizeof(value));
        return *this;
    }

    RequestBuilder& card32(uint32_t value) {
        append(&value, sizeof(value));
        return *this;
    }

    RequestBuilder& pad(size_t num_bytes) {
        bytes_.insert(bytes_.end(), num_bytes, 0);
        return *this;
    }

    RequestBuilder& append(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
        return *this;
    }

    /**
     * Append a list of 32-bit values whose length is determined by the number
     * of bits set in a value mask.
     */
    RequestBuilder& value_list(uint32_t value_mask, const void* value_list) {
        const int num_values = std::popcount(value_mask);
        if (num_values > 0 && value_list) {
            append(value_list, num_values * sizeof(uint32_t));
        }
        return *this;
    }

    /**
     * Pad the request to a multiple of four bytes and write the length field.
     * X11 is native endian, and we always use little endian.
     */
    std::vector<uint8_t> finish() {
        while (bytes_.size() % 4 != 0) {
            bytes_.push_back(0);
        }

        const uint16_t length = static_cast<uint16_t>(bytes_.size() / 4);
        std::memcpy(bytes_.data() + 2, &length, sizeof(length));

        return std::move(bytes_);
    }

   private:
    std::vector<uint8_t> bytes_;
};

/**
 * A fake setup containing a single screen, returned by `xcb_get_setup()` on a
 * connection that failed to connect. This way code that assumes there's always
 * a screen won't crash. Everything is zero initialized, so the root window will
 * be `XCB_NONE`.
 */
struct alignas(4) FallbackSetup {
    xcb_setup_t setup{};
    xcb_screen_t screen{};
};

struct xcb_connection_t {
    xcb_connection_t() : socket_(io_context_) {}

    /**
     * Set when the connection failed or broke at some point. All requests will
     * be ignored and all replies will be null pointers.
     */
    bool has_error_ = true;

    asio::io_context io_context_;
    Socket socket_;
    std::mutex mutex_;

    /**
     * The connection setup information sent by the X11 server, starting with
     * the `xcb_setup_t` header.
     */
    std::vector<uint8_t> setup_bytes_;
    FallbackSetup fallback_setup_{};

    uint32_t resource_id_base_ = 0;
    uint32_t resource_id_mask_ = 0;
    uint32_t last_resource_id_ = 0;

    /**
     * The full (non-truncated) sequence number of the last request sent.
     */
    uint64_t last_request_ = 0;

    /**
     * Requests with these sequence numbers either expect a reply or they were
     * made through a `*_checked()` function. Errors for these requests are
     * kept in `errors_` rather than being added to the event queue.
     */
    std::unordered_set<uint64_t> checked_requests_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> replies_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> errors_;
    std::deque<std::vector<uint8_t>> events_;

    /**
     * Bytes received from the X11 server that have not yet been parsed.
     */
    std::vector<uint8_t> input_buffer_;

    void fail() {
        has_error_ = true;

        std::error_code err;
        socket_.close(err);
    }

    /**
     * Send a request, returning its sequence number.
     */
    uint64_t send_request(std::vector<uint8_t> request, bool checked) {
        std::lock_guard lock(mutex_);
        if (has_error_) {
            return 0;
        }

        try {
            asio::write(socket_, asio::buffer(request));
        } catch (const std::system_error&) {
            fail();
            return 0;
        }

        last_request_++;
        if (checked) {
            checked_requests_.insert(last_request_);
        }

        return last_request_;
    }

    /**
     * Expand a 16-bit sequence number to a full sequence number, based on the
     * sequence number of the last request we've sent. Responses always refer to
     * requests we've already sent.
     */
    uint64_t widen_sequence(uint16_t sequence) const {
        uint64_t full_sequence = (last_request_ & ~uint64_t(0xffff)) | sequence;
        if (full_sequence > last_request_) {
            full_sequence -= 0x10000;
        }

        return full_sequence;
    }

    /**
     * Read data from the socket. If `blocking` is false, then this will only
     * read data that's already available. Returns false on errors.
     */
    bool fill_input_buffer(bool blocking) {
        try {
            size_t bytes_to_read = socket_.available();
            if (bytes_to_read == 0) {
                if (!blocking) {
                    return true;
                }
                bytes_to_read = 4096;
            }

            const size_t old_size = input_buffer_.size();
            input_buffer_.resize(old_size + bytes_to_read);
            const size_t bytes_read = socket_.read_some(asio::buffer(
                input_buffer_.data() + old_size, bytes_to_read));
            input_buffer_.resize(old_size + bytes_read);

            return true;
        } catch (const std::system_error&) {
            fail();
            return false;
        }
    }

    /**
     * Parse all complete packets in the input buffer, and sort them into
     * replies, errors, and events.
     */
    void parse_packets() {
        size_t offset = 0;
        while (input_buffer_.size() - offset >= 32) {
            const uint8_t* packet = input_buffer_.data() + offset;
            const uint8_t response_type = packet[0] & 0x7f;

            size_t packet_size = 32;
            if (response_type == x11_reply ||
                response_type == x11_generic_event) {
                uint32_t extra_length;
                std::memcpy(&extra_length, packet + 4, sizeof(extra_length));
                packet_size += static_cast<size_t>(extra_length) * 4;
            }
            if (input_buffer_.size() - offset < packet_size) {
                break;
            }

            uint16_t sequence;
            std::memcpy(&sequence, packet + 2, sizeof(sequence));
            const uint64_t full_sequence = widen_sequence(sequence);

            if (response_type == x11_reply) {
                replies_[full_sequence] =
                    std::vector<uint8_t>(packet, packet + packet_size);
            } else {
                // libxcb's error and event structs have an additional
                // `full_sequence` field at the end
                std::vector<uint8_t> data(packet, packet + 32);
                data.resize(36);
                const uint32_t full_sequence_32 =
                    static_cast<uint32_t>(full_sequence);
                std::memcpy(data.data() + 32, &full_sequence_32,
                            sizeof(full_sequence_32));

                if (response_type == x11_error &&
                    checked_requests_.contains(full_sequence)) {
                    errors_[full_sequence] = std::move(data);
                } else if (response_type != x11_generic_event) {
                    // We don't use any extensions, so we'll just drop generic
                    // events
                    events_.push_back(std::move(data));
                }
            }

            offset += packet_size;
        }

        input_buffer_.erase(input_buffer_.begin(),
                            input_buffer_.begin() + offset);
    }

    /**
     * Wait for a reply or an error for the request with the specified sequence
     * number. Returns the reply, or a nullopt in case of an error. Errors are
     * written to `error` if it's not a null pointer.
     */
    std::optional<std::vector<uint8_t>> wait_for_reply(
        uint64_t sequence,
        xcb_generic_error_t** error) {
        if (error) {
            *error = nullptr;
        }

        std::lock_guard lock(mutex_);
        if (sequence == 0) {
            return std::nullopt;
        }

        while (true) {
            if (auto reply = replies_.find(sequence); reply != replies_.end()) {
                std::vector<uint8_t> result = std::move(reply->second);
                replies_.erase(reply);
                checked_requests_.erase(sequence);

                return result;
            }
            if (auto error_data = errors_.find(sequence);
                error_data != errors_.end()) {
                if (error) {
                    *error = static_cast<xcb_generic_error_t*>(
                        malloc(error_data->second.size()));
                    std::memcpy(*error, error_data->second.data(),
                                error_data->second.size());
                }

                errors_.erase(error_data);
                checked_requests_.erase(sequence);

                return std::nullopt;
            }

            if (has_error_ || !fill_input_buffer(true)) {
                return std::nullopt;
            }
            parse_packets();
        }
    }

    /**
     * Connect to the X11 relay at `relay_endpoint` and set up the X11
     * connection. Sets `has_error_` to false if this succeeded.
     */
    void connect(const std::string& relay_endpoint);
};

namespace {

/**
 * The endpoint base directory of the relay used for `xcb_connect()` calls on
 * the current thread. See `ScopedX11Relay`.
 */
thread_local std::optional<std::string> current_relay_endpoint;

/**
 * The most recently used relay endpoint, used as a fallback.
 */
std::mutex last_relay_endpoint_mutex;
std::optional<std::string> last_relay_endpoint;

constexpr char mit_magic_cookie_name[] = "MIT-MAGIC-COOKIE-1";

struct DisplayInfo {
    std::string host;
    std::string number;
};

DisplayInfo parse_display() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* display_env = getenv("DISPLAY");
    const std::string display = display_env ? display_env : "";

    DisplayInfo info{};
    const size_t colon_pos = display.rfind(':');
    if (colon_pos == std::string::npos) {
        return info;
    }

    info.host = display.substr(0, colon_pos);
    if (const size_t slash_pos = info.host.find('/');
        slash_pos != std::string::npos) {
        info.host = info.host.substr(slash_pos + 1);
    }
    info.number = display.substr(colon_pos + 1);
    if (const size_t dot_pos = info.number.find('.');
        dot_pos != std::string::npos) {
        info.number = info.number.substr(0, dot_pos);
    }

    return info;
}

std::string read_file(const std::string& unix_path) {
    fs::ifstream file(fs::path(to_host_path(unix_path)), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

std::string get_hostname() {
    std::string hostname = read_file("/proc/sys/kernel/hostname");
    while (!hostname.empty() &&
           (hostname.back() == '\n' || hostname.back() == '\r')) {
        hostname.pop_back();
    }

    return hostname;
}

/**
 * Find the MIT-MAGIC-COOKIE-1 authentication cookie for the current display
 * in the user's Xauthority file. Returns an empty string if there isn't one,
 * in which case we'll try to connect without authentication.
 */
std::string find_auth_cookie() {
    std::string xauthority_path;
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const char* xauthority = getenv("XAUTHORITY"); xauthority) {
        xauthority_path = xauthority;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
    } else if (const char* home = getenv("HOME"); home) {
        xauthority_path = std::string(home) + "/.Xauthority";
    } else {
        return "";
    }

    const std::string data = read_file(xauthority_path);
    const DisplayInfo display = parse_display();
    const std::string hostname = get_hostname();

    // These are the address families from `Xauth.h`
    constexpr uint16_t family_local = 256;
    constexpr uint16_t family_wild = 65535;

    // We'll prefer exact matches, but fall back to anything with the same
    // display number
    std::string best_cookie;
    int best_score = -1;

    size_t offset = 0;
    const auto read_card16 = [&](uint16_t& value) {
        if (offset + 2 > data.size()) {
            return false;
        }
        value = static_cast<uint16_t>(
            (static_cast<uint8_t>(data[offset]) << 8) |
            static_cast<uint8_t>(data[offset + 1]));
        offset += 2;
        return true;
    };
    const auto read_string = [&](std::string& value) {
        uint16_t length;
        if (!read_card16(length) || offset + length > data.size()) {
            return false;
        }
        value = data.substr(offset, length);
        offset += length;
        return true;
    };

    while (offset < data.size()) {
        uint16_t family;
        std::string address, number, name, cookie;
        if (!read_card16(family) || !read_string(address) ||
            !read_string(number) || !read_string(name) ||
            !read_string(cookie)) {
            break;
        }

        if (name != mit_magic_cookie_name ||
            !(number.empty() || number == display.number)) {
            continue;
        }

        int score = 0;
        if (family == family_local && address == hostname) {
            score = 3;
        } else if (family == family_wild) {
            score = 2;
        } else if (family == family_local) {
            score = 1;
        }

        if (score > best_score) {
            best_score = score;
            best_cookie = cookie;
        }
    }

    return best_cookie;
}

std::string pad_to_4(std::string str) {
    while (str.size() % 4 != 0) {
        str.push_back('\0');
    }

    return str;
}

}  // namespace

void xcb_connection_t::connect(const std::string& relay_endpoint) {
    try {
        SocketEndpoint(relay_endpoint, SocketTransport::tcp_loopback)
            .connect(socket_);

        // Set up the X11 connection, as described in
        // https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html#Encoding::Connection_Setup
        const std::string cookie = find_auth_cookie();
        const std::string auth_name = cookie.empty() ? "" : mit_magic_cookie_name;

        std::vector<uint8_t> setup_request;
        const auto push_card16 = [&](uint16_t value) {
            setup_request.push_back(static_cast<uint8_t>(value & 0xff));
            setup_request.push_back(static_cast<uint8_t>(value >> 8));
        };
        // 'l' for little endian
        setup_request.push_back('l');
        setup_request.push_back(0);
        push_card16(11);
        push_card16(0);
        push_card16(static_cast<uint16_t>(auth_name.size()));
        push_card16(static_cast<uint16_t>(cookie.size()));
        push_card16(0);
        const std::string padded_name = pad_to_4(auth_name);
        const std::string padded_cookie = pad_to_4(cookie);
        setup_request.insert(setup_request.end(), padded_name.begin(),
                             padded_name.end());
        setup_request.insert(setup_request.end(), padded_cookie.begin(),
                             padded_cookie.end());
        asio::write(socket_, asio::buffer(setup_request));

        std::array<uint8_t, 8> header{};
        asio::read(socket_, asio::buffer(header));
        uint16_t additional_length;
        std::memcpy(&additional_length, header.data() + 6,
                    sizeof(additional_length));

        setup_bytes_.resize(8 + (static_cast<size_t>(additional_length) * 4));
        std::memcpy(setup_bytes_.data(), header.data(), header.size());
        asio::read(socket_, asio::buffer(setup_bytes_.data() + 8,
                                         setup_bytes_.size() - 8));

        // A status of 1 means success. Otherwise there's a reason string after
        // the header.
        if (header[0] != 1 || setup_bytes_.size() < sizeof(xcb_setup_t)) {
            const uint8_t reason_length = header[1];
            const std::string reason(
                reinterpret_cast<const char*>(setup_bytes_.data()) + 8,
                std::min<size_t>(reason_length, setup_bytes_.size() - 8));
            std::cerr << "Could not connect to the X11 server: " << reason
                      << std::endl;

            setup_bytes_.clear();
            fail();
            return;
        }

        const auto* setup =
            reinterpret_cast<const xcb_setup_t*>(setup_bytes_.data());
        resource_id_base_ = setup->resource_id_base;
        resource_id_mask_ = setup->resource_id_mask;

        has_error_ = false;
    } catch (const std::exception& error) {
        std::cerr << "Could not connect to the X11 relay at '"
                  << relay_endpoint << "': " << error.what() << std::endl;
        fail();
    }
}

ScopedX11Relay::ScopedX11Relay(const std::string& endpoint_base_dir)
    : previous_endpoint_(current_relay_endpoint) {
    const std::string endpoint =
        (fs::path(endpoint_base_dir) / x11_relay_endpoint_name).string();
    current_relay_endpoint = endpoint;

    std::lock_guard lock(last_relay_endpoint_mutex);
    last_relay_endpoint = endpoint;
}

ScopedX11Relay::~ScopedX11Relay() noexcept {
    current_relay_endpoint = previous_endpoint_;
}

//
// The libxcb API
//

xcb_connection_t* xcb_connect(const char* /*displayname*/, int* screenp) {
    auto* connection = new xcb_connection_t();

    std::optional<std::string> relay_endpoint = current_relay_endpoint;
    if (!relay_endpoint) {
        std::lock_guard lock(last_relay_endpoint_mutex);
        relay_endpoint = last_relay_endpoint;
    }

    if (relay_endpoint) {
        connection->connect(*relay_endpoint);
    } else {
        std::cerr << "Could not connect to the X11 server, no X11 relay is "
                     "available"
                  << std::endl;
    }

    if (screenp) {
        // We'll always use the first screen, just like the Winelib host
        // effectively does when not specifying a display name
        *screenp = 0;
    }

    return connection;
}

void xcb_disconnect(xcb_connection_t* c) {
    if (c) {
        c->fail();
        delete c;
    }
}

int xcb_connection_has_error(xcb_connection_t* c) {
    return !c || c->has_error_ ? XCB_CONN_ERROR : 0;
}

int xcb_flush(xcb_connection_t* c) {
    // We don't buffer requests, so there's nothing to flush
    return c && !c->has_error_ ? 1 : 0;
}

const struct xcb_setup_t* xcb_get_setup(xcb_connection_t* c) {
    if (c->setup_bytes_.empty()) {
        c->fallback_setup_.setup.roots_len = 1;
        return &c->fallback_setup_.setup;
    }

    return reinterpret_cast<const xcb_setup_t*>(c->setup_bytes_.data());
}

xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t* R) {
    xcb_screen_iterator_t iterator{};
    if (!R) {
        return iterator;
    }

    // The screens come after the vendor string and the pixmap formats
    const auto* bytes = reinterpret_cast<const uint8_t*>(R);
    const size_t vendor_length_padded =
        (static_cast<size_t>(R->vendor_len) + 3) & ~static_cast<size_t>(3);
    iterator.data = reinterpret_cast<xcb_screen_t*>(const_cast<uint8_t*>(
        bytes + sizeof(xcb_setup_t) + vendor_length_padded +
        (static_cast<size_t>(R->pixmap_formats_len) * sizeof(xcb_format_t))));
    iterator.rem = R->roots_len;
    iterator.index = static_cast<int>(reinterpret_cast<const uint8_t*>(
                                          iterator.data) -
                                      bytes);

    return iterator;
}

uint32_t xcb_generate_id(xcb_connection_t* c) {
    std::lock_guard lock(c->mutex_);
    if (c->has_error_ || c->resource_id_mask_ == 0) {
        return static_cast<uint32_t>(-1);
    }

    const uint32_t increment = c->resource_id_mask_ & -c->resource_id_mask_;
    c->last_resource_id_ += increment;

    return c->resource_id_base_ | (c->last_resource_id_ & c->resource_id_mask_);
}

xcb_generic_event_t* xcb_poll_for_event(xcb_connection_t* c) {
    std::lock_guard lock(c->mutex_);
    if (c->events_.empty() && !c->has_error_) {
        if (c->fill_input_buffer(false)) {
            c->parse_packets();
        }
    }

    if (c->events_.empty()) {
        return nullptr;
    }

    const std::vector<uint8_t> event = std::move(c->events_.front());
    c->events_.pop_front();

    auto* result = static_cast<xcb_generic_event_t*>(malloc(event.size()));
    std::memcpy(result, event.data(), event.size());

    return result;
}

xcb_generic_error_t* xcb_request_check(xcb_connection_t* c,
                                       xcb_void_cookie_t cookie) {
    if (cookie.sequence == 0) {
        return nullptr;
    }

    // To know for sure that the server has processed the request, we'll make
    // a round trip with a request that always generates a reply. Since the
    // server processes requests in order, any error for the checked request
    // will have arrived before that reply.
    const uint64_t sync_sequence = c->send_request(
        RequestBuilder(opcode::get_input_focus).finish(), true);
    c->wait_for_reply(sync_sequence, nullptr);

    std::lock_guard lock(c->mutex_);
    uint64_t full_sequence = cookie.sequence;
    if (auto error = c->errors_.find(full_sequence);
        error != c->errors_.end()) {
        auto* result =
            static_cast<xcb_generic_error_t*>(malloc(error->second.size()));
        std::memcpy(result, error->second.data(), error->second.size());
        c->errors_.erase(error);
        c->checked_requests_.erase(full_sequence);

        return result;
    }

    c->checked_requests_.erase(full_sequence);
    return nullptr;
}

namespace {

template <typename Reply>
Reply* reply_or_null(xcb_connection_t* c,
                     unsigned int sequence,
                     xcb_generic_error_t** e) {
    std::optional<std::vector<uint8_t>> reply = c->wait_for_reply(sequence, e);
    if (!reply) {
        return nullptr;
    }

    // Replies are always at least 32 bytes, which is also at least the size of
    // all of these structs
    static_assert(sizeof(Reply) <= 32);
    auto* result = static_cast<Reply*>(malloc(reply->size()));
    std::memcpy(result, reply->data(), reply->size());

    return result;
}

xcb_void_cookie_t void_request(xcb_connection_t* c,
                               RequestBuilder& request,
                               bool checked = false) {
    return xcb_void_cookie_t{static_cast<unsigned int>(
        c->send_request(request.finish(), checked))};
}

unsigned int reply_request(xcb_connection_t* c, RequestBuilder& request) {
    return static_cast<unsigned int>(c->send_request(request.finish(), true));
}

}  // namespace

xcb_void_cookie_t xcb_create_window(xcb_connection_t* c,
                                    uint8_t depth,
                                    xcb_window_t wid,
                                    xcb_window_t parent,
                                    int16_t x,
                                    int16_t y,
                                    uint16_t width,
                                    uint16_t height,
                                    uint16_t border_width,
                                    uint16_t _class,
                                    xcb_visualid_t visual,
                                    uint32_t value_mask,
                                    const void* value_list) {
    RequestBuilder request(opcode::create_window, depth);
    request.card32(wid)
        .card32(parent)
        .card16(static_cast<uint16_t>(x))
        .card16(static_cast<uint16_t>(y))
        .card16(width)
        .card16(height)
        .card16(border_width)
        .card16(_class)
        .card32(visual)
        .card32(value_mask)
        .value_list(value_mask, value_list);

    return void_request(c, request);
}

xcb_void_cookie_t xcb_change_window_attributes(xcb_connection_t* c,
                                               xcb_window_t window,
                                               uint32_t value_mask,
                                               const void* value_list) {
    RequestBuilder request(opcode::change_window_attributes);
    request.card32(window).card32(value_mask).value_list(value_mask,
                                                         value_list);

    return void_request(c, request);
}

xcb_void_cookie_t xcb_destroy_window(xcb_connection_t* c, xcb_window_t window) {
    RequestBuilder request(opcode::destroy_window);
    request.card32(window);

    return void_request(c, request);
}

namespace {

RequestBuilder reparent_window_request(xcb_window_t window,
                                       xcb_window_t parent,
                                       int16_t x,
                                       int16_t y) {
    RequestBuilder request(opcode::reparent_window);
    request.card32(window)
        .card32(parent)
        .card16(static_cast<uint16_t>(x))
        .card16(static_cast<uint16_t>(y));

    return request;
}

}  // namespace

xcb_void_cookie_t xcb_reparent_window(xcb_connection_t* c,
                                      xcb_window_t window,
                                      xcb_window_t parent,
                                      int16_t x,
                                      int16_t y) {
    RequestBuilder request = reparent_window_request(window, parent, x, y);
    return void_request(c, request);
}

xcb_void_cookie_t xcb_reparent_window_checked(xcb_connection_t* c,
                                              xcb_window_t window,
                                              xcb_window_t parent,
                                              int16_t x,
                                              int16_t y) {
    RequestBuilder request = reparent_window_request(window, parent, x, y);
    return void_request(c, request, true);
}

xcb_void_cookie_t xcb_map_window(xcb_connection_t* c, xcb_window_t window) {
    RequestBuilder request(opcode::map_window);
    request.card32(window);

    return void_request(c, request);
}

xcb_void_cookie_t xcb_configure_window(xcb_connection_t* c,
                                       xcb_window_t window,
                                       uint16_t value_mask,
                                       const void* value_list) {
    RequestBuilder request(opcode::configure_window);
    request.card32(window).card16(value_mask).pad(2).value_list(value_mask,
                                                                value_list);

    return void_request(c, request);
}

xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t* c,
                                           xcb_drawable_t drawable) {
    RequestBuilder request(opcode::get_geometry);
    request.card32(drawable);

    return xcb_get_geometry_cookie_t{reply_request(c, request)};
}

xcb_get_geometry_reply_t* xcb_get_geometry_reply(
    xcb_connection_t* c,
    xcb_get_geometry_cookie_t cookie,
    xcb_generic_error_t** e) {
    return reply_or_null<xcb_get_geometry_reply_t>(c, cookie.sequence, e);
}

xcb_query_tree_cookie_t xcb_query_tree(xcb_connection_t* c,
                                       xcb_window_t window) {
    RequestBuilder request(opcode::query_tree);
    request.card32(window);

    return xcb_query_tree_cookie_t{reply_request(c, request)};
}

xcb_query_tree_reply_t* xcb_query_tree_reply(xcb_connection_t* c,
                                             xcb_query_tree_cookie_t cookie,
                                             xcb_generic_error_t** e) {
    return reply_or_null<xcb_query_tree_reply_t>(c, cookie.sequence, e);
}

xcb_window_t* xcb_query_tree_children(const xcb_query_tree_reply_t* R) {
    return reinterpret_cast<xcb_window_t*>(const_cast<xcb_query_tree_reply_t*>(R) + 1);
}

int xcb_query_tree_children_length(const xcb_query_tree_reply_t* R) {
    return R->children_len;
}

xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t* c,
                                         uint8_t only_if_exists,
                                         uint16_t name_len,
                                         const char* name) {
    RequestBuilder request(opcode::intern_atom, only_if_exists);
    request.card16(name_len).pad(2).append(name, name_len);

    return xcb_intern_atom_cookie_t{reply_request(c, request)};
}

xcb_intern_atom_reply_t* xcb_intern_atom_reply(xcb_connection_t* c,
                                               xcb_intern_atom_cookie_t cookie,
                                               xcb_generic_error_t** e) {
    return reply_or_null<xcb_intern_atom_reply_t>(c, cookie.sequence, e);
}

xcb_void_cookie_t xcb_change_property(xcb_connection_t* c,
                                      uint8_t mode,
                                      xcb_window_t window,
                                      xcb_atom_t property,
                                      xcb_atom_t type,
                                      uint8_t format,
                                      uint32_t data_len,
                                      const void* data) {
    RequestBuilder request(opcode::change_property, mode);
    request.card32(window)
        .card32(property)
        .card32(type)
        .card8(format)
        .pad(3)
        .card32(data_len)
        .append(data, static_cast<size_t>(data_len) * (format / 8));

    return void_request(c, request);
}

xcb_void_cookie_t xcb_delete_property(xcb_connection_t* c,
                                      xcb_window_t window,
                                      xcb_atom_t property) {
    RequestBuilder request(opcode::delete_property);
    request.card32(window).card32(property);

    return void_request(c, request);
}

xcb_get_property_cookie_t xcb_get_property(xcb_connection_t* c,
                                           uint8_t _delete,
                                           xcb_window_t window,
                                           xcb_atom_t property,
                                           xcb_atom_t type,
                                           uint32_t long_offset,
                                           uint32_t long_length) {
    RequestBuilder request(opcode::get_property, _delete);
    request.card32(window)
        .card32(property)
        .card32(type)
        .card32(long_offset)
        .card32(long_length);

    return xcb_get_property_cookie_t{reply_request(c, request)};
}

xcb_get_property_reply_t* xcb_get_property_reply(
    xcb_connection_t* c,
    xcb_get_property_cookie_t cookie,
    xcb_generic_error_t** e) {
    return reply_or_null<xcb_get_property_reply_t>(c, cookie.sequence, e);
}

void* xcb_get_property_value(const xcb_get_property_reply_t* R) {
    return const_cast<xcb_get_property_reply_t*>(R) + 1;
}

int xcb_get_property_value_length(const xcb_get_property_reply_t* R) {
    return static_cast<int>(R->value_len * (R->format / 8));
}

xcb_void_cookie_t xcb_set_selection_owner(xcb_connection_t* c,
                                          xcb_window_t owner,
                                          xcb_atom_t selection,
                                          xcb_timestamp_t time) {
    RequestBuilder request(opcode::set_selection_owner);
    request.card32(owner).card32(selection).card32(time);

    return void_request(c, request);
}

xcb_void_cookie_t xcb_send_event(xcb_connection_t* c,
                                 uint8_t propagate,
                                 xcb_window_t destination,
                                 uint32_t event_mask,
                                 const char* event) {
    RequestBuilder request(opcode::send_event, propagate);
    request.card32(destination).card32(event_mask).append(event, 32);

    return void_request(c, request);
}

xcb_void_cookie_t xcb_grab_key(xcb_connection_t* c,
                               uint8_t owner_events,
                               xcb_window_t grab_window,
                               uint16_t modifiers,
                               xcb_keycode_t key,
                               uint8_t pointer_mode,
                               uint8_t keyboard_mode) {
    RequestBuilder request(opcode::grab_key, owner_events);
    request.card32(grab_window)
        .card16(modifiers)
        .card8(key)
        .card8(pointer_mode)
        .card8(keyboard_mode)
        .pad(3);

    return void_request(c, request);
}

xcb_void_cookie_t xcb_ungrab_key(xcb_connection_t* c,
                                 xcb_keycode_t key,
                                 xcb_window_t grab_window,
                                 uint16_t modifiers) {
    RequestBuilder request(opcode::ungrab_key, key);
    request.card32(grab_window).card16(modifiers).pad(2);

    return void_request(c, request);
}

xcb_query_pointer_cookie_t xcb_query_pointer(xcb_connection_t* c,
                                             xcb_window_t window) {
    RequestBuilder request(opcode::query_pointer);
    request.card32(window);

    return xcb_query_pointer_cookie_t{reply_request(c, request)};
}

xcb_query_pointer_reply_t* xcb_query_pointer_reply(
    xcb_connection_t* c,
    xcb_query_pointer_cookie_t cookie,
    xcb_generic_error_t** e) {
    return reply_or_null<xcb_query_pointer_reply_t>(c, cookie.sequence, e);
}

xcb_translate_coordinates_cookie_t xcb_translate_coordinates(
    xcb_connection_t* c,
    xcb_window_t src_window,
    xcb_window_t dst_window,
    int16_t src_x,
    int16_t src_y) {
    RequestBuilder request(opcode::translate_coordinates);
    request.card32(src_window)
        .card32(dst_window)
        .card16(static_cast<uint16_t>(src_x))
        .card16(static_cast<uint16_t>(src_y));

    return xcb_translate_coordinates_cookie_t{reply_request(c, request)};
}

xcb_translate_coordinates_reply_t* xcb_translate_coordinates_reply(
    xcb_connection_t* c,
    xcb_translate_coordinates_cookie_t cookie,
    xcb_generic_error_t** e) {
    return reply_or_null<xcb_translate_coordinates_reply_t>(c, cookie.sequence,
                                                            e);
}

xcb_void_cookie_t xcb_set_input_focus(xcb_connection_t* c,
                                      uint8_t revert_to,
                                      xcb_window_t focus,
                                      xcb_timestamp_t time) {
    RequestBuilder request(opcode::set_input_focus, revert_to);
    request.card32(focus).card32(time);

    return void_request(c, request);
}

xcb_get_input_focus_cookie_t xcb_get_input_focus(xcb_connection_t* c) {
    RequestBuilder request(opcode::get_input_focus);

    return xcb_get_input_focus_cookie_t{reply_request(c, request)};
}

xcb_get_input_focus_reply_t* xcb_get_input_focus_reply(
    xcb_connection_t* c,
    xcb_get_input_focus_cookie_t cookie,
    xcb_generic_error_t** e) {
    return reply_or_null<xcb_get_input_focus_reply_t>(c, cookie.sequence, e);
}

xcb_get_keyboard_mapping_cookie_t xcb_get_keyboard_mapping(
    xcb_connection_t* c,
    xcb_keycode_t first_keycode,
    uint8_t count) {
    RequestBuilder request(opcode::get_keyboard_mapping);
    request.card8(first_keycode).card8(count).pad(2);

    return xcb_get_keyboard_mapping_cookie_t{reply_request(c, request)};
}

xcb_get_keyboard_mapping_reply_t* xcb_get_keyboard_mapping_reply(
    xcb_connection_t* c,
    xcb_get_keyboard_mapping_cookie_t cookie,
    xcb_generic_error_t** e) {
    return reply_or_null<xcb_get_keyboard_mapping_reply_t>(c, cookie.sequence,
                                                           e);
}

xcb_keysym_t* xcb_get_keyboard_mapping_keysyms(
    const xcb_get_keyboard_mapping_reply_t* R) {
    return reinterpret_cast<xcb_keysym_t*>(
        const_cast<xcb_get_keyboard_mapping_reply_t*>(R) + 1);
}

int xcb_get_keyboard_mapping_keysyms_length(
    const xcb_get_keyboard_mapping_reply_t* R) {
    return static_cast<int>(R->length);
}
