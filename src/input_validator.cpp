#include "input_validator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <zstd.h>

#include "databento/record.hpp"
#include "mbo_mbp10/converter.hpp"

namespace mbo_mbp10 {
namespace {

constexpr std::size_t kDbnPreludeSize = 8;
constexpr std::size_t kDetectionSize = 4;

std::uint32_t DecodeLittleEndian32(const std::byte* bytes) {
  return static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(bytes[0])) |
         (static_cast<std::uint32_t>(
              std::to_integer<std::uint8_t>(bytes[1]))
          << 8U) |
         (static_cast<std::uint32_t>(
              std::to_integer<std::uint8_t>(bytes[2]))
          << 16U) |
         (static_cast<std::uint32_t>(
              std::to_integer<std::uint8_t>(bytes[3]))
          << 24U);
}

bool HasDbnPrefix(const std::byte* bytes) {
  return bytes[0] == std::byte{'D'} && bytes[1] == std::byte{'B'} &&
         bytes[2] == std::byte{'N'};
}

class DbnFramingValidator {
 public:
  void Consume(const std::byte* data, std::size_t size) {
    while (size != 0) {
      if (prelude_size_ < prelude_.size()) {
        const auto copy_size =
            std::min(size, prelude_.size() - prelude_size_);
        std::copy(data, data + copy_size,
                  prelude_.begin() +
                      static_cast<std::ptrdiff_t>(prelude_size_));
        prelude_size_ += copy_size;
        data += copy_size;
        size -= copy_size;
        if (prelude_size_ == prelude_.size()) {
          if (!HasDbnPrefix(prelude_.data())) {
            throw ConversionError{
                "decompressed input does not begin with the DBN prefix"};
          }
          metadata_remaining_ =
              DecodeLittleEndian32(prelude_.data() + kDetectionSize);
        }
        continue;
      }

      if (metadata_remaining_ != 0) {
        const auto skip = std::min<std::size_t>(size, metadata_remaining_);
        metadata_remaining_ -= static_cast<std::uint32_t>(skip);
        data += skip;
        size -= skip;
        continue;
      }

      if (record_remaining_ == 0) {
        const auto words = std::to_integer<std::uint8_t>(*data);
        const auto record_size =
            static_cast<std::size_t>(words) *
            databento::RecordHeader::kLengthMultiplier;
        if (record_size < sizeof(databento::RecordHeader) ||
            record_size > databento::kMaxRecordLen) {
          throw ConversionError{"DBN record has an invalid encoded length"};
        }
        record_remaining_ = record_size;
      }

      const auto consume = std::min(size, record_remaining_);
      record_remaining_ -= consume;
      data += consume;
      size -= consume;
    }
  }

  void Finish() const {
    if (prelude_size_ != prelude_.size()) {
      throw ConversionError{"input ended inside the DBN metadata prelude"};
    }
    if (metadata_remaining_ != 0) {
      throw ConversionError{"input ended inside DBN metadata"};
    }
    if (record_remaining_ != 0) {
      throw ConversionError{"input ended inside a DBN record"};
    }
  }

 private:
  std::array<std::byte, kDbnPreludeSize> prelude_{};
  std::size_t prelude_size_{};
  std::uint32_t metadata_remaining_{};
  std::size_t record_remaining_{};
};

std::ifstream OpenInput(const std::filesystem::path& input_path) {
  std::ifstream input{input_path, std::ios::binary};
  if (!input) {
    throw ConversionError{"cannot open input for integrity validation: " +
                          input_path.string()};
  }
  return input;
}

void CheckReadError(const std::ifstream& input,
                    const std::filesystem::path& input_path) {
  if (input.bad()) {
    throw ConversionError{"error reading input during integrity validation: " +
                          input_path.string()};
  }
}

void ValidateRaw(const std::filesystem::path& input_path) {
  auto input = OpenInput(input_path);
  std::vector<std::byte> buffer(1U << 20U);
  DbnFramingValidator validator;
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    const auto read_size = input.gcount();
    if (read_size > 0) {
      validator.Consume(buffer.data(),
                        static_cast<std::size_t>(read_size));
    }
  }
  CheckReadError(input, input_path);
  validator.Finish();
}

struct ZstdDeleter {
  void operator()(ZSTD_DStream* stream) const noexcept {
    ZSTD_freeDStream(stream);
  }
};

void ValidateZstd(const std::filesystem::path& input_path) {
  auto input = OpenInput(input_path);
  std::vector<std::byte> compressed(ZSTD_DStreamInSize());
  std::vector<std::byte> decompressed(ZSTD_DStreamOutSize());
  std::unique_ptr<ZSTD_DStream, ZstdDeleter> stream{ZSTD_createDStream()};
  if (stream == nullptr) {
    throw ConversionError{"cannot allocate a Zstandard validation stream"};
  }
  std::size_t remaining = ZSTD_initDStream(stream.get());
  if (ZSTD_isError(remaining)) {
    throw ConversionError{
        std::string{"cannot initialize Zstandard validation: "} +
        ZSTD_getErrorName(remaining)};
  }

  DbnFramingValidator validator;
  bool read_any_input = false;
  while (input) {
    input.read(reinterpret_cast<char*>(compressed.data()),
               static_cast<std::streamsize>(compressed.size()));
    const auto read_size = input.gcount();
    if (read_size <= 0) {
      continue;
    }
    read_any_input = true;
    ZSTD_inBuffer in_buffer{compressed.data(),
                            static_cast<std::size_t>(read_size), 0};
    while (in_buffer.pos < in_buffer.size) {
      ZSTD_outBuffer out_buffer{decompressed.data(), decompressed.size(), 0};
      const auto previous_position = in_buffer.pos;
      remaining =
          ZSTD_decompressStream(stream.get(), &out_buffer, &in_buffer);
      if (ZSTD_isError(remaining)) {
        throw ConversionError{
            std::string{"invalid Zstandard-compressed DBN input: "} +
            ZSTD_getErrorName(remaining)};
      }
      if (out_buffer.pos != 0) {
        validator.Consume(decompressed.data(), out_buffer.pos);
      }
      if (in_buffer.pos == previous_position && out_buffer.pos == 0) {
        throw ConversionError{
            "Zstandard validation made no decompression progress"};
      }
    }
  }
  CheckReadError(input, input_path);
  if (!read_any_input || remaining != 0) {
    throw ConversionError{
        "input ended before the Zstandard frame was complete"};
  }
  validator.Finish();
}

}  // namespace

void ValidateDbnInput(const std::filesystem::path& input_path) {
  auto input = OpenInput(input_path);
  std::array<std::byte, kDetectionSize> detection{};
  input.read(reinterpret_cast<char*>(detection.data()),
             static_cast<std::streamsize>(detection.size()));
  if (input.gcount() != static_cast<std::streamsize>(detection.size())) {
    throw ConversionError{"input is too short to contain DBN data"};
  }

  if (HasDbnPrefix(detection.data())) {
    ValidateRaw(input_path);
    return;
  }
  if (DecodeLittleEndian32(detection.data()) == ZSTD_MAGICNUMBER) {
    ValidateZstd(input_path);
    return;
  }
  throw ConversionError{"input is neither raw DBN nor Zstandard-compressed DBN"};
}

}  // namespace mbo_mbp10
