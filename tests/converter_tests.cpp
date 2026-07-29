#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "databento/constants.hpp"
#include "databento/datetime.hpp"
#include "databento/dbn.hpp"
#include "databento/dbn_encoder.hpp"
#include "databento/dbn_store.hpp"
#include "databento/detail/zstd_stream.hpp"
#include "databento/enums.hpp"
#include "databento/file_stream.hpp"
#include "databento/flag_set.hpp"
#include "databento/record.hpp"
#include "mbo_mbp10/converter.hpp"
#include "mbo_mbp10/file_converter.hpp"

namespace {

class TestFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw TestFailure{std::string{"check failed: "} + #condition + " at " +  \
                        __FILE__ + ":" + std::to_string(__LINE__)};            \
    }                                                                          \
  } while (false)

databento::MboMsg Message(
    databento::Action action, databento::Side side, std::uint64_t order_id,
    std::int64_t price, std::uint32_t size,
    std::uint8_t flags = databento::FlagSet::kLast,
    std::uint32_t instrument_id = 1, std::uint32_t sequence = 1) {
  return databento::MboMsg{
      databento::RecordHeader{
          static_cast<std::uint8_t>(
              sizeof(databento::MboMsg) /
              databento::RecordHeader::kLengthMultiplier),
          databento::RType::Mbo,
          1,
          instrument_id,
          databento::UnixNanos{std::chrono::nanoseconds{sequence}}},
      order_id,
      price,
      size,
      databento::FlagSet{flags},
      0,
      action,
      side,
      databento::UnixNanos{std::chrono::nanoseconds{sequence + 10}},
      databento::TimeDeltaNanos{1},
      sequence};
}

databento::MboMsg Clear(std::uint8_t flags = databento::FlagSet::kLast,
                        std::uint32_t instrument_id = 1,
                        std::uint32_t sequence = 1) {
  return Message(databento::Action::Clear, databento::Side::None, 0,
                 databento::kUndefPrice, 0, flags, instrument_id, sequence);
}

databento::MboMsg None(std::uint8_t flags = databento::FlagSet::kLast,
                       std::uint32_t instrument_id = 1,
                       std::uint32_t sequence = 1) {
  return Message(databento::Action::None, databento::Side::None, 0,
                 databento::kUndefPrice, 0, flags, instrument_id, sequence);
}

template <typename Function>
void ExpectConversionError(Function&& function, const std::string& needle) {
  try {
    function();
  } catch (const mbo_mbp10::ConversionError& error) {
    CHECK(std::string{error.what()}.find(needle) != std::string::npos);
    return;
  }
  throw TestFailure{"expected ConversionError containing: " + needle};
}

void TestLegacyInlineLast() {
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};

  converter.Process(Clear());
  converter.Process(Message(databento::Action::Add, databento::Side::Bid, 10,
                            100, 5));
  converter.Process(Message(databento::Action::Add, databento::Side::Ask, 11,
                            110, 7));
  const auto stats = converter.Finish();

  CHECK(stats.input_records == 3);
  CHECK(stats.output_records == 3);
  CHECK(stats.completed_events == 3);
  CHECK(output.size() == 3);
  CHECK(output[0].action == databento::Action::Clear);
  CHECK(output[0].flags.IsLast());
  CHECK(output[0].levels[0].bid_px == databento::kUndefPrice);
  CHECK(output[0].levels[0].ask_px == databento::kUndefPrice);
  CHECK(output[1].levels[0].bid_px == 100);
  CHECK(output[1].levels[0].bid_sz == 5);
  CHECK(output[1].levels[0].bid_ct == 1);
  CHECK(output[2].levels[0].ask_px == 110);
  CHECK(output[2].levels[0].ask_sz == 7);
  CHECK(output[2].levels[0].ask_ct == 1);
  CHECK(output[2].hd.rtype == databento::RType::Mbp10);
  CHECK(output[2].hd.length ==
        sizeof(databento::Mbp10Msg) /
            databento::RecordHeader::kLengthMultiplier);
}

