#include "sink/file_sink.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX   // keep the min/max macros away from std::min
#include <windows.h>

#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

namespace jalert::sink {

const char* to_string(SinkPhase p) {
    switch (p) {
        case SinkPhase::Active: return "Active";
        case SinkPhase::Error:  return "Error";
        case SinkPhase::Off:
        default:                return "Off";
    }
}

// Everything below works on the open handle, never on the path: after the
// output file is renamed and recreated, the path names a different file, and
// only the handle still refers to the one being appended to.
namespace {
// How long a record's write waits for another writer's lock. One record is a
// single unbuffered write, so this is orders of magnitude more than a healthy
// writer needs; the point of the bound is that a stuck holder must not stall
// the decoder, the GUI or shutdown.
constexpr int kLockTries = 20;
constexpr int kLockWaitMs = 1;

int fd_of(std::FILE* fp) {
#ifdef _WIN32
    return _fileno(fp);
#else
    return fileno(fp);
#endif
}

void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(static_cast<DWORD>(ms));
#else
    usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

// `busy` tells a lock another writer holds from a real failure.
bool try_lock_file(std::FILE* fp, bool& busy, std::string& err) {
    busy = false;
#ifdef _WIN32
    auto h = reinterpret_cast<HANDLE>(_get_osfhandle(fd_of(fp)));
    OVERLAPPED ov{};
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                   MAXDWORD, MAXDWORD, &ov)) {
        return true;
    }
    const DWORD e = GetLastError();
    if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) { busy = true; }
    else { err = "cannot lock file"; }
    return false;
#else
    if (flock(fd_of(fp), LOCK_EX | LOCK_NB) == 0) return true;
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) busy = true;
    else err = std::strerror(errno);
    return false;
#endif
}

void unlock_file(std::FILE* fp) {
#ifdef _WIN32
    auto h = reinterpret_cast<HANDLE>(_get_osfhandle(fd_of(fp)));
    OVERLAPPED ov{};
    UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
#else
    flock(fd_of(fp), LOCK_UN);
#endif
}

// Exclusive lock on the whole file, held across one record's boundary check,
// write and rollback and never beyond: a sink that cannot finish its rollback
// must not keep other writers (or its own GUI and shutdown) waiting.
//
// flock(2) and LockFileEx() both lock per open handle rather than per process,
// so two sinks in one SDR++ contend just like two SDR++ instances do (POSIX
// fcntl/lockf locks would not, and an unrelated close in the same process
// would drop them).
class FileLock {
public:
    FileLock() = default;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock() { release(); }

    bool acquire(std::FILE* fp, std::string& err) {
        for (int i = 0; i < kLockTries; ++i) {
            bool busy = false;
            if (try_lock_file(fp, busy, err)) { fp_ = fp; return true; }
            if (!busy) return false;
            sleep_ms(kLockWaitMs);
        }
        err = "output file is busy";
        return false;
    }

    void release() {
        if (!fp_) return;
        unlock_file(fp_);
        fp_ = nullptr;
    }

private:
    std::FILE* fp_ = nullptr;
};

bool file_size_of(std::FILE* fp, std::uintmax_t& size) {
#ifdef _WIN32
    struct ::_stat64 st;
    if (_fstat64(fd_of(fp), &st) != 0) return false;
#else
    struct ::stat st;
    if (fstat(fd_of(fp), &st) != 0) return false;
#endif
    size = static_cast<std::uintmax_t>(st.st_size);
    return true;
}

// Which file the handle refers to, so a reopen can tell the file a failed write
// left a fragment in from a replacement that has taken over the path since.
bool identify_file(std::FILE* fp, FileIdentity& id) {
#ifdef _WIN32
    auto h = reinterpret_cast<HANDLE>(_get_osfhandle(fd_of(fp)));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) return false;
    id.volume = info.dwVolumeSerialNumber;
    id.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
               info.nFileIndexLow;
#else
    struct ::stat st;
    if (fstat(fd_of(fp), &st) != 0) return false;
    id.volume = static_cast<std::uint64_t>(st.st_dev);
    id.index = static_cast<std::uint64_t>(st.st_ino);
#endif
    id.valid = true;
    return true;
}

