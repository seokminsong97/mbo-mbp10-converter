#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>

#include "databento/dbn.hpp"
#include "databento/record.hpp"

namespace mbo_mbp10 {

struct ParquetWriterOptions {
  // Each completed batch becomes a Parquet row group. Memory use is bounded by
  // this number of MBP-10 records.
  std::size_t row_group_size{65536};
};

// Streaming, lossless writer for the fixed-point MBP-10 tabular schema.
//
// Prices remain signed DBN fixed-point integers (1 unit = 1e-9), timestamps
// remain nanosecond-resolution timestamps, and undefined prices remain
// databento::kUndefPrice rather than being converted to null.
class Mbp10ParquetWriter {
 public:
  Mbp10ParquetWriter(const std::filesystem::path& output_path,
                     const databento::Metadata& metadata,
                     ParquetWriterOptions options = {});
  ~Mbp10ParquetWriter();

  Mbp10ParquetWriter(const Mbp10ParquetWriter&) = delete;
  Mbp10ParquetWriter& operator=(const Mbp10ParquetWriter&) = delete;
  Mbp10ParquetWriter(Mbp10ParquetWriter&&) noexcept;
  Mbp10ParquetWriter& operator=(Mbp10ParquetWriter&&) noexcept;

  void Append(const databento::Mbp10Msg& message);

  // Flushes the final row group and writes the Parquet footer. Calling Close
  // more than once is allowed.
  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mbo_mbp10
