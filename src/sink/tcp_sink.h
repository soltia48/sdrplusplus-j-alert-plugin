#pragma once
// TCP JSONL sink. Listens on a port and fans out every received line to each
// connected client via per-client queues drained by background threads, so a
// slow client only loses lines (drop-oldest at kMaxQueuedLines) and never
// blocks the decoder.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sink/sink.h"

namespace jalert::sink {

// Portable socket handle: a file descriptor on POSIX, a SOCKET on Windows.
// -1 means "no socket".
using SockFd = std::intptr_t;

struct TcpSinkSnapshot {
    SinkPhase phase = SinkPhase::Off;
    int port = 0;
    int client_count = 0;
    long long bytes_sent = 0;
    std::string error_message;
};

class TcpJsonlSink : public IJsonlSink {
public:
    explicit TcpJsonlSink(int port);
    ~TcpJsonlSink() override;

    std::string status() const override;
    void write_line(const std::string& line) override;
    void stop() override;

    int port() const { return port_; }
    TcpSinkSnapshot snapshot() const;

private:
    class Client {
    public:
        Client(SockFd fd, TcpJsonlSink* owner);
        ~Client();
        bool is_closed() const { return closed_.load(std::memory_order_acquire); }
        void start();
        void enqueue(const std::shared_ptr<std::string>& line);
        void shutdown();

    private:
        void write_loop();
        SockFd fd_;
        TcpJsonlSink* owner_;
        std::mutex q_gate_;
        std::condition_variable q_cv_;
        std::deque<std::shared_ptr<std::string>> queue_;
        std::atomic<bool> closed_{false};
        std::thread writer_;
    };

    static constexpr int kMaxQueuedLines = 4096;

    void accept_loop();
    void on_bytes_sent(long long n);

    mutable std::mutex gate_;
    int port_;
    SockFd listen_fd_ = -1;
    bool net_ready_ = false;
    std::thread accepter_;
    std::vector<std::unique_ptr<Client>> clients_;
    std::string status_;
    std::string error_;
    long long bytes_sent_ = 0;
    std::atomic<bool> stop_{false};
};

} // namespace jalert::sink
