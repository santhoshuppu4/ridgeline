#include "ridgeline/wal.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "ridgeline/wal_record_codec.h"

namespace {

namespace fs = std::filesystem;

class WalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / fs::path("ridgeline_wal_test_" + std::to_string(::getpid()) + "_" +
                                                std::to_string(test_counter_++));
    fs::create_directories(dir_);
    wal_path_ = (dir_ / "events.wal").string();
    ckpt_path_ = (dir_ / "events.ckpt").string();
  }
  void TearDown() override { std::error_code ec; fs::remove_all(dir_, ec); }

  fs::path dir_;
  std::string wal_path_;
  std::string ckpt_path_;
  static inline int test_counter_ = 0;
};

}  // namespace

TEST_F(WalTest, FreshWalHasNothingToReplay) {
  ridgeline::Wal wal(wal_path_, ckpt_path_);
  EXPECT_EQ(wal.LastAcked(), 0u);
  int calls = 0;
  EXPECT_EQ(wal.ReplayUnacked([&](std::uint64_t, const std::string&) { ++calls; }), 0u);
  EXPECT_EQ(calls, 0);
}

TEST_F(WalTest, AppendThenReplayReturnsRecordsInOrder) {
  ridgeline::Wal wal(wal_path_, ckpt_path_);
  wal.Append(1, "alpha");
  wal.Append(2, "beta");
  wal.Append(3, "gamma");

  std::vector<std::pair<std::uint64_t, std::string>> got;
  const std::size_t n = wal.ReplayUnacked([&](std::uint64_t seq, const std::string& payload) { got.emplace_back(seq, payload); });

  ASSERT_EQ(n, 3u);
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0], (std::pair<std::uint64_t, std::string>{1, "alpha"}));
  EXPECT_EQ(got[1], (std::pair<std::uint64_t, std::string>{2, "beta"}));
  EXPECT_EQ(got[2], (std::pair<std::uint64_t, std::string>{3, "gamma"}));
}

TEST_F(WalTest, AcknowledgeIsPersistedAcrossInstances) {
  {
    ridgeline::Wal wal(wal_path_, ckpt_path_);
    wal.Append(1, "a");
    wal.Append(2, "b");
    wal.Append(3, "c");
    wal.Acknowledge(2);
  }
  // A fresh Wal instance over the same files -- simulates a process restart.
  ridgeline::Wal wal2(wal_path_, ckpt_path_);
  EXPECT_EQ(wal2.LastAcked(), 2u);
  std::vector<std::uint64_t> replayed_seqs;
  wal2.ReplayUnacked([&](std::uint64_t seq, const std::string&) { replayed_seqs.push_back(seq); });
  EXPECT_EQ(replayed_seqs, (std::vector<std::uint64_t>{3}))
      << "only the unacked record (seq=3) should replay after restart";
}

TEST_F(WalTest, AcknowledgeIgnoresStaleOrDuplicateValues) {
  ridgeline::Wal wal(wal_path_, ckpt_path_);
  wal.Acknowledge(5);
  EXPECT_EQ(wal.LastAcked(), 5u);
  wal.Acknowledge(3);  // Stale (lower than current) -- must not move backwards.
  EXPECT_EQ(wal.LastAcked(), 5u);
  wal.Acknowledge(5);  // Duplicate of current -- no-op, not an error.
  EXPECT_EQ(wal.LastAcked(), 5u);
}

