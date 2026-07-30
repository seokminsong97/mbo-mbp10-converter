#include "mbo_mbp10/parquet_writer.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <parquet/types.h>

#include "databento/constants.hpp"
#include "databento/enums.hpp"
#include "mbo_mbp10/converter.hpp"

namespace mbo_mbp10 {
namespace {

constexpr std::size_t kBookDepth = 10;

void CheckArrow(const arrow::Status& status, std::string_view operation) {
  if (!status.ok()) {
    throw ConversionError{std::string{operation} + ": " + status.ToString()};
  }
}

template <typename T>
T ArrowValue(arrow::Result<T> result, std::string_view operation) {
  if (!result.ok()) {
    throw ConversionError{std::string{operation} + ": " +
                          result.status().ToString()};
  }
  return std::move(result).ValueUnsafe();
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream output;
  output << '"';
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20) {
          output << "\\u" << std::hex << std::setw(4)
                 << std::setfill('0') << static_cast<unsigned>(character)
                 << std::dec << std::setfill(' ');
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  output << '"';
  return output.str();
}

std::string JsonStrings(const std::vector<std::string>& values) {
  std::ostringstream output;
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    output << JsonEscape(values[index]);
  }
  output << ']';
  return output.str();
}

std::string MetadataJson(const databento::Metadata& metadata) {
  std::ostringstream output;
  output << '{'
         << "\"version\":" << static_cast<unsigned>(metadata.version)
         << ",\"dataset\":" << JsonEscape(metadata.dataset)
         << ",\"schema\":";
  if (metadata.schema.has_value()) {
    output << JsonEscape(databento::ToString(*metadata.schema));
  } else {
    output << "null";
  }
  output << ",\"start\":" << metadata.start.time_since_epoch().count()
         << ",\"end\":" << metadata.end.time_since_epoch().count()
         << ",\"limit\":" << metadata.limit << ",\"stype_in\":";
  if (metadata.stype_in.has_value()) {
    output << JsonEscape(databento::ToString(*metadata.stype_in));
  } else {
    output << "null";
  }
  output << ",\"stype_out\":"
         << JsonEscape(databento::ToString(metadata.stype_out))
         << ",\"ts_out\":" << (metadata.ts_out ? "true" : "false")
         << ",\"symbol_cstr_len\":" << metadata.symbol_cstr_len
         << ",\"symbols\":" << JsonStrings(metadata.symbols)
         << ",\"partial\":" << JsonStrings(metadata.partial)
         << ",\"not_found\":" << JsonStrings(metadata.not_found)
         << ",\"mappings\":[";

  for (std::size_t mapping_index = 0;
       mapping_index < metadata.mappings.size(); ++mapping_index) {
    if (mapping_index != 0) {
      output << ',';
    }
    const auto& mapping = metadata.mappings[mapping_index];
    output << "{\"raw_symbol\":" << JsonEscape(mapping.raw_symbol)
           << ",\"intervals\":[";
    for (std::size_t interval_index = 0;
         interval_index < mapping.intervals.size(); ++interval_index) {
      if (interval_index != 0) {
        output << ',';
      }
      const auto& interval = mapping.intervals[interval_index];
      output << "{\"start\":"
             << JsonEscape(date::format("%F", interval.start_date))
             << ",\"end\":"
             << JsonEscape(date::format("%F", interval.end_date))
             << ",\"symbol\":" << JsonEscape(interval.symbol) << '}';
    }
    output << "]}";
  }
  output << "]}";
  return output.str();
}

std::shared_ptr<arrow::KeyValueMetadata> SchemaMetadata(
    const databento::Metadata& metadata) {
  auto result = std::make_shared<arrow::KeyValueMetadata>();
  result->Append("dbn.metadata", MetadataJson(metadata));
  result->Append("dbn.dataset", metadata.dataset);
  result->Append(
      "dbn.schema",
      metadata.schema.has_value() ? databento::ToString(*metadata.schema) : "");
  result->Append("dbn.version", std::to_string(metadata.version));
  result->Append("mbo_mbp10.price_encoding", "fixed");
  result->Append("mbo_mbp10.price_scale", "1e-9");
  result->Append("mbo_mbp10.undefined_price",
                 std::to_string(databento::kUndefPrice));
  return result;
}

std::string LevelName(std::string_view prefix, std::size_t depth) {
  std::ostringstream result;
  result << prefix << '_' << std::setw(2) << std::setfill('0') << depth;
  return result.str();
}

std::shared_ptr<arrow::Schema> Mbp10Schema(
    const databento::Metadata& metadata) {
  const auto timestamp = arrow::timestamp(arrow::TimeUnit::NANO, "UTC");
  std::vector<std::shared_ptr<arrow::Field>> fields;
  fields.reserve(13 + kBookDepth * 6);
  fields.push_back(arrow::field("ts_recv", timestamp, false));
  fields.push_back(arrow::field("ts_event", timestamp, false));
  fields.push_back(arrow::field("rtype", arrow::uint8(), false));
  fields.push_back(arrow::field("publisher_id", arrow::uint16(), false));
  fields.push_back(arrow::field("instrument_id", arrow::uint32(), false));
  fields.push_back(arrow::field("action", arrow::utf8(), false));
  fields.push_back(arrow::field("side", arrow::utf8(), false));
  fields.push_back(arrow::field("depth", arrow::uint8(), false));
  fields.push_back(arrow::field("price", arrow::int64(), false));
  fields.push_back(arrow::field("size", arrow::uint32(), false));
  fields.push_back(arrow::field("flags", arrow::uint8(), false));
  fields.push_back(arrow::field("ts_in_delta", arrow::int32(), false));
  fields.push_back(arrow::field("sequence", arrow::uint32(), false));
  for (std::size_t depth = 0; depth < kBookDepth; ++depth) {
    fields.push_back(
        arrow::field(LevelName("bid_px", depth), arrow::int64(), false));
    fields.push_back(
        arrow::field(LevelName("ask_px", depth), arrow::int64(), false));
    fields.push_back(
        arrow::field(LevelName("bid_sz", depth), arrow::uint32(), false));
    fields.push_back(
        arrow::field(LevelName("ask_sz", depth), arrow::uint32(), false));
    fields.push_back(
        arrow::field(LevelName("bid_ct", depth), arrow::uint32(), false));
    fields.push_back(
        arrow::field(LevelName("ask_ct", depth), arrow::uint32(), false));
  }
  return std::make_shared<arrow::Schema>(std::move(fields),
                                         SchemaMetadata(metadata));
}

template <typename Builder>
void Reserve(Builder& builder, std::size_t row_count) {
  CheckArrow(builder.Reserve(static_cast<std::int64_t>(row_count)),
             "cannot reserve a Parquet column buffer");
}

template <typename Builder>
void FinishColumn(Builder& builder,
                  std::vector<std::shared_ptr<arrow::Array>>& columns) {
  std::shared_ptr<arrow::Array> column;
  CheckArrow(builder.Finish(&column), "cannot finish a Parquet column");
  columns.push_back(std::move(column));
}

}  // namespace