// Reads n bytes at `off` without disturbing where the next append lands (the
// handle is O_APPEND, so writes always go to the end).
bool read_at(std::FILE* fp, char* buf, std::size_t n, std::uintmax_t off) {
#ifdef _WIN32
    const int fd = fd_of(fp);
    if (_lseeki64(fd, static_cast<__int64>(off), SEEK_SET) < 0) return false;
    return _read(fd, buf, static_cast<unsigned>(n)) == static_cast<int>(n);
#else
    return pread(fd_of(fp), buf, n, static_cast<off_t>(off)) ==
           static_cast<ssize_t>(n);
#endif
}

// Size of the open file and the offset just past its last '\n' (0 if none),
// i.e. the end of the last complete record.
bool scan_tail(std::FILE* fp, std::uintmax_t& size, std::uintmax_t& record_end,
               std::string& err) {
    record_end = 0;
    if (!file_size_of(fp, size)) { err = std::strerror(errno); return false; }

    char buf[4096];
    std::uintmax_t pos = size;
    while (pos > 0) {
        const std::uintmax_t n = std::min<std::uintmax_t>(pos, sizeof(buf));
        pos -= n;
        if (!read_at(fp, buf, static_cast<std::size_t>(n), pos)) {
            err = "cannot read file";
            return false;
        }
        for (std::uintmax_t i = n; i > 0; --i) {
            if (buf[i - 1] == '\n') { record_end = pos + i; return true; }
        }
    }
    return true;
}

bool truncate_open_file(std::FILE* fp, std::uintmax_t keep, std::string& err) {
#ifdef _WIN32
    const int e = _chsize_s(fd_of(fp), static_cast<__int64>(keep));
    if (e != 0) { err = std::strerror(e); return false; }
#else
    if (ftruncate(fd_of(fp), static_cast<off_t>(keep)) != 0) {
        err = std::strerror(errno);
        return false;
    }
#endif
    return true;
}
} // namespace

