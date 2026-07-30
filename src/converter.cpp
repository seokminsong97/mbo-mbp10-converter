#include "mbo_mbp10/converter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "databento/constants.hpp"
#include "databento/enums.hpp"
#include "databento/flag_set.hpp"

namespace mbo_mbp10 {
namespace {

constexpr std::size_t kDepth = 10;

std::string BookLabel(const databento::MboMsg& message) {
  std::ostringstream stream;
  stream << "publisher_id=" << message.hd.publisher_id
         << ", instrument_id=" << message.hd.instrument_id
         << ", sequence=" << message.sequence
         << ", action=" << static_cast<char>(message.action);
  return stream.str();
}

[[noreturn]] void Fail(const databento::MboMsg& message,
                       const std::string& reason) {
  throw ConversionError{reason + " (" + BookLabel(message) + ")"};
}

std::uint64_t BookKey(const databento::MboMsg& message) {
  return (static_cast<std::uint64_t>(message.hd.publisher_id) << 32U) |
         static_cast<std::uint64_t>(message.hd.instrument_id);
}

bool IsBookSide(databento::Side side) {
  return side == databento::Side::Bid || side == databento::Side::Ask;
}

bool IsSequenceZeroReset(const databento::MboMsg& message) {
  return message.action == databento::Action::Clear &&
         message.side == databento::Side::None && message.order_id == 0 &&
         message.price == databento::kUndefPrice && message.size == 0 &&
         message.sequence == 0 &&
         message.flags.Raw() == databento::FlagSet::kBadTsRecv;
}

void ValidateOrderFields(const databento::MboMsg& message,
                         bool allow_zero_size) {
  if (!IsBookSide(message.side)) {
    Fail(message, "book-changing record has side=N");
  }
  if (message.order_id == 0) {
    Fail(message, "book-changing record has order_id=0");
  }
  if (message.price == databento::kUndefPrice) {
    Fail(message, "book-changing record has an undefined price");
  }
  if (message.size == databento::kUndefOrderSize) {
    Fail(message, "book-changing record has an undefined size");
  }
  if (!allow_zero_size && message.size == 0) {
    Fail(message, "book-changing record has size=0");
  }
}

struct Level {
  std::uint32_t size{};
  std::uint32_t count{};
};

struct Order {
  std::int64_t price{};
  std::uint32_t size{};
  databento::Side side{databento::Side::None};
};

struct PendingOutput {
  databento::Mbp10Msg message{};
  bool ready{};
};

struct BookCandidate {
  databento::MboMsg source{};
  std::uint8_t depth{};
  bool reset_on_next_visible_update{};
  bool bid_at_depth{};
  bool ask_at_depth{};
};

using BidLevels =
    std::map<std::int64_t, Level, std::greater<std::int64_t>>;
using AskLevels = std::map<std::int64_t, Level, std::less<std::int64_t>>;

struct Book {
  explicit Book(std::size_t order_reserve) {
    orders.reserve(order_reserve);
    orders.max_load_factor(0.8F);
    pending.reserve(16);
  }

