// Regression tests for FileJsonlSink failure recovery.
//
// The partial-write test needs RLIMIT_FSIZE to make a write fail part-way, so
// it only runs on POSIX; the other tests are portable.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "sink/file_sink.h"

#ifndef _WIN32
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using jalert::sink::FileJsonlSink;

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const auto& va = (a);                                                \
        const auto& vb = (b);                                                \
        if (!(va == vb)) {                                                   \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n",       \
                         __FILE__, __LINE__, #a, #b);                        \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

std::string temp_path(const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(p);
    return p.string();
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_raw(const std::string& path, const std::string& data) {
    std::ofstream(path, std::ios::binary) << data;
}

// A file that does not end in '\n' gets a line break before the first record.
void test_unterminated_existing_file() {
    const std::string path = temp_path("jalert_sink_unterminated.jsonl");
    write_raw(path, "{\"old\":1}\n{\"tor");
    {
        FileJsonlSink sink(path);
        sink.write_line("{\"a\":1}");
        CHECK(sink.snapshot().error_message.empty());
        CHECK_EQ(sink.snapshot().records_written, 1LL);
    }
    CHECK_EQ(read_all(path), std::string("{\"old\":1}\n{\"tor\n{\"a\":1}\n"));
    std::filesystem::remove(path);
}

// A properly terminated file is appended to unchanged.
void test_append_existing_file() {
    const std::string path = temp_path("jalert_sink_append.jsonl");
    write_raw(path, "{\"old\":1}\n");
    {
        FileJsonlSink sink(path);
        sink.write_line("{\"a\":1}");
    }
    CHECK_EQ(read_all(path), std::string("{\"old\":1}\n{\"a\":1}\n"));
    std::filesystem::remove(path);
}

void test_no_writes_after_stop() {
    const std::string path = temp_path("jalert_sink_stop.jsonl");
    {
        FileJsonlSink sink(path);
        sink.write_line("{\"a\":1}");
        sink.stop();
        sink.write_line("{\"b\":2}");
        CHECK_EQ(sink.snapshot().records_written, 1LL);
    }
    CHECK_EQ(read_all(path), std::string("{\"a\":1}\n"));
    std::filesystem::remove(path);
}

#ifndef _WIN32
rlim_t set_fsize_limit(rlim_t soft) {
    rlimit rl{};
    getrlimit(RLIMIT_FSIZE, &rl);
    const rlim_t old = rl.rlim_cur;
    rl.rlim_cur = soft;
    setrlimit(RLIMIT_FSIZE, &rl);
    return old;
}

// Partial write -> failure -> recovery -> next record: the incomplete record
// is trimmed, so every line in the file is a complete record.
void test_partial_write_recovery() {
    std::signal(SIGXFSZ, SIG_IGN);   // get EFBIG instead of being killed
    const std::string path = temp_path("jalert_sink_partial.jsonl");
    {
        FileJsonlSink sink(path);
        sink.write_line("{\"zero\":0}");
        CHECK(sink.snapshot().error_message.empty());

        // Room for only part of the next record.
        const rlim_t old = set_fsize_limit(std::filesystem::file_size(path) + 8);
        sink.write_line("{\"first\":1}");
        set_fsize_limit(old);
        CHECK(!sink.snapshot().error_message.empty());
        CHECK_EQ(sink.snapshot().records_written, 1LL);

        sink.write_line("{\"second\":2}");
        CHECK(sink.snapshot().error_message.empty());
        CHECK_EQ(sink.snapshot().records_written, 2LL);
    }
    CHECK_EQ(read_all(path), std::string("{\"zero\":0}\n{\"second\":2}\n"));
    std::filesystem::remove(path);
}

// Two sinks on one file (several module instances share the default output
// path): rolling back A's failed write must not remove B's record.
void test_shared_file_rollback() {
    std::signal(SIGXFSZ, SIG_IGN);
    const std::string path = temp_path("jalert_sink_shared.jsonl");
    {
        FileJsonlSink a(path);
        FileJsonlSink b(path);
        a.write_line("{\"from_a\":1}");
        b.write_line("{\"from_b\":2}");
        CHECK(a.snapshot().error_message.empty());
        CHECK(b.snapshot().error_message.empty());

        const rlim_t old = set_fsize_limit(std::filesystem::file_size(path) + 8);
        a.write_line("{\"failed\":0}");
        set_fsize_limit(old);
        CHECK(!a.snapshot().error_message.empty());
        CHECK_EQ(read_all(path),
                 std::string("{\"from_a\":1}\n{\"from_b\":2}\n"));

        a.write_line("{\"after\":3}");
        b.write_line("{\"from_b\":4}");
        CHECK(a.snapshot().error_message.empty());
        CHECK(b.snapshot().error_message.empty());
        CHECK_EQ(a.snapshot().records_written, 2LL);
        CHECK_EQ(b.snapshot().records_written, 2LL);
    }
    CHECK_EQ(read_all(path),
             std::string("{\"from_a\":1}\n{\"from_b\":2}\n{\"after\":3}\n"
                         "{\"from_b\":4}\n"));
    std::filesystem::remove(path);
}

// The output file is rotated (renamed, then recreated by another sink) while A
// still holds the original. A's failed write must be rolled back in the
// original file; the replacement must not be truncated with its offset.
void test_rotated_file_rollback() {
    std::signal(SIGXFSZ, SIG_IGN);
    const std::string path = temp_path("jalert_sink_rotate.jsonl");
    const std::string rotated = temp_path("jalert_sink_rotate.jsonl.1");
    {
        FileJsonlSink a(path);
        a.write_line("{\"old\":1}");
        std::filesystem::rename(path, rotated);
        FileJsonlSink b(path);
        b.write_line("{\"new\":1234567890}");
        CHECK(b.snapshot().error_message.empty());

        const rlim_t old = set_fsize_limit(std::filesystem::file_size(rotated) + 8);
        a.write_line("{\"failed\":0}");
        set_fsize_limit(old);
        CHECK(!a.snapshot().error_message.empty());
        CHECK_EQ(read_all(rotated), std::string("{\"old\":1}\n"));
        CHECK_EQ(read_all(path), std::string("{\"new\":1234567890}\n"));

        b.write_line("{\"new\":2}");
        a.write_line("{\"after\":3}");   // reopens: now the replacement file
        CHECK(a.snapshot().error_message.empty());
        CHECK(b.snapshot().error_message.empty());
    }
    CHECK_EQ(read_all(rotated), std::string("{\"old\":1}\n"));
    CHECK_EQ(read_all(path),
             std::string("{\"new\":1234567890}\n{\"new\":2}\n{\"after\":3}\n"));
    std::filesystem::remove(path);
    std::filesystem::remove(rotated);
}

// A fragment sits at the end of the file and no rollback removed it (another
// sink's write failed and its own truncate failed too, or it was killed). The
// next writer must cut the fragment off, not append behind it.
void test_fragment_from_other_writer() {
    const std::string path = temp_path("jalert_sink_fragment.jsonl");
    {
        FileJsonlSink sink(path);
        sink.write_line("{\"zero\":0}");
        CHECK(sink.snapshot().error_message.empty());

        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        CHECK(::write(fd, "{\"first\"", 8) == 8);
        ::close(fd);

        // The sink is already open, so recovery cannot rely on a reopen.
        sink.write_line("{\"second\":2}");
        CHECK(sink.snapshot().error_message.empty());
        CHECK_EQ(sink.snapshot().records_written, 2LL);
    }
    CHECK_EQ(read_all(path), std::string("{\"zero\":0}\n{\"second\":2}\n"));
    std::filesystem::remove(path);
}

// Another writer holds the lock and does not let go. write_line runs on the
// DSP thread and snapshot()/status() on the GUI thread, so none of them — nor
// stop() — may wait for that writer.
void test_lock_holder_does_not_block() {
    const std::string path = temp_path("jalert_sink_busy.jsonl");
    write_raw(path, "{\"old\":1}\n");
    FileJsonlSink sink(path);   // opens and releases the lock again

    int fds[2];
    CHECK(pipe(fds) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
        flock(fd, LOCK_EX);
        char c = 'x';
        if (::write(fds[1], &c, 1) != 1) _exit(1);
        usleep(2000000);   // far longer than the sink may wait
        flock(fd, LOCK_UN);
        ::close(fd);
        _exit(0);
    }
    ::close(fds[1]);
    char c = 0;
    CHECK(::read(fds[0], &c, 1) == 1);   // the child holds the lock now
    ::close(fds[0]);

    const auto t0 = std::chrono::steady_clock::now();
    sink.write_line("{\"a\":1}");
    const auto snap = sink.snapshot();
    const std::string status = sink.status();
    sink.stop();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
    CHECK(waited < 1000);
    // The record is reported as not written instead of being written blindly.
    CHECK(!snap.error_message.empty());
    CHECK_EQ(snap.records_written, 0LL);
    CHECK(!status.empty());

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK_EQ(read_all(path), std::string("{\"old\":1}\n"));
    std::filesystem::remove(path);
}

// Two processes writing at the same time, as two SDR++ instances on the default
// output path do: the short lock wait must cover ordinary contention, so every
// record lands and every line stays whole.
void test_concurrent_processes_keep_every_record() {
    const std::string path = temp_path("jalert_sink_concurrent.jsonl");
    constexpr int kEach = 200;
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        FileJsonlSink child(path);
        for (int i = 0; i < kEach; ++i) {
            child.write_line("{\"c\":" + std::to_string(i) + "}");
        }
        const bool ok = child.snapshot().records_written == kEach;
        child.stop();
        _exit(ok ? 0 : 1);
    }
    {
        FileJsonlSink parent(path);
        for (int i = 0; i < kEach; ++i) {
            parent.write_line("{\"p\":" + std::to_string(i) + "}");
        }
        CHECK_EQ(parent.snapshot().records_written, static_cast<long long>(kEach));
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    const std::string all = read_all(path);
    int lines = 0, from_parent = 0, from_child = 0;
    for (std::size_t start = 0; start < all.size();) {
        const std::size_t nl = all.find('\n', start);
        CHECK(nl != std::string::npos);   // no unterminated tail
        if (nl == std::string::npos) break;
        const std::string line = all.substr(start, nl - start);
        start = nl + 1;
        ++lines;
        CHECK(!line.empty() && line.back() == '}');   // no truncated record
        if (line.rfind("{\"p\":", 0) == 0)      ++from_parent;
        else if (line.rfind("{\"c\":", 0) == 0) ++from_child;
    }
    CHECK_EQ(lines, 2 * kEach);
    CHECK_EQ(from_parent, kEach);
    CHECK_EQ(from_child, kEach);
    std::filesystem::remove(path);
}

rlim_t g_restore_limit = RLIM_INFINITY;
extern "C" void restore_limit_on_xfsz(int) {
    // Linux setrlimit is a plain syscall; fine to call from this handler.
    rlimit rl{};
    getrlimit(RLIMIT_FSIZE, &rl);
    rl.rlim_cur = g_restore_limit;
    setrlimit(RLIMIT_FSIZE, &rl);
}

// Write availability returns in the middle of a record: the body is written
// only partly, then further writes succeed again. Nothing may be appended to
// the fragment (in particular not its newline), and the fragment must be gone
// before the next record.
void test_availability_returns_mid_record() {
    const std::string path = temp_path("jalert_sink_midrecord.jsonl");
    {
        FileJsonlSink sink(path);
        sink.write_line("{\"zero\":0}");
        CHECK(sink.snapshot().error_message.empty());

        // 8 bytes are written, the next write raises SIGXFSZ, and the handler
        // lifts the limit again before fwrite() returns short.
        const std::string big = "{\"big\":\"" + std::string(200000, 'x') + "\"}";
        g_restore_limit = set_fsize_limit(std::filesystem::file_size(path) + 8);
        std::signal(SIGXFSZ, restore_limit_on_xfsz);
        sink.write_line(big);
        std::signal(SIGXFSZ, SIG_IGN);
        set_fsize_limit(g_restore_limit);
        CHECK(!sink.snapshot().error_message.empty());
        CHECK_EQ(sink.snapshot().records_written, 1LL);
        // The fragment is dropped right away, not only on recovery.
        CHECK_EQ(read_all(path), std::string("{\"zero\":0}\n"));

        sink.write_line("{\"second\":2}");
        CHECK(sink.snapshot().error_message.empty());
        CHECK_EQ(sink.snapshot().records_written, 2LL);
    }
    CHECK_EQ(read_all(path), std::string("{\"zero\":0}\n{\"second\":2}\n"));
    std::filesystem::remove(path);
}
#endif

} // namespace

int main() {
    test_unterminated_existing_file();
    test_append_existing_file();
    test_no_writes_after_stop();
#ifndef _WIN32
    test_partial_write_recovery();
    test_availability_returns_mid_record();
    test_shared_file_rollback();
    test_rotated_file_rollback();
    test_fragment_from_other_writer();
    test_lock_holder_does_not_block();
    test_concurrent_processes_keep_every_record();
#endif
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all file sink tests passed");
    return 0;
}
