// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/system.h>
#include <compat/compat.h>
#include <test/util/setup_common.h>
#include <util/sock.h>
#include <util/threadinterrupt.h>

#include <boost/test/unit_test.hpp>

#include <cassert>
#include <thread>

using namespace std::chrono_literals;

BOOST_FIXTURE_TEST_SUITE(sock_tests, BasicTestingSetup)

static bool SocketIsClosed(const SOCKET& s)
{
    // Notice that if another thread is running and creates its own socket after `s` has been
    // closed, it may be assigned the same file descriptor number. In this case, our test will
    // wrongly pretend that the socket is not closed.
    int type;
    socklen_t len = sizeof(type);
    return getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &len) == SOCKET_ERROR;
}

static SOCKET CreateSocket()
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOST_REQUIRE(s != static_cast<SOCKET>(SOCKET_ERROR));
    return s;
}

BOOST_AUTO_TEST_CASE(constructor_and_destructor)
{
    const SOCKET s = CreateSocket();
    Sock* sock = new Sock(s);
    BOOST_CHECK(*sock == s);
    BOOST_CHECK(!SocketIsClosed(s));
    delete sock;
    BOOST_CHECK(SocketIsClosed(s));
}

BOOST_AUTO_TEST_CASE(move_constructor)
{
    const SOCKET s = CreateSocket();
    Sock* sock1 = new Sock(s);
    Sock* sock2 = new Sock(std::move(*sock1));
    delete sock1;
    BOOST_CHECK(!SocketIsClosed(s));
    BOOST_CHECK(*sock2 == s);
    delete sock2;
    BOOST_CHECK(SocketIsClosed(s));
}

BOOST_AUTO_TEST_CASE(move_assignment)
{
    const SOCKET s1 = CreateSocket();
    const SOCKET s2 = CreateSocket();
    Sock* sock1 = new Sock(s1);
    Sock* sock2 = new Sock(s2);

    BOOST_CHECK(!SocketIsClosed(s1));
    BOOST_CHECK(!SocketIsClosed(s2));

    *sock2 = std::move(*sock1);
    BOOST_CHECK(!SocketIsClosed(s1));
    BOOST_CHECK(SocketIsClosed(s2));
    BOOST_CHECK(*sock2 == s1);

    delete sock1;
    BOOST_CHECK(!SocketIsClosed(s1));
    BOOST_CHECK(SocketIsClosed(s2));
    BOOST_CHECK(*sock2 == s1);

    delete sock2;
    BOOST_CHECK(SocketIsClosed(s1));
    BOOST_CHECK(SocketIsClosed(s2));
}

BOOST_AUTO_TEST_CASE(tcp_info)
{
    const SOCKET s1 = CreateSocket();
    Sock* sock1 = new Sock(s1);
    TCPInfo sock1_info{*sock1};
    // Test that we can acquire a valid TCP_INFO structure on all
    // supported platforms.
    BOOST_CHECK(sock1_info.m_valid);
    BOOST_CHECK(!SocketIsClosed(s1));

    BOOST_CHECK(sock1_info.GetTCPWindowSize());

    delete sock1;
}

#ifndef WIN32 // Windows does not have socketpair(2).

static std::pair<Sock, Sock> CreateSocketPair()
{
    int s[2];
    BOOST_REQUIRE_EQUAL(socketpair(AF_UNIX, SOCK_STREAM, 0, s), 0);
    return std::pair<Sock, Sock>{ s[0], s[1] };
}

#else

static std::pair<Sock, Sock> CreateSocketPair()
{
    const SOCKET listener = CreateSocket();
    const SOCKET sender = CreateSocket();

    Sock sock_listener{listener};
    Sock sock_sender{sender};

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    BOOST_CHECK_EQUAL(sock_listener.Bind(reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    BOOST_CHECK_EQUAL(sock_listener.Listen(1), 0);

    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    BOOST_CHECK_EQUAL(sock_listener.GetSockName(reinterpret_cast<sockaddr*>(&bound), &blen), 0);

    BOOST_CHECK_EQUAL(sock_sender.Connect(reinterpret_cast<sockaddr*>(&bound), sizeof(bound)), 0);

    std::unique_ptr<Sock> accepted = sock_listener.Accept(nullptr, nullptr);
    Sock sock_accepted = std::move(*accepted);

    return std::pair<Sock, Sock>{std::move(sock_sender), std::move(sock_accepted)};
}

#endif

static void SendAndRecvMessage(const Sock& sender, const Sock& receiver)
{
    const char* msg = "abcd";
    constexpr ssize_t msg_len = 4;
    char recv_buf[10];

    BOOST_CHECK_EQUAL(sender.Send(msg, msg_len, 0), msg_len);
    BOOST_CHECK_EQUAL(receiver.Recv(recv_buf, sizeof(recv_buf), 0), msg_len);
    BOOST_CHECK_EQUAL(strncmp(msg, recv_buf, msg_len), 0);
}

BOOST_AUTO_TEST_CASE(send_and_receive)
{
    Sock *sock0moved, *sock1moved;
    {
    auto [sock0, sock1] = CreateSocketPair();

    SendAndRecvMessage(sock0, sock1);

    sock0moved = new Sock(std::move(sock0));
    sock1moved = new Sock(INVALID_SOCKET);
    *sock1moved = std::move(sock1);
    }

    SendAndRecvMessage(*sock1moved, *sock0moved);

    delete sock0moved;
    delete sock1moved;
}

BOOST_AUTO_TEST_CASE(wait)
{
    auto [sock0, sock1] = CreateSocketPair();

    std::thread waiter([&sock0]() { (void)sock0.Wait(24h, Sock::RECV); });

    BOOST_REQUIRE_EQUAL(sock1.Send("a", 1, 0), 1);

    waiter.join();
}

BOOST_AUTO_TEST_CASE(recv_until_terminator_limit)
{
    constexpr auto timeout = 1min; // High enough so that it is never hit.
    CThreadInterrupt interrupt;
    auto [sock_send, sock_recv] = CreateSocketPair();

    std::thread receiver([&sock_recv, &timeout, &interrupt]() {
        constexpr size_t max_data{10};
        bool threw_as_expected{false};
        // BOOST_CHECK_EXCEPTION() writes to some variables shared with the main thread which
        // creates a data race. So mimic it manually.
        try {
            (void)sock_recv.RecvUntilTerminator('\n', timeout, interrupt, max_data);
        } catch (const std::runtime_error& e) {
            threw_as_expected = HasReason("too many bytes without a terminator")(e);
        }
        assert(threw_as_expected);
    });

    BOOST_REQUIRE_NO_THROW(sock_send.SendComplete("1234567", timeout, interrupt));
    BOOST_REQUIRE_NO_THROW(sock_send.SendComplete("89a\n", timeout, interrupt));

    receiver.join();
}

BOOST_AUTO_TEST_SUITE_END()
