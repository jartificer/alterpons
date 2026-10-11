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

#include "transport.h"

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>
#include <system_error>

#include <asio/detail/socket_ops.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <ghc/filesystem.hpp>

#ifdef YABRIDGE_PE_HOST
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <asio/local/stream_protocol.hpp>
#endif

#include "../utils.h"

namespace fs = ghc::filesystem;

namespace {

/**
 * The size of the nonce the listening side sends to the connecting side, and
 * the size of the response the connecting side sends back.
 */
constexpr size_t handshake_size = 32;

/**
 * A minimal SHA-256 implementation, used for the challenge-response handshake
 * on TCP connections. This way the authentication token itself never gets sent
 * over the socket, so a process that manages to listen on a stale port that
 * one of our port files still points to does not learn anything useful.
 */
class Sha256 {
   public:
    void update(const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; i++) {
            block_[block_size_++] = data[i];
            if (block_size_ == 64) {
                transform();
                total_bits_ += 512;
                block_size_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish() {
        const uint64_t total_bits = total_bits_ + (block_size_ * 8);

        block_[block_size_++] = 0x80;
        if (block_size_ > 56) {
            while (block_size_ < 64) {
                block_[block_size_++] = 0;
            }
            transform();
            block_size_ = 0;
        }
        while (block_size_ < 56) {
            block_[block_size_++] = 0;
        }
        for (int i = 7; i >= 0; i--) {
            block_[block_size_++] = static_cast<uint8_t>(total_bits >> (i * 8));
        }
        transform();

        std::array<uint8_t, 32> digest{};
        for (size_t i = 0; i < 8; i++) {
            digest[(i * 4) + 0] = static_cast<uint8_t>(state_[i] >> 24);
            digest[(i * 4) + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[(i * 4) + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[(i * 4) + 3] = static_cast<uint8_t>(state_[i]);
        }

        return digest;
    }

   private:
    static uint32_t rotr(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    void transform() {
        static constexpr std::array<uint32_t, 64> k{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(block_[(i * 4) + 0]) << 24) |
                   (static_cast<uint32_t>(block_[(i * 4) + 1]) << 16) |
                   (static_cast<uint32_t>(block_[(i * 4) + 2]) << 8) |
                   static_cast<uint32_t>(block_[(i * 4) + 3]);
        }
        for (size_t i = 16; i < 64; i++) {
            const uint32_t s0 =
                rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 =
                rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3],
                 e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (size_t i = 0; i < 64; i++) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                   0xa54ff53a, 0x510e527f, 0x9b05688c,
                                   0x1f83d9ab, 0x5be0cd19};
    std::array<uint8_t, 64> block_{};
    size_t block_size_ = 0;
    uint64_t total_bits_ = 0;
};

/**
 * The response the connecting side should send for a nonce:
 * `SHA256("yabridge-tcp-auth" || token || nonce)`.
 */
std::array<uint8_t, handshake_size> handshake_response(
    const std::array<uint8_t, handshake_size>& nonce) {
    static constexpr char domain[] = "yabridge-tcp-auth";
    const std::string& token = get_tcp_auth_token();

    Sha256 hasher;
    hasher.update(reinterpret_cast<const uint8_t*>(domain), sizeof(domain) - 1);
    hasher.update(reinterpret_cast<const uint8_t*>(token.data()), token.size());
    hasher.update(nonce.data(), nonce.size());

    return hasher.finish();
}

/**
 * Fill a buffer with cryptographically secure random bytes.
 */
void random_bytes(uint8_t* data, size_t size) {
    // libstdc++ uses `getrandom()`/`/dev/urandom` on Linux and `rand_s()` on
    // MinGW for `std::random_device`, both of which are suitable for this.
    std::random_device random_device;
    for (size_t i = 0; i < size; i++) {
        data[i] = static_cast<uint8_t>(random_device());
    }
}

std::string random_hex_string(size_t num_bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";

    std::vector<uint8_t> bytes(num_bytes);
    random_bytes(bytes.data(), bytes.size());

    std::string result;
    result.reserve(num_bytes * 2);
    for (const uint8_t byte : bytes) {
        result.push_back(hex_chars[byte >> 4]);
        result.push_back(hex_chars[byte & 0xf]);
    }

    return result;
}

bool is_valid_token(const std::string& token) {
    return token.size() == tcp_auth_token_length &&
           token.find_first_not_of("0123456789abcdef") == std::string::npos;
}

#ifndef YABRIDGE_PE_HOST
/**
 * Read the token from the token file. Returns an empty string if the file does
 * not exist. Throws if the file exists but it can't be trusted.
 */
std::string read_token_file(const fs::path& token_path) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int fd = open(token_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd == -1) {
        if (errno == ENOENT) {
            return "";
        }

        throw std::system_error(errno, std::system_category(),
                                "Could not open '" + token_path.string() + "'");
    }

    struct stat file_info {};
    if (fstat(fd, &file_info) != 0 || file_info.st_uid != getuid() ||
        (file_info.st_mode & 077) != 0 || !S_ISREG(file_info.st_mode)) {
        close(fd);
        throw std::runtime_error(
            "Refusing to use '" + token_path.string() +
            "' since it's not a regular file that's only accessible by the "
            "current user");
    }

    std::string token(tcp_auth_token_length, '\0');
    const ssize_t bytes_read = read(fd, token.data(), token.size());
    close(fd);

    if (bytes_read != static_cast<ssize_t>(tcp_auth_token_length)) {
        // This can happen if another process is still writing the file, but
        // we write it using `link()` so this should not happen
        token.clear();
    }

    return token;
}

std::string load_or_create_token() {
    const fs::path token_path =
        get_temporary_directory() /
        ("yabridge-tcp-token-" + std::to_string(getuid()));

    if (std::string token = read_token_file(token_path);
        is_valid_token(token)) {
        return token;
    }

    // The token is written to a temporary file first, which we then try to
    // hard link to its final location. `link()` fails if the target already
    // exists, so if another process is doing the same thing at the same time
    // then only one of them will win and everyone will end up reading the same
    // token.
    const std::string new_token = random_hex_string(tcp_auth_token_length / 2);
    const fs::path temp_path =
        token_path.string() + "." + random_hex_string(8) + ".tmp";
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const int fd =
            open(temp_path.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd == -1) {
            throw std::system_error(
                errno, std::system_category(),
                "Could not create '" + temp_path.string() + "'");
        }

        const ssize_t bytes_written =
            write(fd, new_token.data(), new_token.size());
        close(fd);
        if (bytes_written != static_cast<ssize_t>(new_token.size())) {
            unlink(temp_path.c_str());
            throw std::runtime_error("Could not write '" + temp_path.string() +
                                     "'");
        }
    }

    // If this fails, then another process created the file in the meantime
    // and we'll just read that file instead
    [[maybe_unused]] const int link_result =
        link(temp_path.c_str(), token_path.c_str());
    unlink(temp_path.c_str());

    std::string token = read_token_file(token_path);
    if (!is_valid_token(token)) {
        throw std::runtime_error("Could not read the token from '" +
                                 token_path.string() + "'");
    }

    return token;
}
#endif

/**
 * Write `contents` to `path` atomically by first writing it to a temporary file
 * in the same directory, and then renaming that file to `path`.
 */
void write_file_atomically(const std::string& path,
                           const std::string& contents) {
    const fs::path target_path(path);
    const fs::path temp_path =
        target_path.string() + "." + random_hex_string(8) + ".tmp";

    {
        fs::ofstream file(temp_path, std::ios::out | std::ios::trunc);
        file << contents;
        file.close();
        if (file.fail()) {
            std::error_code err;
            fs::remove(temp_path, err);
            throw std::system_error(
                std::make_error_code(std::errc::io_error),
                "Could not write to '" + temp_path.string() + "'");
        }
    }

    std::error_code err;
    fs::rename(temp_path, target_path, err);
    if (err) {
        std::error_code ignored;
        fs::remove(temp_path, ignored);
        throw std::system_error(
            err, "Could not create '" + target_path.string() + "'");
    }
}

/**
 * How long we'll wait for the other side during the TCP handshake. A stale port
 * file could point to a port that's now used by some unrelated program, and we
 * don't want to hang indefinitely when that happens.
 */
constexpr int handshake_timeout_ms = 10'000;

/**
 * Read exactly `buffer.size()` bytes from the socket, failing if the other side
 * takes longer than `handshake_timeout_ms` to send them.
 *
 * Signals interrupting the wait are not errors. The native plugin library runs
 * on the host's threads, and `poll()` is never restarted after a signal handler
 * runs, even when that handler was installed with `SA_RESTART`. Failing the
 * handshake there would make yabridge think that the Wine plugin host died.
 */
void read_handshake(Socket& socket,
                    std::array<uint8_t, handshake_size>& buffer) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(handshake_timeout_ms);

    size_t bytes_read = 0;
    while (bytes_read < buffer.size()) {
        const auto time_left =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (time_left <= 0) {
            throw std::system_error(std::make_error_code(std::errc::timed_out),
                                    "Timed out during socket handshake");
        }

        std::error_code err;
        const int ready = asio::detail::socket_ops::poll_read(
            socket.native_handle(), 0, static_cast<int>(time_left), err);
        if (err == asio::error::interrupted) {
            continue;
        }
        if (err) {
            throw std::system_error(err, "Error during socket handshake");
        }
        if (ready == 0) {
            continue;
        }

        bytes_read += socket.read_some(asio::buffer(buffer.data() + bytes_read,
                                                    buffer.size() - bytes_read),
                                       err);
        if (err && err != asio::error::interrupted) {
            throw std::system_error(err, "Error during socket handshake");
        }
    }
}

asio::ip::tcp::endpoint loopback_endpoint(uint16_t port) {
    return asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port);
}

}  // namespace