  std::unordered_map<std::uint64_t, Order> orders;
  BidLevels bids;
  AskLevels asks;
  std::vector<PendingOutput*> pending;
  std::optional<BookCandidate> book_candidate;
  databento::MboMsg snapshot_source{};
  bool initialized{};
  bool event_open{};
  bool snapshot_event{};
  bool sequence_zero_reset_only{};
};

template <typename Levels>
std::optional<std::uint8_t> VisibleDepth(const Levels& levels,
                                         std::int64_t price) {
  std::uint8_t depth = 0;
  for (auto it = levels.begin(); it != levels.end() && depth < kDepth;
       ++it, ++depth) {
    if (it->first == price) {
      return depth;
    }
  }
  return std::nullopt;
}

std::optional<std::uint8_t> VisibleDepth(const Book& book,
                                         databento::Side side,
                                         std::int64_t price) {
  if (side == databento::Side::Bid) {
    return VisibleDepth(book.bids, price);
  }
  return VisibleDepth(book.asks, price);
}

template <typename Levels>
void ValidateLevelAddition(const Levels& levels, std::int64_t price,
                           std::uint32_t size,
                           const databento::MboMsg& message) {
  const auto it = levels.find(price);
  if (it == levels.end()) {
    return;
  }
  if (size > std::numeric_limits<std::uint32_t>::max() - it->second.size) {
    Fail(message, "aggregate price-level size exceeds DBN uint32 capacity");
  }
  if (it->second.count == std::numeric_limits<std::uint32_t>::max()) {
    Fail(message, "aggregate price-level count exceeds DBN uint32 capacity");
  }
}

template <typename Levels>
void AddToLevel(Levels& levels, std::int64_t price, std::uint32_t size) {
  auto [it, inserted] = levels.try_emplace(price, Level{});
  (void)inserted;
  it->second.size += size;
  ++it->second.count;
}

template <typename Levels>
void RemoveFromLevel(Levels& levels, std::int64_t price, std::uint32_t size,
                     bool remove_order, const databento::MboMsg& message) {
  const auto it = levels.find(price);
  if (it == levels.end() || it->second.size < size ||
      (remove_order && it->second.count == 0)) {
    Fail(message, "internal aggregate level is inconsistent with order state");
  }

  it->second.size -= size;
  if (remove_order) {
    --it->second.count;
  }
  if (it->second.count == 0) {
    if (it->second.size != 0) {
      Fail(message, "empty aggregate level has nonzero size");
    }
    levels.erase(it);
  }
}

void ValidateLevelAddition(const Book& book, databento::Side side,
                           std::int64_t price, std::uint32_t size,
                           const databento::MboMsg& message) {
  if (side == databento::Side::Bid) {
    ValidateLevelAddition(book.bids, price, size, message);
  } else {
    ValidateLevelAddition(book.asks, price, size, message);
  }
}

void AddToLevel(Book& book, databento::Side side, std::int64_t price,
                std::uint32_t size) {
  if (side == databento::Side::Bid) {
    AddToLevel(book.bids, price, size);
  } else {
    AddToLevel(book.asks, price, size);
  }
}

void RemoveFromLevel(Book& book, databento::Side side, std::int64_t price,
                     std::uint32_t size, bool remove_order,
                     const databento::MboMsg& message) {
  if (side == databento::Side::Bid) {
    RemoveFromLevel(book.bids, price, size, remove_order, message);
  } else {
    RemoveFromLevel(book.asks, price, size, remove_order, message);
  }
}

void SetLevelSize(Book& book, databento::Side side, std::int64_t price,
                  std::uint32_t old_order_size, std::uint32_t new_order_size,
                  const databento::MboMsg& message) {
  auto update = [&](auto& levels) {
    const auto it = levels.find(price);
    if (it == levels.end() || it->second.size < old_order_size ||
        it->second.count == 0) {
      Fail(message, "internal aggregate level is inconsistent with order state");
    }
    const std::uint64_t new_level_size =
        static_cast<std::uint64_t>(it->second.size) - old_order_size +
        new_order_size;
    if (new_level_size > std::numeric_limits<std::uint32_t>::max()) {
      Fail(message, "aggregate price-level size exceeds DBN uint32 capacity");
    }
    it->second.size = static_cast<std::uint32_t>(new_level_size);
  };

  if (side == databento::Side::Bid) {
    update(book.bids);
  } else {
    update(book.asks);
  }
}

void ProjectLevels(const Book& book, databento::Mbp10Msg& output) {
  for (auto& level : output.levels) {
    level.bid_px = databento::kUndefPrice;
    level.ask_px = databento::kUndefPrice;
    level.bid_sz = 0;
    level.ask_sz = 0;
    level.bid_ct = 0;
    level.ask_ct = 0;
  }

  auto bid = book.bids.begin();
  auto ask = book.asks.begin();
  for (std::size_t depth = 0; depth < kDepth; ++depth) {
    auto& level = output.levels[depth];
    if (bid != book.bids.end()) {
      level.bid_px = bid->first;
      level.bid_sz = bid->second.size;
      level.bid_ct = bid->second.count;
      ++bid;
    }
    if (ask != book.asks.end()) {
      level.ask_px = ask->first;
      level.ask_sz = ask->second.size;
      level.ask_ct = ask->second.count;
      ++ask;
    }
  }
}

databento::Mbp10Msg MakeOutput(const databento::MboMsg& input,
                               const Book& book, std::uint8_t depth) {
  databento::Mbp10Msg output{};
  output.hd.length = static_cast<std::uint8_t>(
      sizeof(databento::Mbp10Msg) /
      databento::RecordHeader::kLengthMultiplier);
  output.hd.rtype = databento::RType::Mbp10;
  output.hd.publisher_id = input.hd.publisher_id;
  output.hd.instrument_id = input.hd.instrument_id;
  output.hd.ts_event = input.hd.ts_event;
  output.price = input.price;
  output.size = input.size;
  output.action = input.action;
  output.side = input.side;
  output.flags.SetRaw(static_cast<std::uint8_t>(
      input.flags.Raw() &
      static_cast<std::uint8_t>(~databento::FlagSet::kLast)));
  output.depth = depth;
  output.ts_recv = input.ts_recv;
  output.ts_in_delta = input.ts_in_delta;
  output.sequence = input.sequence;
  ProjectLevels(book, output);
  return output;
}

}  // namespace

class Converter::Impl {
 public:
  Impl(OutputSink output_sink, ConverterOptions options)
      : output_sink_{std::move(output_sink)}, options_{options} {
    if (!output_sink_) {
      throw ConversionError{"output sink must not be empty"};
    }
    if (options_.max_buffered_output_records == 0) {
      throw ConversionError{"max_buffered_output_records must be positive"};
    }
    books_.reserve(options_.instrument_reserve);
    books_.max_load_factor(0.8F);
  }