class Mbp10ParquetWriter::Impl {
 public:
  Impl(const std::filesystem::path& output_path,
       const databento::Metadata& metadata, ParquetWriterOptions options)
      : schema_{Mbp10Schema(metadata)}, row_group_size_{options.row_group_size} {
    if (!metadata.schema.has_value() ||
        *metadata.schema != databento::Schema::Mbp10) {
      throw ConversionError{"Parquet writer metadata schema must be MBP-10"};
    }
    if (metadata.ts_out) {
      throw ConversionError{
          "Parquet writer does not support appended ts_out records"};
    }
    if (row_group_size_ == 0 ||
        row_group_size_ >
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      throw ConversionError{"Parquet row group size must be positive"};
    }

    rows_.reserve(row_group_size_);
    output_ = ArrowValue(arrow::io::FileOutputStream::Open(output_path.string()),
                         "cannot open Parquet output");

    parquet::WriterProperties::Builder parquet_properties;
    parquet_properties
        .compression(parquet::Compression::ZSTD)
        ->max_row_group_length(static_cast<std::int64_t>(row_group_size_));
    parquet::ArrowWriterProperties::Builder arrow_properties;
    arrow_properties.store_schema();
    writer_ = ArrowValue(
        parquet::arrow::FileWriter::Open(
            *schema_, arrow::default_memory_pool(), output_,
            parquet_properties.build(), arrow_properties.build()),
        "cannot initialize Parquet writer");
  }

