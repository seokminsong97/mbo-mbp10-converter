#pragma once

#include <filesystem>

namespace mbo_mbp10 {

// Verifies container completion and DBN record framing before conversion.
// This is stricter than the SDK decoder, which can treat an incomplete Zstd
// frame or a trailing partial DBN record as a clean EOF.
void ValidateDbnInput(const std::filesystem::path& input_path);

}  // namespace mbo_mbp10