  void Process(const databento::MboMsg& message) {
    if (finished_) {
      throw ConversionError{"cannot process records after Finish()"};
    }
    ++stats_.input_records;

    if (options_.reject_maybe_bad_book && message.flags.IsMaybeBadBook()) {
      Fail(message, "MAYBE_BAD_BOOK is set");
    }
    if (options_.reject_aggregate_messages &&
        (message.flags.IsTob() || message.flags.IsMbp())) {
      Fail(message, "TOB/MBP aggregate message cannot reconstruct MBO state");
    }

    auto [book_it, inserted] =
        books_.try_emplace(BookKey(message),
                           options_.order_reserve_per_instrument);
    Book& book = book_it->second;
    if (inserted) {
      stats_.instruments = static_cast<std::uint64_t>(books_.size());
    }

    ValidateSnapshotTransition(book, message);
    const bool sequence_zero_reset = IsSequenceZeroReset(message);
    if (sequence_zero_reset && book.event_open) {
      Fail(message, "sequence-zero reset arrived before the previous event completed");
    }
    if (sequence_zero_reset) {
      book.sequence_zero_reset_only = true;
    } else if (book.sequence_zero_reset_only) {
      book.sequence_zero_reset_only = false;
    }

    std::optional<std::uint8_t> output_depth;
    bool emit = false;

    switch (message.action) {
      case databento::Action::Clear:
        ++stats_.clears;
        book.orders.clear();
        book.bids.clear();
        book.asks.clear();
        book.initialized = true;
        if (message.flags.IsSnapshot()) {
          book.snapshot_event = true;
          book.snapshot_source = message;
        } else {
          emit = true;
          output_depth = 0;
        }
        break;

      case databento::Action::Add:
        ++stats_.adds;
        EnsureInitialized(book, message);
        output_depth = Add(book, message);
        emit = output_depth.has_value();
        break;

      case databento::Action::Cancel:
        ++stats_.cancels;
        EnsureInitialized(book, message);
        output_depth = Cancel(book, message);
        emit = output_depth.has_value();
        break;

      case databento::Action::Modify:
        ++stats_.modifies;
        EnsureInitialized(book, message);
        output_depth = Modify(book, message);
        emit = output_depth.has_value();
        break;

      case databento::Action::Trade:
        ++stats_.trades;
        EnsureInitialized(book, message);
        emit = true;
        output_depth = 0;
        break;

      case databento::Action::Fill:
        ++stats_.fills;
        EnsureInitialized(book, message);
        break;

      case databento::Action::None:
        ++stats_.none_markers;
        break;

      default:
        Fail(message, "unsupported MBO action");
    }

    book.event_open = true;
    if (book.snapshot_event) {
      if (message.action == databento::Action::Add) {
        book.snapshot_source = message;
      }
      ProcessSnapshotBoundary(book, message);
      return;
    }

    if (emit && message.action == databento::Action::Trade) {
      if (book.book_candidate.has_value()) {
        book.book_candidate->reset_on_next_visible_update = true;
      }
      BufferOutput(book, MakeOutput(message, book, *output_depth), message);
    } else if (message.action == databento::Action::Add ||
               message.action == databento::Action::Cancel ||
               message.action == databento::Action::Modify) {
      TrackBookUpdate(book, message, output_depth);
    } else if (emit) {
      TrackBookUpdate(book, message, output_depth);
    }

    if (message.flags.IsLast()) {
      const bool emitted_book_update =
          BufferCoalescedBookUpdate(book, message);
      const bool boundary_is_trade =
          message.action == databento::Action::Trade;
      CompleteEvent(book, emitted_book_update || boundary_is_trade);
    }
  }

