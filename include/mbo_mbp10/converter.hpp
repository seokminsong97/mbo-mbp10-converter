#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>

#include "databento/record.hpp"

namespace mbo_mbp10 {

struct ConverterOptions {
  // Reject a stream that starts in the middle of a book. A normal historical
  // MBO stream should begin each reconstructed book with action=R.
  bool require_initial_clear{true};

  // Continuing through a feed gap can produce a plausible but incorrect book.
  bool reject_maybe_bad_book{true};

  // MBO reconstruction requires individual-order messages, not TOB/MBP input.
  bool reject_aggregate_messages{true};

  std::size_t instrument_reserve{4096};
  std::size_t order_reserve_per_instrument{256};

  // Ready events may wait behind an earlier unfinished interleaved event. This
  // bound prevents malformed input from consuming memory without limit.
  std::size_t max_buffered_output_records{262144};
};

struct ConversionStats {
  std::uint64_t input_records{};
  std::uint64_t output_records{};
  std::uint64_t completed_events{};
  std::uint64_t instruments{};

  std::uint64_t adds{};
  std::uint64_t cancels{};
  std::uint64_t modifies{};
  std::uint64_t clears{};
  std::uint64_t trades{};
  std::uint64_t fills{};
  std::uint64_t none_markers{};
  std::uint64_t completed_snapshots{};
  std::uint64_t snapshot_outputs{};

  std::uint64_t invisible_book_updates{};
  std::uint64_t coalesced_book_updates{};
  std::uint64_t modifies_as_add{};
  std::uint64_t peak_buffered_output_records{};
};

class ConversionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Converter {
 public:
  using OutputSink = std::function<void(const databento::Mbp10Msg&)>;

  explicit Converter(OutputSink output_sink,
                     ConverterOptions options = ConverterOptions{});
  ~Converter();

  Converter(const Converter&) = delete;
  Converter& operator=(const Converter&) = delete;
  Converter(Converter&&) noexcept;
  Converter& operator=(Converter&&) noexcept;

  void Process(const databento::MboMsg& message);

  // Validates that no event is cut off at EOF and flushes ready output.
  // Calling Finish more than once is allowed.
  ConversionStats Finish();

  const ConversionStats& Stats() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mbo_mbp10
