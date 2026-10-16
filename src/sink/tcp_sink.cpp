#include "sink/tcp_sink.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace jalert::sink {

namespace {
#ifdef _WIN32
using sock_len_t = int;
constexpr int kShutBoth = SD_BOTH;
constexpr int kSendFlags = 0;  // Windows never raises SIGPIPE

SOCKET to_os(SockFd fd) { return static_cast<SOCKET>(fd); }
SockFd from_os(SOCKET s) { return s == INVALID_SOCKET ? -1 : static_cast<SockFd>(s); }
void close_os(SockFd fd) { ::closesocket(to_os(fd)); }

std::string last_socket_error() {
    const int code = ::WSAGetLastError();
    char buf[256] = {};
    const DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(code), MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
        buf, sizeof(buf), nullptr);
    std::string msg(buf, n);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == '.'))
        msg.pop_back();
    if (msg.empty()) msg = "socket error";
    return msg + " (WSA " + std::to_string(code) + ")";
}

// WSAStartup is reference counted; each sink instance holds one reference.
bool net_init(std::string& err) {
    WSADATA wsa;
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        err = "WSAStartup failed (" + std::to_string(rc) + ")";
        return false;
    }
    return true;
}
void net_cleanup() { ::WSACleanup(); }
#else
using sock_len_t = socklen_t;
constexpr int kShutBoth = SHUT_RDWR;
constexpr int kSendFlags = MSG_NOSIGNAL;

int to_os(SockFd fd) { return static_cast<int>(fd); }
SockFd from_os(int s) { return s < 0 ? -1 : static_cast<SockFd>(s); }
void close_os(SockFd fd) { ::close(to_os(fd)); }
std::string last_socket_error() { return std::strerror(errno); }
bool net_init(std::string&) { return true; }
void net_cleanup() {}
#endif

void close_fd(SockFd& fd) {
    if (fd >= 0) {
        ::shutdown(to_os(fd), kShutBoth);
        close_os(fd);
        fd = -1;
    }
}
} // namespace

TcpJsonlSink::TcpJsonlSink(int port) : port_(port) {
    if (!net_init(error_)) {
        status_ = "error: " + error_;
        return;
    }
    net_ready_ = true;
#ifdef _WIN32
    listen_fd_ = from_os(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
#else
    listen_fd_ = from_os(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
#endif
    if (listen_fd_ < 0) {
        error_ = last_socket_error();
        status_ = "error: " + error_;
        return;
    }
    const int one = 1;
#ifdef _WIN32
    // Windows SO_REUSEADDR lets another process steal the port; bind exclusively.
    ::setsockopt(to_os(listen_fd_), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(to_os(listen_fd_), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(to_os(listen_fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error_ = last_socket_error();
        status_ = "error: " + error_;
        close_os(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    if (::listen(to_os(listen_fd_), 8) < 0) {
        error_ = last_socket_error();
        status_ = "error: " + error_;
        close_os(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    status_ = "listening on 0.0.0.0:" + std::to_string(port);
    accepter_ = std::thread([this] { accept_loop(); });
}

TcpJsonlSink::~TcpJsonlSink() {
    stop();
    if (net_ready_) net_cleanup();
}

std::string TcpJsonlSink::status() const {
    std::lock_guard<std::mutex> lk(gate_);
    return status_ + "; clients=" + std::to_string(clients_.size());
}

TcpSinkSnapshot TcpJsonlSink::snapshot() const {
    std::lock_guard<std::mutex> lk(gate_);
    TcpSinkSnapshot s;
    s.port = port_;
    s.client_count = static_cast<int>(clients_.size());
    s.bytes_sent = bytes_sent_;
    s.error_message = error_;
    if (!error_.empty())          s.phase = SinkPhase::Error;
    else if (listen_fd_ >= 0)     s.phase = SinkPhase::Active;
    else                          s.phase = SinkPhase::Off;
    return s;
}

void TcpJsonlSink::accept_loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        sock_len_t plen = sizeof(peer);
        const SockFd cfd = from_os(
            ::accept(to_os(listen_fd_), reinterpret_cast<sockaddr*>(&peer), &plen));
        if (cfd < 0) return;
        if (stop_.load(std::memory_order_acquire)) { close_os(cfd); return; }
        const int one = 1;
        ::setsockopt(to_os(cfd), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));

        auto client = std::make_unique<Client>(cfd, this);
        Client* raw = client.get();
        {
            std::lock_guard<std::mutex> lk(gate_);
            clients_.push_back(std::move(client));
        }
        raw->start();
    }
}

void TcpJsonlSink::write_line(const std::string& line) {
    if (line.empty()) return;
    auto shared = std::make_shared<std::string>(line);
    shared->push_back('\n');

    std::lock_guard<std::mutex> lk(gate_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        if ((*it)->is_closed()) {
            (*it)->shutdown();
            it = clients_.erase(it);
        } else {
            (*it)->enqueue(shared);
            ++it;
        }
    }
}

void TcpJsonlSink::stop() {
    if (stop_.exchange(true, std::memory_order_acq_rel)) return;
    std::vector<std::unique_ptr<Client>> taken;
    {
        std::lock_guard<std::mutex> lk(gate_);
        status_ = "off";
        close_fd(listen_fd_);
        taken.swap(clients_);
    }
    for (auto& c : taken) c->shutdown();
    if (accepter_.joinable()) accepter_.join();
}

void TcpJsonlSink::on_bytes_sent(long long n) {
    std::lock_guard<std::mutex> lk(gate_);
    bytes_sent_ += n;
}

TcpJsonlSink::Client::Client(SockFd fd, TcpJsonlSink* owner) : fd_(fd), owner_(owner) {}
TcpJsonlSink::Client::~Client() { shutdown(); }

void TcpJsonlSink::Client::start() {
    writer_ = std::thread([this] { write_loop(); });
}

void TcpJsonlSink::Client::enqueue(const std::shared_ptr<std::string>& line) {
    if (closed_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(q_gate_);
        while (static_cast<int>(queue_.size()) >= kMaxQueuedLines) queue_.pop_front();
        queue_.push_back(line);
    }
    q_cv_.notify_one();
}

void TcpJsonlSink::Client::write_loop() {
    while (!closed_.load(std::memory_order_acquire)) {
        std::shared_ptr<std::string> chunk;
        {
            std::unique_lock<std::mutex> lk(q_gate_);
            q_cv_.wait(lk, [this] {
                return closed_.load(std::memory_order_acquire) || !queue_.empty();
            });
            if (closed_.load(std::memory_order_acquire)) return;
            chunk = std::move(queue_.front());
            queue_.pop_front();
        }
        const char* data = chunk->data();
        size_t remaining = chunk->size();
        while (remaining > 0) {
#ifdef _WIN32
            const int want = remaining > 0x7fffffff ? 0x7fffffff : static_cast<int>(remaining);
            const long long n = ::send(to_os(fd_), data, want, kSendFlags);
#else
            const long long n = ::send(to_os(fd_), data, remaining, kSendFlags);
#endif
            if (n <= 0) {
                closed_.store(true, std::memory_order_release);
                return;
            }
            data += n;
            remaining -= static_cast<size_t>(n);
            owner_->on_bytes_sent(static_cast<long long>(n));
        }
    }
}

void TcpJsonlSink::Client::shutdown() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        if (writer_.joinable() && std::this_thread::get_id() != writer_.get_id())
            writer_.join();
        return;
    }
    q_cv_.notify_all();
    close_fd(fd_);
    if (writer_.joinable() && std::this_thread::get_id() != writer_.get_id())
        writer_.join();
}

} // namespace jalert::sink