  ConversionStats Finish() {
    if (finished_) {
      return stats_;
    }

    for (auto& [key, book] : books_) {
      if (book.event_open) {
        const bool empty_sequence_zero_reset =
            book.sequence_zero_reset_only && book.pending.empty() &&
            book.book_candidate.has_value() &&
            IsSequenceZeroReset(book.book_candidate->source);
        if (empty_sequence_zero_reset) {
          book.book_candidate.reset();
          book.event_open = false;
          book.sequence_zero_reset_only = false;
          continue;
        }
        std::ostringstream stream;
        stream << "input ended before F_LAST for publisher_id=" << (key >> 32U)
               << ", instrument_id="
               << static_cast<std::uint32_t>(key);
        throw ConversionError{stream.str()};
      }
    }

    FlushReady();
    if (!output_queue_.empty()) {
      throw ConversionError{"internal output queue contains an unready event"};
    }
    finished_ = true;
    return stats_;
  }

  const ConversionStats& Stats() const noexcept { return stats_; }

 private:
  void ValidateSnapshotTransition(Book& book,
                                  const databento::MboMsg& message) {
    if (book.snapshot_event) {
      const bool standalone_boundary =
          message.action == databento::Action::None &&
          message.flags.IsLast();
      if (!message.flags.IsSnapshot() && !standalone_boundary) {
        Fail(message,
             "non-snapshot record arrived before snapshot event completed");
      }
      if (message.action != databento::Action::Add &&
          message.action != databento::Action::None) {
        Fail(message, "snapshot may contain only R, A, and final N records");
      }
      return;
    }

    if (!message.flags.IsSnapshot()) {
      return;
    }
    if (message.action != databento::Action::Clear) {
      Fail(message, "snapshot does not begin with action=R (clear)");
    }
    if (book.event_open) {
      Fail(message, "snapshot began before the previous event completed");
    }
  }

  void ProcessSnapshotBoundary(Book& book,
                               const databento::MboMsg& message) {
    if (!message.flags.IsLast()) {
      return;
    }

    ++stats_.completed_snapshots;
    bool emitted_snapshot = false;
    if (!book.bids.empty() || !book.asks.empty()) {
      auto output = MakeOutput(book.snapshot_source, book, 0);
      output.side = databento::Side::None;
      BufferOutput(book, std::move(output), message);
      ++stats_.snapshot_outputs;
      emitted_snapshot = true;
    }
    book.snapshot_event = false;
    CompleteEvent(book, emitted_snapshot);
  }

