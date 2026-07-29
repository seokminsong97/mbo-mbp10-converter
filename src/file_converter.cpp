#include "mbo_mbp10/file_converter.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "databento/constants.hpp"
#include "databento/dbn.hpp"
#include "databento/dbn_encoder.hpp"
#include "databento/dbn_store.hpp"
#include "databento/detail/zstd_stream.hpp"
#include "databento/enums.hpp"
#include "databento/file_stream.hpp"
#include "databento/record.hpp"

namespace mbo_mbp10 {
namespace {

bool EndsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

enum class OutputCompression { None, Zstd };

OutputCompression ParseOutputCompression(
    const std::filesystem::path& output_path) {
  const std::string filename = output_path.filename().string();
  if (EndsWith(filename, ".dbn.zst")) {
    return OutputCompression::Zstd;
  }
  if (EndsWith(filename, ".dbn")) {
    return OutputCompression::None;
  }
  throw ConversionError{
      "output filename must end in .dbn or .dbn.zst"};
}

std::filesystem::path NormalizedAbsolute(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    throw ConversionError{"cannot resolve path '" + path.string() +
                          "': " + error.message()};
  }
  return absolute.lexically_normal();
}

std::filesystem::path MakeTemporaryPath(
    const std::filesystem::path& output_path) {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  for (std::uint32_t attempt = 0; attempt < 1000; ++attempt) {
    auto candidate = output_path;
    candidate += ".tmp." + std::to_string(nonce) + "." +
                 std::to_string(attempt);
    if (!std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  throw ConversionError{"unable to allocate a temporary output filename"};
}

class TemporaryOutput {
 public:
  explicit TemporaryOutput(std::filesystem::path path)
      : path_{std::move(path)} {}

  ~TemporaryOutput() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  const std::filesystem::path& Path() const noexcept { return path_; }
  void Commit() noexcept { committed_ = true; }

 private:
  std::filesystem::path path_;
  bool committed_{};
};

databento::Metadata OutputMetadata(const databento::Metadata& input) {
  databento::Metadata output = input;
  output.version = databento::kDbnVersion;
  output.schema = databento::Schema::Mbp10;
  output.limit = 0;
  output.ts_out = false;
  return output;
}

ConversionStats EncodeRecords(databento::DbnStore& input,
                              databento::DbnEncoder& encoder,
                              const ConverterOptions& options) {
  Converter converter{
      [&](const databento::Mbp10Msg& output) {
        encoder.EncodeRecord(output);
      },
      options};

  while (const databento::Record* record = input.NextRecord()) {
    const auto* mbo = record->GetIf<databento::MboMsg>();
    if (mbo == nullptr) {
      throw ConversionError{
          "MBO metadata contained a non-MBO data record"};
    }
    converter.Process(*mbo);
  }
  return converter.Finish();
}

}  // namespace

ConversionStats ConvertFile(const std::filesystem::path& input_path,
                            const std::filesystem::path& output_path,
                            const FileConversionOptions& options) {
  const auto compression = ParseOutputCompression(output_path);
  const auto input_absolute = NormalizedAbsolute(input_path);
  const auto output_absolute = NormalizedAbsolute(output_path);

  if (input_absolute == output_absolute) {
    throw ConversionError{"input and output paths must be different"};
  }
  if (!std::filesystem::is_regular_file(input_absolute)) {
    throw ConversionError{"input is not a regular file: " +
                          input_absolute.string()};
  }
  if (std::filesystem::exists(output_absolute) && !options.overwrite) {
    throw ConversionError{"output already exists (use --force to replace it): " +
                          output_absolute.string()};
  }

  const auto parent = output_absolute.parent_path();
  if (!std::filesystem::is_directory(parent)) {
    throw ConversionError{"output directory does not exist: " +
                          parent.string()};
  }

  databento::DbnStore input{input_absolute};
  const databento::Metadata& metadata = input.GetMetadata();
  if (!metadata.schema.has_value() ||
      *metadata.schema != databento::Schema::Mbo) {
    throw ConversionError{"input DBN metadata schema must be MBO"};
  }
  if (metadata.ts_out) {
    throw ConversionError{
        "input with ts_out=true is not supported; request DBN without ts_out"};
  }
  if (options.require_glbx_mdp3 &&
      metadata.dataset != databento::dataset::kGlbxMdp3) {
    throw ConversionError{
        "input dataset must be GLBX.MDP3 unless dataset validation is disabled"};
  }

  const auto output_metadata = OutputMetadata(metadata);
  TemporaryOutput temporary{MakeTemporaryPath(output_absolute)};
  ConversionStats stats{};

  if (compression == OutputCompression::Zstd) {
    databento::OutFileStream file{temporary.Path()};
    databento::detail::ZstdCompressStream compressed{&file};
    databento::DbnEncoder encoder{output_metadata, &compressed};
    stats = EncodeRecords(input, encoder, options.converter);
  } else {
    databento::OutFileStream file{temporary.Path()};
    databento::DbnEncoder encoder{output_metadata, &file};
    stats = EncodeRecords(input, encoder, options.converter);
  }

  std::error_code rename_error;
  std::filesystem::rename(temporary.Path(), output_absolute, rename_error);
  if (rename_error) {
    throw ConversionError{"cannot commit output file: " +
                          rename_error.message()};
  }
  temporary.Commit();
  return stats;
}

}  // namespace mbo_mbp10
