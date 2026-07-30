#pragma once

#include <filesystem>

#include "mbo_mbp10/converter.hpp"
#include "mbo_mbp10/parquet_writer.hpp"

namespace mbo_mbp10 {

struct FileConversionOptions {
  ConverterOptions converter{};
  ParquetWriterOptions parquet{};

  // The initial implementation is verified only for CME Globex MDP 3.0.
  bool require_glbx_mdp3{true};

  // Existing output is never replaced unless this is explicitly enabled.
  bool overwrite{false};
};

// Converts one raw or Zstandard-compressed DBN MBO file. Output format and
// compression are selected by the output suffix: .dbn, .dbn.zst, or .parquet.
ConversionStats ConvertFile(const std::filesystem::path& input_path,
                            const std::filesystem::path& output_path,
                            const FileConversionOptions& options = {});

}  // namespace mbo_mbp10
