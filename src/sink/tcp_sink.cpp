#include "sink/tcp_sink.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace jalert::sink {

namespace {
void close_fd(int& fd) {
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        fd = -1;
    }
}
} // namespace

TcpJsonlSink::TcpJsonlSink(int port) : port_(port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        error_ = std::strerror(errno);
        status_ = "error: " + error_;
        return;
    }
    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error_ = std::strerror(errno);
        status_ = "error: " + error_;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    if (::listen(listen_fd_, 8) < 0) {
        error_ = std::strerror(errno);
        status_ = "error: " + error_;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    status_ = "listening on 0.0.0.0:" + std::to_string(port);
    accepter_ = std::thread([this] { accept_loop(); });
}

TcpJsonlSink::~TcpJsonlSink() { stop(); }

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
        socklen_t plen = sizeof(peer);
        const int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0) return;
        if (stop_.load(std::memory_order_acquire)) { ::close(cfd); return; }
        const int one = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

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

TcpJsonlSink::Client::Client(int fd, TcpJsonlSink* owner) : fd_(fd), owner_(owner) {}
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
            const ssize_t n = ::send(fd_, data, remaining, MSG_NOSIGNAL);
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
        // The writer flagged the client closed after a send error; once it has
        // exited, release the socket too (it used to leak here).
        if (writer_.joinable() && std::this_thread::get_id() != writer_.get_id())
            writer_.join();
        if (!writer_.joinable()) close_fd(fd_);
        return;
    }
    q_cv_.notify_all();
    close_fd(fd_);
    if (writer_.joinable() && std::this_thread::get_id() != writer_.get_id())
        writer_.join();
}

} // namespace jalert::sink