const std::string& get_tcp_auth_token() {
    static std::mutex token_mutex;
    static std::string token;

    std::lock_guard lock(token_mutex);
    if (token.empty()) {
#ifdef YABRIDGE_PE_HOST
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        const char* env_token = getenv(tcp_auth_token_env_var);
        if (!env_token || !is_valid_token(env_token)) {
            throw std::runtime_error(
                "The '" + std::string(tcp_auth_token_env_var) +
                "' environment variable has not been set, this host should "
                "be launched by yabridge");
        }

        token = env_token;
#else
        token = load_or_create_token();
#endif
    }

    return token;
}

void configure_socket(Socket& socket, SocketTransport transport) {
    if (transport == SocketTransport::tcp_loopback) {
        socket.set_option(asio::ip::tcp::no_delay(true));
    }
}

SocketEndpoint::SocketEndpoint(std::string path, SocketTransport transport)
    : path_(std::move(path)), transport_(transport) {}

SocketEndpoint::SocketEndpoint(const char* path, SocketTransport transport)
    : path_(path), transport_(transport) {}

uint16_t SocketEndpoint::read_port() const {
    fs::ifstream file{fs::path(path_)};
    unsigned long port = 0;
    if (!(file >> port) || port == 0 || port > 65535) {
        throw std::system_error(
            std::make_error_code(std::errc::connection_refused),
            "No valid TCP port file at '" + path_ + "'");
    }

    return static_cast<uint16_t>(port);
}