FileJsonlSink::FileJsonlSink(const std::string& path) : path_(path) {
    try {
        std::filesystem::path p(path);
        if (p.has_parent_path() && !p.parent_path().empty()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    } catch (...) {
        // best-effort directory create — fall through to fopen
    }

    std::lock_guard<std::mutex> lk(gate_);
    open_locked();
}

// Opens the file positioned at a complete-record boundary. On failure the sink
// stays in the error state and the next write_line retries.
bool FileJsonlSink::open_locked() {
    // Read access ("a+b", not "ab") so the tail check and the rollback can go
    // through this handle; writes still always append.
    fp_ = std::fopen(path_.c_str(), "a+b");
    if (!fp_) { fail_locked(std::strerror(errno)); return false; }
    // Unbuffered: whatever fwrite() could not write is never flushed later
    // (e.g. by fclose), so after a rollback nothing more lands in the file.
    std::setvbuf(fp_, nullptr, _IONBF, 0);

    std::string err;
    FileLock lock;
    if (!lock.acquire(fp_, err)) { fail_locked(err); close_locked(); return false; }

    // An unterminated tail is this sink's own fragment only if a write of ours
    // failed, its rollback failed too, and this is still the file that write
    // went to — not a replacement that has taken over the path since.
    if (fragment_pending_) {
        FileIdentity now;
        if (!fragment_file_.valid || !identify_file(fp_, now)) {
            // Without both identities there is no telling the fragment from
            // content to keep, and either guess risks a malformed record or
            // lost data: stay in the error state; the next write tries again.
            fail_locked("cannot identify the file holding an incomplete record");
            lock.release();
            close_locked();
            return false;
        }
        if (now.is(fragment_file_)) {
            // Ours: cut it off. Nothing can have been appended behind it, since
            // every writer trims the tail before it appends.
            if (!repair_tail_locked(err)) {
                fail_locked("cannot trim incomplete record: " + err);
                lock.release();
                close_locked();
                return false;
            }
            fragment_pending_ = false;
            ok_locked();
            return true;
        }
        // A fragment in a file this path no longer names is out of reach; the
        // writer still holding that file removes it.
        fragment_pending_ = false;
    }

    std::uintmax_t size = 0, record_end = 0;
    if (!scan_tail(fp_, size, record_end, err)) {
        fail_locked(err);
        lock.release();
        close_locked();
        return false;
    }
    // Content already there and not ending in '\n' predates this sink (a file
    // that is not ours, or one left by a killed process): keep it and start a
    // new line rather than extending its last one. A fragment of our own never
    // lands here — it is cut off above — and from here on an unterminated tail
    // can only be a fragment of a failed write, which write_line cuts off.
    if (record_end < size && std::fputc('\n', fp_) == EOF) {
        err = std::strerror(errno);
        lock.release();
        fail_locked(err);
        close_locked();
        return false;
    }
    ok_locked();
    return true;
}

// Re-establishes the append boundary under the lock: cuts off a record fragment
// that a failed write left at the end of the file. Correctness rests on this
// check, not on the rollback in write_line having succeeded — the fragment may
// be another sink's, or ours from a write whose rollback failed, and whoever
// writes next removes it. A fragment is never a valid record, so cutting it
// loses nothing (content that predates the sink was terminated at open).
bool FileJsonlSink::repair_tail_locked(std::string& err) {
    std::uintmax_t size = 0, record_end = 0;
    if (!scan_tail(fp_, size, record_end, err)) return false;
    if (record_end == size) return true;   // already on a record boundary
    return truncate_open_file(fp_, record_end, err);
}

void FileJsonlSink::close_locked() {
    if (fp_) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

void FileJsonlSink::ok_locked() {
    error_.clear();
    status_ = std::string("writing → ") +
              std::filesystem::path(path_).filename().string();
}

void FileJsonlSink::fail_locked(const std::string& msg) {
    error_ = msg;
    status_ = "error: " + error_;
}

FileJsonlSink::~FileJsonlSink() { stop(); }

std::string FileJsonlSink::status() const {
    std::lock_guard<std::mutex> lk(gate_);
    return status_;
}

void FileJsonlSink::write_line(const std::string& line) {
    if (line.empty()) return;
    std::lock_guard<std::mutex> lk(gate_);
    if (stopped_) return;
    // Retry after an earlier open/write failure (e.g. a drive that came back).
    if (!fp_ && !open_locked()) return;

    std::string err;
    FileLock lock;
    if (!lock.acquire(fp_, err)) { fail_locked(err); return; }
    // An incomplete tail must go before anything is appended after it. If it
    // cannot, this record is rejected rather than written behind a fragment,
    // and the next one tries again.
    if (!repair_tail_locked(err)) {
        fail_locked("cannot trim incomplete record: " + err);
        return;
    }

    // Rollback point: the end of the file as it is now, which includes records
    // other sinks appended while they held the lock.
    std::uintmax_t before = 0;
    if (!file_size_of(fp_, before)) {
        err = std::strerror(errno);
        lock.release();
        fail_locked(err);
        close_locked();
        return;
    }

    // Record and newline in one write: after a short write nothing more is
    // appended, so a fragment never gets a line break of its own.
    const std::string record = line + '\n';
    const std::size_t n = std::fwrite(record.data(), 1, record.size(), fp_);
    if (n != record.size()) {
        err = std::strerror(errno);
        // Drop whatever reached the file. If this fails the fragment stays and
        // the next writer removes it, so nothing hangs on it succeeding — but
        // note which file it is in, so our own reopen below cuts it off instead
        // of taking it for content that predates the sink.
        std::string ignored;
        if (!truncate_open_file(fp_, before, ignored)) {
            fragment_file_ = FileIdentity{};
            identify_file(fp_, fragment_file_);
            fragment_pending_ = true;
        }
        lock.release();
        fail_locked(err);
        close_locked();   // the next write reopens, picking up a rotation
        return;
    }
    lock.release();
    bytes_written_ += static_cast<long long>(record.size());
    ++records_written_;
    ok_locked();
}

void FileJsonlSink::stop() {
    std::lock_guard<std::mutex> lk(gate_);
    stopped_ = true;
    close_locked();
    status_ = "off";
}

FileSinkSnapshot FileJsonlSink::snapshot() const {
    std::lock_guard<std::mutex> lk(gate_);
    FileSinkSnapshot s;
    s.path = path_;
    s.bytes_written = bytes_written_;
    s.records_written = records_written_;
    s.error_message = error_;
    if (!error_.empty())   s.phase = SinkPhase::Error;
    else if (fp_)          s.phase = SinkPhase::Active;
    else                   s.phase = SinkPhase::Off;
    return s;
}

} // namespace jalert::sink