  static void SetAffectedSide(BookCandidate& candidate,
                              databento::Side side) {
    if (side == databento::Side::Bid) {
      candidate.bid_at_depth = true;
    } else if (side == databento::Side::Ask) {
      candidate.ask_at_depth = true;
    } else {
      candidate.bid_at_depth = true;
      candidate.ask_at_depth = true;
    }
  }

  void TrackBookUpdate(Book& book, const databento::MboMsg& message,
                       std::optional<std::uint8_t> depth) {
    if (!depth.has_value()) {
      ++stats_.invisible_book_updates;
      if (book.book_candidate.has_value()) {
        book.book_candidate->source = message;
      }
      return;
    }

    if (!book.book_candidate.has_value()) {
      book.book_candidate =
          BookCandidate{message, *depth, false, false, false};
      SetAffectedSide(*book.book_candidate, message.side);
      return;
    }

    ++stats_.coalesced_book_updates;
    BookCandidate& candidate = *book.book_candidate;
    candidate.source = message;
    if (candidate.reset_on_next_visible_update) {
      candidate.depth = *depth;
      candidate.reset_on_next_visible_update = false;
      candidate.bid_at_depth = false;
      candidate.ask_at_depth = false;
    }
    if (*depth < candidate.depth) {
      candidate.depth = *depth;
      candidate.bid_at_depth = false;
      candidate.ask_at_depth = false;
      SetAffectedSide(candidate, message.side);
    } else if (*depth == candidate.depth) {
      SetAffectedSide(candidate, message.side);
    }
  }

  static databento::Side AffectedSide(const BookCandidate& candidate) {
    if (candidate.bid_at_depth == candidate.ask_at_depth) {
      return databento::Side::None;
    }
    return candidate.bid_at_depth ? databento::Side::Bid
                                  : databento::Side::Ask;
  }

  bool BufferCoalescedBookUpdate(Book& book,
                                 const databento::MboMsg& boundary) {
    if (!book.book_candidate.has_value()) {
      return false;
    }
    const BookCandidate candidate = *book.book_candidate;
    book.book_candidate.reset();
    auto output = MakeOutput(candidate.source, book, candidate.depth);
    output.side = AffectedSide(candidate);
    BufferOutput(book, std::move(output), boundary);
    return true;
  }

  void EnsureInitialized(Book& book, const databento::MboMsg& message) {
    if (book.initialized) {
      return;
    }
    if (options_.require_initial_clear) {
      Fail(message, "first book record is not action=R (clear)");
    }
    book.initialized = true;
  }

  std::optional<std::uint8_t> Add(Book& book,
                                  const databento::MboMsg& message) {
    ValidateOrderFields(message, false);
    if (book.orders.find(message.order_id) != book.orders.end()) {
      Fail(message, "duplicate add for an existing order_id");
    }

    ValidateLevelAddition(book, message.side, message.price, message.size,
                          message);
    book.orders.emplace(
        message.order_id,
        Order{message.price, message.size, message.side});
    AddToLevel(book, message.side, message.price, message.size);
    return VisibleDepth(book, message.side, message.price);
  }

  std::optional<std::uint8_t> Cancel(Book& book,
                                     const databento::MboMsg& message) {
    ValidateOrderFields(message, false);
    const auto order_it = book.orders.find(message.order_id);
    if (order_it == book.orders.end()) {
      Fail(message, "cancel references an unknown order_id");
    }

    Order& order = order_it->second;
    if (order.side != message.side || order.price != message.price) {
      Fail(message, "cancel side/price does not match resting order");
    }
    if (message.size > order.size) {
      Fail(message, "cancel size exceeds resting order size");
    }

    const auto old_depth = VisibleDepth(book, order.side, order.price);
    const bool remove_order = message.size == order.size;
    RemoveFromLevel(book, order.side, order.price, message.size, remove_order,
                    message);
    if (remove_order) {
      book.orders.erase(order_it);
    } else {
      order.size -= message.size;
    }
    return old_depth;
  }