void SocketEndpoint::connect(Socket& socket) const {
    switch (transport_) {
        case SocketTransport::unix_domain: {
#ifdef YABRIDGE_PE_HOST
            throw std::system_error(
                std::make_error_code(std::errc::address_family_not_supported),
                "Unix domain sockets are not supported in this build");
#else
            socket.connect(asio::generic::stream_protocol::endpoint(
                asio::local::stream_protocol::endpoint(path_)));
#endif
        } break;
        case SocketTransport::tcp_loopback: {
            socket.connect(asio::generic::stream_protocol::endpoint(
                loopback_endpoint(read_port())));
            configure_socket(socket, transport_);

            std::array<uint8_t, handshake_size> nonce{};
            read_handshake(socket, nonce);
            const auto response = handshake_response(nonce);
            asio::write(socket, asio::buffer(response));
        } break;
    }
}

SocketAcceptor SocketEndpoint::listen(asio::io_context& io_context) const {
    switch (transport_) {
        case SocketTransport::unix_domain: {
#ifdef YABRIDGE_PE_HOST
            throw std::system_error(
                std::make_error_code(std::errc::address_family_not_supported),
                "Unix domain sockets are not supported in this build");
#else
            return SocketAcceptor(
                io_context, asio::generic::stream_protocol::endpoint(
                                asio::local::stream_protocol::endpoint(path_)));
#endif
        } break;
        case SocketTransport::tcp_loopback: {
            // Binding to port 0 makes the OS pick a free port for us.
            // `reuse_address` is explicitly disabled since on Windows that
            // would allow other processes to bind to the same port.
            const asio::generic::stream_protocol::endpoint endpoint(
                loopback_endpoint(0));
            SocketAcceptor acceptor(io_context);
            acceptor.open(endpoint.protocol());
            acceptor.bind(endpoint);
            acceptor.listen();

            // The OS picked a port for us, and the other side can find that
            // port through the port file
            asio::ip::tcp::endpoint bound_endpoint;
            const asio::generic::stream_protocol::endpoint local_endpoint =
                acceptor.local_endpoint();
            std::memcpy(bound_endpoint.data(), local_endpoint.data(),
                        local_endpoint.size());
            write_file_atomically(path_,
                                  std::to_string(bound_endpoint.port()) + "\n");

            return acceptor;
        } break;
    }

    // Unreachable
    throw std::logic_error("Unknown socket transport");
}

