#include "ridgeline/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include "ridgeline/wal_record_codec.h"

namespace ridgeline {

namespace {

// Thin RAII wrapper around a POSIX fd, since we need fsync() specifically
// (fflush() on a FILE* only flushes to the OS page cache buffer, not the
// device -- fsync is the actual durability boundary described in wal.h).
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() { if (fd_ >= 0) ::close(fd_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

 private:
  int fd_;
};

void ThrowErrno(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

std::string ReadWholeFile(const std::string& path) {
  Fd fd(::open(path.c_str(), O_RDONLY));
  if (!fd.valid()) {
    if (errno == ENOENT) return {};
    ThrowErrno("open(" + path + ") for read");
  }
  std::string data;
  char buf[65536];
  ssize_t n;
  while ((n = ::read(fd.get(), buf, sizeof(buf))) > 0) {
    data.append(buf, static_cast<std::size_t>(n));
  }
  if (n < 0) ThrowErrno("read(" + path + ")");
  return data;
}

// write-temp + fsync + rename: the standard pattern for "this file either
// has the old complete contents or the new complete contents, never a torn
// mix," because rename() on the same filesystem is atomic at the directory
// level -- readers never observe a half-renamed state.
void AtomicWriteFile(const std::string& path, const std::string& contents) {
  const std::string tmp = path + ".tmp";
  {
    Fd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (!fd.valid()) ThrowErrno("open(" + tmp + ") for write");
    std::size_t written = 0;
    while (written < contents.size()) {
      const ssize_t n = ::write(fd.get(), contents.data() + written, contents.size() - written);
      if (n < 0) ThrowErrno("write(" + tmp + ")");
      written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd.get()) != 0) ThrowErrno("fsync(" + tmp + ")");
  }  // fd closed here
  if (::rename(tmp.c_str(), path.c_str()) != 0) ThrowErrno("rename(" + tmp + ", " + path + ")");
}

}  // namespace

Wal::Wal(std::string wal_path, std::string checkpoint_path)
    : wal_path_(std::move(wal_path)), checkpoint_path_(std::move(checkpoint_path)) {
  // Create the WAL file if it doesn't exist yet, then immediately close --
  // Append() reopens per-call in append mode. (Reopening per-append is not
  // the fastest possible design; it keeps the class simple and the fd
  // lifetime obviously tied to each operation. A production version under
  // real load would keep one fd open across the process lifetime -- noted
  // here rather than silently optimized away, since it's a real, deliberate
  // simplicity-vs-throughput tradeoff.)
  Fd fd(::open(wal_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644));
  if (!fd.valid()) ThrowErrno("open(" + wal_path_ + ") for create");
  LoadCheckpoint();
}

Wal::~Wal() = default;

void Wal::LoadCheckpoint() {
  const std::string data = ReadWholeFile(checkpoint_path_);
  last_acked_seq_ = 0;
  if (data.size() == 8) {
    std::memcpy(&last_acked_seq_, data.data(), 8);
  }
  // Any other size (0 = no checkpoint yet, or something malformed) is
  // treated as "nothing acked yet" -- the safe default, since it causes
  // MORE replay (at-least-once redelivery of already-acked events), not
  // silent data loss. The gateway's cumulative-ack dedup (ADR-0001) already
  // handles redelivered duplicates.
}

void Wal::Append(std::uint64_t seq, const std::string& payload) {
  const std::string record = EncodeRecord(seq, payload);
  Fd fd(::open(wal_path_.c_str(), O_WRONLY | O_APPEND));
  if (!fd.valid()) ThrowErrno("open(" + wal_path_ + ") for append");
  std::size_t written = 0;
  while (written < record.size()) {
    const ssize_t n = ::write(fd.get(), record.data() + written, record.size() - written);
    if (n < 0) ThrowErrno("write(" + wal_path_ + ")");
    written += static_cast<std::size_t>(n);
  }
  if (::fsync(fd.get()) != 0) ThrowErrno("fsync(" + wal_path_ + ")");
}

void Wal::Acknowledge(std::uint64_t up_to_seq) {
  if (up_to_seq <= last_acked_seq_) return;  // Stale/duplicate ack; nothing to do.
  std::string data(8, '\0');
  std::memcpy(data.data(), &up_to_seq, 8);
  AtomicWriteFile(checkpoint_path_, data);
  last_acked_seq_ = up_to_seq;
}

std::size_t Wal::ReplayUnacked(const std::function<void(std::uint64_t, const std::string&)>& on_record) {
  const std::string data = ReadWholeFile(wal_path_);
  std::size_t offset = 0;
  std::size_t replayed = 0;
  std::uint64_t seq = 0;
  std::string payload;
  while (DecodeRecord(data, offset, seq, payload)) {
    if (seq > last_acked_seq_) {  // Skip records the gateway already durably has.
      on_record(seq, payload);
      ++replayed;
    }
  }
  return replayed;
}

void Wal::Compact() {
  std::string kept;
  const std::string data = ReadWholeFile(wal_path_);
  std::size_t offset = 0;
  std::uint64_t seq = 0;
  std::string payload;
  while (DecodeRecord(data, offset, seq, payload)) {
    if (seq > last_acked_seq_) kept += EncodeRecord(seq, payload);
  }
  AtomicWriteFile(wal_path_, kept);
}

std::uint64_t Wal::FileSizeBytes() const { return ReadWholeFile(wal_path_).size(); }

}  // namespace ridgeline
