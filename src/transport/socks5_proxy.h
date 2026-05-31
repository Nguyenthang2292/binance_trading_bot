#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace transport::socks5 {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

inline asio::awaitable<void> readExact(tcp::socket& socket, asio::mutable_buffer buffer) {
    co_await asio::async_read(socket, buffer, asio::transfer_exactly(buffer.size()), asio::use_awaitable);
}

inline asio::awaitable<void> connectTunnel(tcp::socket& socket,
                                           std::string_view remoteHost,
                                           std::uint16_t remotePort,
                                           std::chrono::seconds timeout = std::chrono::seconds{30}) {
    if (remoteHost.empty() || remoteHost.size() > 255) {
        throw std::runtime_error("invalid SOCKS5 remote host");
    }
    if (timeout <= std::chrono::seconds::zero()) {
        throw std::runtime_error("invalid SOCKS5 tunnel timeout");
    }

    struct DeadlineState {
        std::atomic_bool completed{false};
    };
    auto deadlineState = std::make_shared<DeadlineState>();
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer deadline(executor);
    deadline.expires_after(timeout);
    deadline.async_wait([deadlineState, &socket](const boost::system::error_code& ec) {
        if (!ec && !deadlineState->completed.exchange(true)) {
            boost::system::error_code ignored;
            socket.cancel(ignored);
        }
    });

    auto completeDeadline = [&deadline, deadlineState]() {
        deadlineState->completed = true;
        boost::system::error_code ignored;
        deadline.cancel(ignored);
    };

    try {
        const std::array<std::uint8_t, 3> greeting{0x05, 0x01, 0x00};
        co_await asio::async_write(socket, asio::buffer(greeting), asio::use_awaitable);

        std::array<std::uint8_t, 2> greetingResp{};
        co_await readExact(socket, asio::buffer(greetingResp));
        if (greetingResp[0] != 0x05 || greetingResp[1] != 0x00) {
            throw std::runtime_error("SOCKS5 authentication method rejected");
        }

        std::vector<std::uint8_t> request;
        request.reserve(7 + remoteHost.size());
        request.push_back(0x05);
        request.push_back(0x01); // CONNECT
        request.push_back(0x00);
        request.push_back(0x03); // Domain name
        request.push_back(static_cast<std::uint8_t>(remoteHost.size()));
        request.insert(request.end(), remoteHost.begin(), remoteHost.end());
        request.push_back(static_cast<std::uint8_t>((remotePort >> 8) & 0xFF));
        request.push_back(static_cast<std::uint8_t>(remotePort & 0xFF));
        co_await asio::async_write(socket, asio::buffer(request), asio::use_awaitable);

        std::array<std::uint8_t, 4> responseHeader{};
        co_await readExact(socket, asio::buffer(responseHeader));
        if (responseHeader[0] != 0x05) {
            throw std::runtime_error("invalid SOCKS5 response version");
        }
        if (responseHeader[1] != 0x00) {
            throw std::runtime_error("SOCKS5 CONNECT failed, code " + std::to_string(responseHeader[1]));
        }

        std::size_t bindAddrLen = 0;
        switch (responseHeader[3]) {
            case 0x01: bindAddrLen = 4; break;  // IPv4
            case 0x04: bindAddrLen = 16; break; // IPv6
            case 0x03: {
                std::array<std::uint8_t, 1> len{};
                co_await readExact(socket, asio::buffer(len));
                bindAddrLen = len[0];
                if (bindAddrLen == 0 || bindAddrLen > 255) {
                    throw std::runtime_error("invalid SOCKS5 bound domain length");
                }
                break;
            }
            default: throw std::runtime_error("invalid SOCKS5 address type");
        }

        std::vector<std::uint8_t> bindAddressAndPort(bindAddrLen + 2);
        if (!bindAddressAndPort.empty()) {
            co_await readExact(socket, asio::buffer(bindAddressAndPort));
        }
        completeDeadline();
    } catch (...) {
        completeDeadline();
        throw;
    }
}

} // namespace transport::socks5