void TestStandaloneLastMarker() {
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};

  converter.Process(Clear());
  converter.Process(Message(databento::Action::Add, databento::Side::Bid, 10,
                            100, 5, 0, 1, 2));
  converter.Process(Message(databento::Action::Add, databento::Side::Ask, 11,
                            110, 7, 0, 1, 3));
  CHECK(output.size() == 1);
  converter.Process(None(databento::FlagSet::kLast, 1, 4));
  const auto stats = converter.Finish();

  CHECK(output.size() == 2);
  CHECK(output[1].flags.IsLast());
  CHECK(output[1].action == databento::Action::Add);
  CHECK(output[1].side == databento::Side::Ask);
  CHECK(output[1].levels[0].bid_px == 100);
  CHECK(output[1].levels[0].ask_px == 110);
  CHECK(stats.none_markers == 1);
  CHECK(stats.completed_events == 2);
  CHECK(stats.coalesced_book_updates == 1);
}

void TestLegacyMultiUpdateCoalescing() {
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};

  converter.Process(Clear());
  converter.Process(Message(databento::Action::Add, databento::Side::Bid, 10,
                            100, 5, 0, 1, 2));
  converter.Process(Message(databento::Action::Add, databento::Side::Ask, 11,
                            110, 7, databento::FlagSet::kLast, 1, 3));
  const auto stats = converter.Finish();

  CHECK(output.size() == 2);
  CHECK(output[1].action == databento::Action::Add);
  CHECK(output[1].side == databento::Side::Ask);
  CHECK(output[1].levels[0].bid_px == 100);
  CHECK(output[1].levels[0].ask_px == 110);
  CHECK(output[1].flags.IsLast());
  CHECK(stats.coalesced_book_updates == 1);
}

void TestTopTenSuppressionAndCancel() {
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};

  converter.Process(Clear());
  for (std::uint32_t index = 0; index < 11; ++index) {
    converter.Process(Message(databento::Action::Add, databento::Side::Ask,
                              1000 + index, 100 + index, 1,
                              databento::FlagSet::kLast, 1, index + 2));
  }
  CHECK(output.size() == 11);

  converter.Process(Message(databento::Action::Cancel, databento::Side::Ask,
                            1010, 110, 1, databento::FlagSet::kLast, 1, 20));
  CHECK(output.size() == 11);

  converter.Process(Message(databento::Action::Cancel, databento::Side::Ask,
                            1000, 100, 1, databento::FlagSet::kLast, 1, 21));
  const auto stats = converter.Finish();
  CHECK(output.size() == 12);
  CHECK(output.back().depth == 0);
  CHECK(output.back().levels[0].ask_px == 101);
  CHECK(output.back().levels[8].ask_px == 109);
  CHECK(output.back().levels[9].ask_px == databento::kUndefPrice);
  CHECK(stats.invisible_book_updates == 2);
}

void TestAggregateModifyAndCancel() {
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};

  converter.Process(Clear());
  converter.Process(Message(databento::Action::Add, databento::Side::Bid, 1,
                            100, 2, databento::FlagSet::kLast, 1, 2));
  converter.Process(Message(databento::Action::Add, databento::Side::Bid, 2,
                            100, 3, databento::FlagSet::kLast, 1, 3));
  CHECK(output.back().levels[0].bid_sz == 5);
  CHECK(output.back().levels[0].bid_ct == 2);

  converter.Process(Message(databento::Action::Modify, databento::Side::Bid, 1,
                            100, 4, databento::FlagSet::kLast, 1, 4));
  CHECK(output.back().levels[0].bid_sz == 7);
  CHECK(output.back().levels[0].bid_ct == 2);

  converter.Process(Message(databento::Action::Cancel, databento::Side::Bid, 2,
                            100, 1, databento::FlagSet::kLast, 1, 5));
  CHECK(output.back().levels[0].bid_sz == 6);
  CHECK(output.back().levels[0].bid_ct == 2);

  converter.Process(Message(databento::Action::Cancel, databento::Side::Bid, 2,
                            100, 2, databento::FlagSet::kLast, 1, 6));
  CHECK(output.back().levels[0].bid_sz == 4);
  CHECK(output.back().levels[0].bid_ct == 1);

  converter.Process(Message(databento::Action::Modify, databento::Side::Bid, 1,
                            105, 6, databento::FlagSet::kLast, 1, 7));
  converter.Finish();
  CHECK(output.back().levels[0].bid_px == 105);
  CHECK(output.back().levels[0].bid_sz == 6);
  CHECK(output.back().levels[0].bid_ct == 1);
}