  std::optional<std::uint8_t> Modify(Book& book,
                                     const databento::MboMsg& message) {
    ValidateOrderFields(message, true);
    auto order_it = book.orders.find(message.order_id);
    if (order_it == book.orders.end()) {
      if (message.size == 0) {
        Fail(message, "zero-size modify references an unknown order_id");
      }
      ++stats_.modifies_as_add;
      ValidateLevelAddition(book, message.side, message.price, message.size,
                            message);
      book.orders.emplace(
          message.order_id,
          Order{message.price, message.size, message.side});
      AddToLevel(book, message.side, message.price, message.size);
      return VisibleDepth(book, message.side, message.price);
    }

    const Order old_order = order_it->second;
    if (old_order.side != message.side) {
      Fail(message, "modify changes the side of a resting order");
    }
    const auto old_depth =
        VisibleDepth(book, old_order.side, old_order.price);

    if (old_order.price == message.price) {
      if (message.size == 0) {
        RemoveFromLevel(book, old_order.side, old_order.price, old_order.size,
                        true, message);
        book.orders.erase(order_it);
        return old_depth;
      }
      SetLevelSize(book, old_order.side, old_order.price, old_order.size,
                   message.size, message);
      order_it->second.size = message.size;
      return VisibleDepth(book, message.side, message.price);
    }

    if (message.size != 0) {
      ValidateLevelAddition(book, message.side, message.price, message.size,
                            message);
    }
    RemoveFromLevel(book, old_order.side, old_order.price, old_order.size, true,
                    message);
    if (message.size == 0) {
      book.orders.erase(order_it);
      return old_depth;
    }

    AddToLevel(book, message.side, message.price, message.size);
    order_it->second = Order{message.price, message.size, message.side};
    const auto new_depth = VisibleDepth(book, message.side, message.price);
    if (new_depth.has_value() && old_depth.has_value()) {
      return std::min(*new_depth, *old_depth);
    }
    return new_depth.has_value() ? new_depth : old_depth;
  }

  void BufferOutput(Book& book, databento::Mbp10Msg output,
                    const databento::MboMsg& source) {
    if (output_queue_.size() >= options_.max_buffered_output_records) {
      Fail(source, "buffered output limit exceeded before event completion");
    }

    output_queue_.push_back(PendingOutput{std::move(output), false});
    // std::deque insertion at an end preserves references to existing
    // elements, so these pointers remain valid until their event is flushed.
    book.pending.push_back(&output_queue_.back());
    stats_.peak_buffered_output_records =
        std::max(stats_.peak_buffered_output_records,
                 static_cast<std::uint64_t>(output_queue_.size()));
  }

  void CompleteEvent(Book& book, bool mark_last) {
    if (!book.pending.empty()) {
      if (mark_last) {
        PendingOutput* final = book.pending.back();
        final->message.flags.SetRaw(
            static_cast<std::uint8_t>(final->message.flags.Raw() |
                                      databento::FlagSet::kLast));
      }
      for (PendingOutput* pending : book.pending) {
        pending->ready = true;
      }
      book.pending.clear();
    }

    book.event_open = false;
    ++stats_.completed_events;
    FlushReady();
  }

  void FlushReady() {
    while (!output_queue_.empty() && output_queue_.front().ready) {
      output_sink_(output_queue_.front().message);
      output_queue_.pop_front();
      ++stats_.output_records;
    }
  }

  OutputSink output_sink_;
  ConverterOptions options_;
  ConversionStats stats_{};
  std::unordered_map<std::uint64_t, Book> books_;
  std::deque<PendingOutput> output_queue_;
  bool finished_{};
};

Converter::Converter(OutputSink output_sink, ConverterOptions options)
    : impl_{std::make_unique<Impl>(std::move(output_sink), options)} {}

Converter::~Converter() = default;
Converter::Converter(Converter&&) noexcept = default;
Converter& Converter::operator=(Converter&&) noexcept = default;

void Converter::Process(const databento::MboMsg& message) {
  impl_->Process(message);
}

ConversionStats Converter::Finish() { return impl_->Finish(); }

const ConversionStats& Converter::Stats() const noexcept {
  return impl_->Stats();
}

}  // namespace mbo_mbp10
