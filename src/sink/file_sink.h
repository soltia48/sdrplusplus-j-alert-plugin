#pragma once
// Append-mode JSONL file sink. Each line is written straight through (the
// stream is unbuffered) before write_line returns. No fsync per line (a slow
// disk must not stall the decoder).
//
// A record's boundary check, write and rollback run under an exclusive OS lock
// on the file, taken through the handle that does the write. So the file is
// appended to only at the end of a complete record, and a failed write is cut
// back to exactly what it added, even when other sinks — in this process or in
// another SDR++ instance — write to the same file, and even when the path has
// since been renamed and recreated.
//
// Nothing is held between calls: the lock is released when write_line returns,
// and the boundary check at the start of the next write removes a fragment an
// earlier rollback could not. Lock contention is waited out only briefly and
// then reported, so a stuck writer cannot stall the decoder, the GUI or
// shutdown.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "sink/sink.h"

namespace jalert::sink {

struct FileSinkSnapshot {
    SinkPhase phase = SinkPhase::Off;
    std::string path;
    long long bytes_written = 0;
    long long records_written = 0;
    std::string error_message;
};

class FileJsonlSink : public IJsonlSink {
public:
    explicit FileJsonlSink(const std::string& path);
    ~FileJsonlSink() override;

    std::string status() const override;
    void write_line(const std::string& line) override;
    void stop() override;

    FileSinkSnapshot snapshot() const;

private:
    mutable std::mutex gate_;
    std::string path_;
    bool open_locked();                           // gate_ held
    bool repair_tail_locked(std::string& err);    // gate_ and file lock held
    void close_locked();                          // gate_ held
    void ok_locked();                             // gate_ held
    void fail_locked(const std::string& msg);     // gate_ held
    std::FILE* fp_ = nullptr;
    bool stopped_ = false;
    std::string status_;
    std::string error_;
    long long bytes_written_ = 0;
    long long records_written_ = 0;
};

} // namespace jalert::sink