void TestTradeFillAndFinalCancel() {
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};

  converter.Process(Clear());
  converter.Process(Message(databento::Action::Add, databento::Side::Bid, 1,
                            100, 5, databento::FlagSet::kLast, 1, 2));
  converter.Process(Message(databento::Action::Trade, databento::Side::Ask, 0,
                            100, 5, 0, 1, 3));
  converter.Process(Message(databento::Action::Fill, databento::Side::Bid, 1,
                            100, 5, 0, 1, 4));
  CHECK(output.size() == 2);
  converter.Process(Message(databento::Action::Cancel, databento::Side::Bid, 1,
                            100, 5, databento::FlagSet::kLast, 1, 5));
  const auto stats = converter.Finish();

  CHECK(output.size() == 4);
  CHECK(output[2].action == databento::Action::Trade);
  CHECK(output[2].levels[0].bid_px == 100);
  CHECK(!output[2].flags.IsLast());
  CHECK(output[3].action == databento::Action::Cancel);
  CHECK(output[3].levels[0].bid_px == databento::kUndefPrice);
  CHECK(output[3].flags.IsLast());
  CHECK(stats.trades == 1);
  CHECK(stats.fills == 1);
}

void TestInterleavedOutputOrder() {
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};

  converter.Process(Clear(databento::FlagSet::kLast, 1, 1));
  converter.Process(Clear(databento::FlagSet::kLast, 2, 2));
  converter.Process(Message(databento::Action::Add, databento::Side::Bid, 10,
                            100, 1, 0, 1, 3));
  converter.Process(Message(databento::Action::Add, databento::Side::Ask, 20,
                            110, 1, databento::FlagSet::kLast, 2, 4));
  // Quote updates are coalesced at each instrument's event boundary, so
  // instrument 2 completes and emits before instrument 1.
  CHECK(output.size() == 3);

  converter.Process(None(databento::FlagSet::kLast, 1, 5));
  converter.Finish();
  CHECK(output.size() == 4);
  CHECK(output[2].hd.instrument_id == 2);
  CHECK(output[3].hd.instrument_id == 1);
  CHECK(output[2].flags.IsLast());
  CHECK(output[3].flags.IsLast());
}

void TestStrictValidation() {
  {
    mbo_mbp10::Converter converter{[](const databento::Mbp10Msg&) {}};
    ExpectConversionError(
        [&] {
          converter.Process(Message(databento::Action::Add,
                                    databento::Side::Bid, 1, 100, 1));
        },
        "first book record");
  }
  {
    mbo_mbp10::Converter converter{[](const databento::Mbp10Msg&) {}};
    converter.Process(Clear());
    ExpectConversionError(
        [&] {
          converter.Process(Message(
              databento::Action::Add, databento::Side::Bid, 1, 100, 1,
              static_cast<std::uint8_t>(databento::FlagSet::kLast |
                                        databento::FlagSet::kMaybeBadBook)));
        },
        "MAYBE_BAD_BOOK");
  }
  {
    mbo_mbp10::Converter converter{[](const databento::Mbp10Msg&) {}};
    converter.Process(Clear(0));
    ExpectConversionError([&] { converter.Finish(); }, "before F_LAST");
  }
}