TEST_F(WalTest, CorruptedLastRecordStopsReplayCleanlyWithoutLosingEarlierRecords) {
  // Simulates a crash mid-write to the LAST record: flip a byte inside it.
  // Earlier, fully-written records must still replay correctly.
  {
    ridgeline::Wal wal(wal_path_, ckpt_path_);
    wal.Append(1, "good-record-one");
    wal.Append(2, "good-record-two");
    wal.Append(3, "this-one-gets-corrupted");
  }
  {
    std::string data;
    { std::ifstream in(wal_path_, std::ios::binary); data.assign((std::istreambuf_iterator<char>(in)), {}); }
    ASSERT_FALSE(data.empty());
    data[data.size() - 1] ^= static_cast<char>(0xFF);  // Flip a bit inside the last record's payload -- fails its CRC.
    std::ofstream out(wal_path_, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  ridgeline::Wal wal(wal_path_, ckpt_path_);
  std::vector<std::uint64_t> replayed;
  const std::size_t n = wal.ReplayUnacked([&](std::uint64_t seq, const std::string&) { replayed.push_back(seq); });
  EXPECT_EQ(n, 2u);
  EXPECT_EQ(replayed, (std::vector<std::uint64_t>{1, 2}))
      << "the two good records before the corrupted one must still replay";
}

TEST_F(WalTest, TruncatedTailFromSimulatedCrashMidWriteReplaysCleanly) {
  // Simulates a crash mid-append: the file is truncated partway through the
  // LAST record's header or payload, exactly what kill -9 during Wal::Append
  // would leave behind (fsync happens AFTER the full write, so a kill before
  // that point can leave a partial write on some filesystems/buffering
  // configurations -- this test exercises the reader's tolerance for that,
  // independent of exactly when the truncation happens).
  {
    ridgeline::Wal wal(wal_path_, ckpt_path_);
    wal.Append(1, "complete-record");
    wal.Append(2, "another-complete-one");
  }
  const std::string full = [&] {
    std::ifstream in(wal_path_, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }();
  ASSERT_GT(full.size(), 10u);

  // Truncate to partway through what would be a third record's header.
  const std::string truncated = full + std::string(6, '\x01');  // 6 bytes: less than a 16-byte header.
  { std::ofstream out(wal_path_, std::ios::binary | std::ios::trunc); out.write(truncated.data(), static_cast<std::streamsize>(truncated.size())); }

  ridgeline::Wal wal(wal_path_, ckpt_path_);
  std::vector<std::uint64_t> replayed;
  EXPECT_EQ(wal.ReplayUnacked([&](std::uint64_t seq, const std::string&) { replayed.push_back(seq); }), 2u);
  EXPECT_EQ(replayed, (std::vector<std::uint64_t>{1, 2}));
}

TEST_F(WalTest, CompactRemovesAckedRecordsAndShrinksFile) {
  ridgeline::Wal wal(wal_path_, ckpt_path_);
  for (std::uint64_t i = 1; i <= 100; ++i) {
    wal.Append(i, std::string(200, 'x'));  // Padding so the size difference is unambiguous.
  }
  wal.Acknowledge(90);
  const auto before = wal.FileSizeBytes();
  wal.Compact();
  const auto after = wal.FileSizeBytes();
  EXPECT_LT(after, before) << "compacting away 90 of 100 records should shrink the file substantially";

  std::vector<std::uint64_t> replayed;
  EXPECT_EQ(wal.ReplayUnacked([&](std::uint64_t seq, const std::string&) { replayed.push_back(seq); }), 10u);
  for (std::uint64_t i = 0; i < 10; ++i) EXPECT_EQ(replayed[i], 91 + i);
}

TEST_F(WalTest, CompactThenRestartStillReplaysCorrectly) {
  {
    ridgeline::Wal wal(wal_path_, ckpt_path_);
    for (std::uint64_t i = 1; i <= 5; ++i) wal.Append(i, "payload-" + std::to_string(i));
    wal.Acknowledge(3);
    wal.Compact();
  }
  ridgeline::Wal wal2(wal_path_, ckpt_path_);
  EXPECT_EQ(wal2.LastAcked(), 3u);
  std::vector<std::uint64_t> replayed;
  wal2.ReplayUnacked([&](std::uint64_t seq, const std::string&) { replayed.push_back(seq); });
  EXPECT_EQ(replayed, (std::vector<std::uint64_t>{4, 5}));
}

// ---------------------------------------------------------------------------
// Record codec tests (DecodeRecord directly) -- the same function the fuzz
// harness in tools/fuzz/wal_record_fuzzer.cc calls on raw bytes.
// ---------------------------------------------------------------------------

TEST(WalRecordCodec, RoundTripsThroughEncodeDecode) {
  const std::string encoded = ridgeline::EncodeRecord(42, "hello world");
  std::size_t offset = 0;
  std::uint64_t seq = 0;
  std::string payload;
  ASSERT_TRUE(ridgeline::DecodeRecord(encoded, offset, seq, payload));
  EXPECT_EQ(seq, 42u);
  EXPECT_EQ(payload, "hello world");
  EXPECT_EQ(offset, encoded.size());
}

TEST(WalRecordCodec, EmptyPayloadRoundTrips) {
  const std::string encoded = ridgeline::EncodeRecord(1, "");
  std::size_t offset = 0;
  std::uint64_t seq = 0;
  std::string payload;
  ASSERT_TRUE(ridgeline::DecodeRecord(encoded, offset, seq, payload));
  EXPECT_TRUE(payload.empty());
}

TEST(WalRecordCodec, RejectsBufferShorterThanHeader) {
  std::string buf(10, '\0');  // Header is 16 bytes.
  std::size_t offset = 0;
  std::uint64_t seq = 0;
  std::string payload;
  EXPECT_FALSE(ridgeline::DecodeRecord(buf, offset, seq, payload));
  EXPECT_EQ(offset, 0u) << "a failed decode must not advance offset";
}

TEST(WalRecordCodec, RejectsImplausiblyLargeDeclaredLength) {
  std::string buf(20, '\0');
  const std::uint32_t huge_len = 0xFFFFFFFFu;
  std::memcpy(buf.data() + 8, &huge_len, 4);
  std::size_t offset = 0;
  std::uint64_t seq = 0;
  std::string payload;
  EXPECT_FALSE(ridgeline::DecodeRecord(buf, offset, seq, payload));
}

TEST(WalRecordCodec, RejectsPayloadShorterThanDeclaredLength) {
  std::string buf(20, '\0');  // Header (16) + 4 bytes payload, but claim len=100.
  const std::uint32_t len = 100;
  std::memcpy(buf.data() + 8, &len, 4);
  std::size_t offset = 0;
  std::uint64_t seq = 0;
  std::string payload;
  EXPECT_FALSE(ridgeline::DecodeRecord(buf, offset, seq, payload));
}

TEST(WalRecordCodec, RejectsWrongCrc) {
  std::string encoded = ridgeline::EncodeRecord(1, "data");
  encoded[encoded.size() - 1] ^= 0x01;  // Flip a payload bit; header (and thus stored CRC) unchanged.
  std::size_t offset = 0;
  std::uint64_t seq = 0;
  std::string payload;
  EXPECT_FALSE(ridgeline::DecodeRecord(encoded, offset, seq, payload));
}