  ~Impl() {
    try {
      Close();
    } catch (...) {
    }
  }

  void Append(const databento::Mbp10Msg& message) {
    if (closed_) {
      throw ConversionError{"cannot append to a closed Parquet writer"};
    }
    rows_.push_back(message);
    if (rows_.size() == row_group_size_) {
      Flush();
    }
  }

  void Close() {
    if (closed_) {
      return;
    }
    Flush();
    CheckArrow(writer_->Close(), "cannot finalize Parquet output");
    CheckArrow(output_->Close(), "cannot close Parquet output");
    closed_ = true;
  }

 private:
  void ReserveColumns(std::size_t row_count) {
    Reserve(ts_recv_, row_count);
    Reserve(ts_event_, row_count);
    Reserve(rtype_, row_count);
    Reserve(publisher_id_, row_count);
    Reserve(instrument_id_, row_count);
    Reserve(action_, row_count);
    CheckArrow(action_.ReserveData(static_cast<std::int64_t>(row_count)),
               "cannot reserve the Parquet action column");
    Reserve(side_, row_count);
    CheckArrow(side_.ReserveData(static_cast<std::int64_t>(row_count)),
               "cannot reserve the Parquet side column");
    Reserve(depth_, row_count);
    Reserve(price_, row_count);
    Reserve(size_, row_count);
    Reserve(flags_, row_count);
    Reserve(ts_in_delta_, row_count);
    Reserve(sequence_, row_count);
    for (std::size_t level = 0; level < kBookDepth; ++level) {
      Reserve(bid_px_[level], row_count);
      Reserve(ask_px_[level], row_count);
      Reserve(bid_sz_[level], row_count);
      Reserve(ask_sz_[level], row_count);
      Reserve(bid_ct_[level], row_count);
      Reserve(ask_ct_[level], row_count);
    }
  }

  void AppendUnsafe(const databento::Mbp10Msg& message) {
    const auto ts_recv = message.ts_recv.time_since_epoch().count();
    const auto ts_event = message.hd.ts_event.time_since_epoch().count();
    if (ts_recv >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) ||
        ts_event >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
      throw ConversionError{
          "timestamp exceeds the signed nanosecond range supported by Parquet"};
    }