void TestModifyAsAdd() {
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};
  converter.Process(Clear());
  converter.Process(Message(databento::Action::Modify, databento::Side::Ask,
                            99, 120, 4));
  const auto stats = converter.Finish();
  CHECK(stats.modifies_as_add == 1);
  CHECK(output.back().action == databento::Action::Modify);
  CHECK(output.back().levels[0].ask_px == 120);
  CHECK(output.back().levels[0].ask_sz == 4);
}

void TestHistoricalSnapshotCollapse() {
  constexpr auto kSnapshotFlags = static_cast<std::uint8_t>(
      databento::FlagSet::kSnapshot | databento::FlagSet::kBadTsRecv);
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};

  converter.Process(Clear(kSnapshotFlags, 1, 1));
  for (std::uint32_t index = 0; index < 12; ++index) {
    converter.Process(Message(databento::Action::Add, databento::Side::Ask,
                              1000 + index, 100 + index, index + 1,
                              kSnapshotFlags, 1, index + 2));
  }
  converter.Process(Message(databento::Action::Add, databento::Side::Bid,
                            2000, 99, 7, kSnapshotFlags, 1, 20));
  CHECK(output.empty());

  // The post-cutover form may use a standalone N/F_LAST record without
  // repeating F_SNAPSHOT on the marker.
  converter.Process(None(databento::FlagSet::kLast, 1, 21));
  const auto stats = converter.Finish();

  CHECK(output.size() == 1);
  CHECK(output[0].action == databento::Action::Clear);
  CHECK(output[0].side == databento::Side::None);
  CHECK(output[0].price == databento::kUndefPrice);
  CHECK(output[0].flags.IsSnapshot());
  CHECK(output[0].flags.IsBadTsRecv());
  CHECK(output[0].flags.IsLast());
  CHECK(output[0].levels[0].bid_px == 99);
  CHECK(output[0].levels[0].bid_sz == 7);
  CHECK(output[0].levels[0].ask_px == 100);
  CHECK(output[0].levels[9].ask_px == 109);
  CHECK(stats.completed_snapshots == 1);
  CHECK(stats.snapshot_outputs == 1);
  CHECK(stats.invisible_book_updates == 0);
}