void SocketEndpoint::finish_accept(Socket& socket) const {
    if (transport_ != SocketTransport::tcp_loopback) {
        return;
    }

    try {
        configure_socket(socket, transport_);

        std::array<uint8_t, handshake_size> nonce{};
        random_bytes(nonce.data(), nonce.size());
        asio::write(socket, asio::buffer(nonce));

        std::array<uint8_t, handshake_size> response{};
        read_handshake(socket, response);

        // Constant time comparison, even though this probably doesn't matter
        // much here
        const auto expected_response = handshake_response(nonce);
        uint8_t difference = 0;
        for (size_t i = 0; i < handshake_size; i++) {
            difference |= response[i] ^ expected_response[i];
        }

        if (difference != 0) {
            throw std::system_error(
                std::make_error_code(std::errc::permission_denied),
                "Rejected an unauthenticated connection on '" + path_ + "'");
        }
    } catch (...) {
        std::error_code err;
        socket.close(err);
        throw;
    }
}

void SocketEndpoint::accept(SocketAcceptor& acceptor,
                            Socket& socket,
                            const std::atomic_bool* abort) const {
    while (true) {
        // When we can be aborted, we'll wait for incoming connections in small
        // increments so we can check whether we should stop waiting
        if (abort) {
            while (true) {
                if (*abort) {
                    throw std::system_error(
                        std::make_error_code(std::errc::operation_canceled),
                        "Stopped waiting for a connection on '" + path_ + "'");
                }

                std::error_code err;
                const int ready = asio::detail::socket_ops::poll_read(
                    acceptor.native_handle(), 0, 100, err);
                if (err == asio::error::interrupted) {
                    continue;
                }
                if (err) {
                    throw std::system_error(err,
                                            "Error while waiting for a "
                                            "connection on '" +
                                                path_ + "'");
                }
                if (ready > 0) {
                    break;
                }
            }
        }

        // If the acceptor gets closed, then this will throw
        acceptor.accept(socket);

        try {
            finish_accept(socket);
            return;
        } catch (const std::system_error&) {
            // Either some other process connected to our port, or the other
            // side hung up during the handshake. In both cases we'll just wait
            // for the next connection.
        }
    }
}

void SocketEndpoint::remove() const noexcept {
    std::error_code err;
    fs::remove(fs::path(path_), err);
}

bool SocketEndpoint::is_active(asio::io_context& io_context) const {
    try {
        Socket socket(io_context);
        switch (transport_) {
            case SocketTransport::unix_domain:
#ifdef YABRIDGE_PE_HOST
                return false;
#else
                socket.connect(asio::generic::stream_protocol::endpoint(
                    asio::local::stream_protocol::endpoint(path_)));
#endif
                break;
            case SocketTransport::tcp_loopback:
                // We don't do the handshake here, the other side will simply
                // drop this connection
                socket.connect(asio::generic::stream_protocol::endpoint(
                    loopback_endpoint(read_port())));
                break;
        }

        std::error_code err;
        socket.close(err);

        return true;
    } catch (const std::system_error&) {
        return false;
    }
}