    ts_recv_.UnsafeAppend(static_cast<std::int64_t>(ts_recv));
    ts_event_.UnsafeAppend(static_cast<std::int64_t>(ts_event));
    rtype_.UnsafeAppend(static_cast<std::uint8_t>(message.hd.rtype));
    publisher_id_.UnsafeAppend(message.hd.publisher_id);
    instrument_id_.UnsafeAppend(message.hd.instrument_id);
    const char action = static_cast<char>(message.action);
    const char side = static_cast<char>(message.side);
    action_.UnsafeAppend(&action, 1);
    side_.UnsafeAppend(&side, 1);
    depth_.UnsafeAppend(message.depth);
    price_.UnsafeAppend(message.price);
    size_.UnsafeAppend(message.size);
    flags_.UnsafeAppend(message.flags.Raw());
    ts_in_delta_.UnsafeAppend(message.ts_in_delta.count());
    sequence_.UnsafeAppend(message.sequence);
    for (std::size_t level = 0; level < kBookDepth; ++level) {
      const auto& pair = message.levels[level];
      bid_px_[level].UnsafeAppend(pair.bid_px);
      ask_px_[level].UnsafeAppend(pair.ask_px);
      bid_sz_[level].UnsafeAppend(pair.bid_sz);
      ask_sz_[level].UnsafeAppend(pair.ask_sz);
      bid_ct_[level].UnsafeAppend(pair.bid_ct);
      ask_ct_[level].UnsafeAppend(pair.ask_ct);
    }
  }

  std::vector<std::shared_ptr<arrow::Array>> FinishColumns() {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(static_cast<std::size_t>(schema_->num_fields()));
    FinishColumn(ts_recv_, columns);
    FinishColumn(ts_event_, columns);
    FinishColumn(rtype_, columns);
    FinishColumn(publisher_id_, columns);
    FinishColumn(instrument_id_, columns);
    FinishColumn(action_, columns);
    FinishColumn(side_, columns);
    FinishColumn(depth_, columns);
    FinishColumn(price_, columns);
    FinishColumn(size_, columns);
    FinishColumn(flags_, columns);
    FinishColumn(ts_in_delta_, columns);
    FinishColumn(sequence_, columns);
    for (std::size_t level = 0; level < kBookDepth; ++level) {
      FinishColumn(bid_px_[level], columns);
      FinishColumn(ask_px_[level], columns);
      FinishColumn(bid_sz_[level], columns);
      FinishColumn(ask_sz_[level], columns);
      FinishColumn(bid_ct_[level], columns);
      FinishColumn(ask_ct_[level], columns);
    }
    return columns;
  }

  void Flush() {
    if (rows_.empty()) {
      return;
    }
    ReserveColumns(rows_.size());
    for (const auto& message : rows_) {
      AppendUnsafe(message);
    }

    const auto row_count = static_cast<std::int64_t>(rows_.size());
    auto batch =
        arrow::RecordBatch::Make(schema_, row_count, FinishColumns());
    CheckArrow(writer_->WriteRecordBatch(*batch),
               "cannot write a Parquet row group");
    rows_.clear();
  }

  std::shared_ptr<arrow::Schema> schema_;
  std::size_t row_group_size_;
  std::vector<databento::Mbp10Msg> rows_;
  std::shared_ptr<arrow::io::FileOutputStream> output_;
  std::unique_ptr<parquet::arrow::FileWriter> writer_;
  bool closed_{};

  arrow::TimestampBuilder ts_recv_{arrow::timestamp(arrow::TimeUnit::NANO,
                                                    "UTC"),
                                   arrow::default_memory_pool()};
  arrow::TimestampBuilder ts_event_{arrow::timestamp(arrow::TimeUnit::NANO,
                                                     "UTC"),
                                    arrow::default_memory_pool()};
  arrow::UInt8Builder rtype_;
  arrow::UInt16Builder publisher_id_;
  arrow::UInt32Builder instrument_id_;
  arrow::StringBuilder action_;
  arrow::StringBuilder side_;
  arrow::UInt8Builder depth_;
  arrow::Int64Builder price_;
  arrow::UInt32Builder size_;
  arrow::UInt8Builder flags_;
  arrow::Int32Builder ts_in_delta_;
  arrow::UInt32Builder sequence_;
  std::array<arrow::Int64Builder, kBookDepth> bid_px_;
  std::array<arrow::Int64Builder, kBookDepth> ask_px_;
  std::array<arrow::UInt32Builder, kBookDepth> bid_sz_;
  std::array<arrow::UInt32Builder, kBookDepth> ask_sz_;
  std::array<arrow::UInt32Builder, kBookDepth> bid_ct_;
  std::array<arrow::UInt32Builder, kBookDepth> ask_ct_;
};

Mbp10ParquetWriter::Mbp10ParquetWriter(
    const std::filesystem::path& output_path,
    const databento::Metadata& metadata, ParquetWriterOptions options)
    : impl_{std::make_unique<Impl>(output_path, metadata, options)} {}

Mbp10ParquetWriter::~Mbp10ParquetWriter() = default;
Mbp10ParquetWriter::Mbp10ParquetWriter(Mbp10ParquetWriter&&) noexcept =
    default;
Mbp10ParquetWriter& Mbp10ParquetWriter::operator=(
    Mbp10ParquetWriter&&) noexcept = default;

void Mbp10ParquetWriter::Append(const databento::Mbp10Msg& message) {
  if (impl_ == nullptr) {
    throw ConversionError{"cannot append to a moved-from Parquet writer"};
  }
  impl_->Append(message);
}

void Mbp10ParquetWriter::Close() {
  if (impl_ != nullptr) {
    impl_->Close();
  }
}

}  // namespace mbo_mbp10