void TestEmptyHistoricalSnapshotIsSuppressed() {
  constexpr auto kFlags = static_cast<std::uint8_t>(
      databento::FlagSet::kSnapshot | databento::FlagSet::kBadTsRecv |
      databento::FlagSet::kLast);
  std::vector<databento::Mbp10Msg> output;
  mbo_mbp10::Converter converter{
      [&](const databento::Mbp10Msg& message) { output.push_back(message); }};
  converter.Process(Clear(kFlags));
  const auto stats = converter.Finish();
  CHECK(output.empty());
  CHECK(stats.completed_snapshots == 1);
  CHECK(stats.snapshot_outputs == 0);
  CHECK(stats.completed_events == 1);
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mbo-mbp10-tests-" + std::to_string(nonce));
    std::filesystem::create_directory(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void TestDbnFileRoundTrip() {
  TemporaryDirectory temporary;
  const auto input_path = temporary.Path() / "input.dbn";
  const auto output_path = temporary.Path() / "output.dbn.zst";
  const auto compressed_input_path =
      temporary.Path() / "compressed-input.dbn.zst";
  const auto raw_output_path = temporary.Path() / "raw-output.dbn";

  databento::Metadata metadata{};
  metadata.version = databento::kDbnVersion;
  metadata.dataset = databento::dataset::kGlbxMdp3;
  metadata.schema = databento::Schema::Mbo;
  metadata.start =
      databento::UnixNanos{std::chrono::nanoseconds{1'000'000}};
  metadata.end =
      databento::UnixNanos{std::chrono::nanoseconds{2'000'000}};
  metadata.limit = 0;
  metadata.stype_in = databento::SType::RawSymbol;
  metadata.stype_out = databento::SType::InstrumentId;
  metadata.ts_out = false;
  metadata.symbol_cstr_len = databento::kSymbolCstrLen;
  metadata.symbols = {"TEST"};

  {
    databento::OutFileStream file{input_path};
    databento::DbnEncoder encoder{metadata, &file};
    encoder.EncodeRecord(Clear());
    encoder.EncodeRecord(Message(databento::Action::Add,
                                 databento::Side::Bid, 1, 100, 2, 0, 1, 2));
    encoder.EncodeRecord(Message(databento::Action::Add,
                                 databento::Side::Ask, 2, 110, 3, 0, 1, 3));
    encoder.EncodeRecord(None(databento::FlagSet::kLast, 1, 4));
  }

  const auto stats = mbo_mbp10::ConvertFile(input_path, output_path);
  CHECK(stats.input_records == 4);
  CHECK(stats.output_records == 2);
  CHECK(std::filesystem::is_regular_file(output_path));

  databento::DbnStore output_store{output_path};
  const auto& output_metadata = output_store.GetMetadata();
  CHECK(output_metadata.version == databento::kDbnVersion);
  CHECK(output_metadata.dataset == databento::dataset::kGlbxMdp3);
  CHECK(output_metadata.schema == databento::Schema::Mbp10);
  CHECK(output_metadata.limit == 0);
  CHECK(!output_metadata.ts_out);

  std::vector<databento::Mbp10Msg> output;
  while (const databento::Record* record = output_store.NextRecord()) {
    output.push_back(record->Get<databento::Mbp10Msg>());
  }
  CHECK(output.size() == 2);
  CHECK(output[0].action == databento::Action::Clear);
  CHECK(output[1].action == databento::Action::Add);
  CHECK(output[1].side == databento::Side::Ask);
  CHECK(output[1].levels[0].bid_px == 100);
  CHECK(output[1].levels[0].ask_px == 110);
  CHECK(output[1].flags.IsLast());

  ExpectConversionError(
      [&] { mbo_mbp10::ConvertFile(input_path, output_path); },
      "output already exists");
  mbo_mbp10::FileConversionOptions overwrite_options{};
  overwrite_options.overwrite = true;
  CHECK(mbo_mbp10::ConvertFile(input_path, output_path, overwrite_options)
            .output_records == 2);

  {
    databento::OutFileStream file{compressed_input_path};
    databento::detail::ZstdCompressStream compressed{&file};
    databento::DbnEncoder encoder{metadata, &compressed};
    encoder.EncodeRecord(Clear());
    encoder.EncodeRecord(Message(databento::Action::Add,
                                 databento::Side::Bid, 1, 100, 2, 0, 1, 2));
    encoder.EncodeRecord(Message(databento::Action::Add,
                                 databento::Side::Ask, 2, 110, 3, 0, 1, 3));
    encoder.EncodeRecord(None(databento::FlagSet::kLast, 1, 4));
  }
  CHECK(mbo_mbp10::ConvertFile(compressed_input_path, raw_output_path)
            .output_records == 2);
  databento::DbnStore raw_output_store{raw_output_path};
  CHECK(raw_output_store.GetMetadata().schema == databento::Schema::Mbp10);
  std::size_t raw_record_count = 0;
  while (raw_output_store.NextRecord() != nullptr) {
    ++raw_record_count;
  }
  CHECK(raw_record_count == 2);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"legacy inline F_LAST", TestLegacyInlineLast},
      {"legacy multi-update coalescing", TestLegacyMultiUpdateCoalescing},
      {"standalone N/F_LAST", TestStandaloneLastMarker},
      {"top-10 suppression", TestTopTenSuppressionAndCancel},
      {"aggregate modify/cancel", TestAggregateModifyAndCancel},
      {"trade/fill/cancel", TestTradeFillAndFinalCancel},
      {"interleaved output order", TestInterleavedOutputOrder},
      {"strict validation", TestStrictValidation},
      {"modify as add", TestModifyAsAdd},
      {"historical snapshot collapse", TestHistoricalSnapshotCollapse},
      {"empty snapshot suppression", TestEmptyHistoricalSnapshotIsSuppressed},
      {"DBN file round trip", TestDbnFileRoundTrip},
  };

  std::size_t failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
    }
  }

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " tests passed\n";
  return 0;
}
