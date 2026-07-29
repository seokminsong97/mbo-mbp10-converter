#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "mbo_mbp10/file_converter.hpp"

namespace {

void PrintUsage(std::ostream& stream) {
  stream
      << "Usage: mbo-mbp10 [options] INPUT.dbn[.zst] OUTPUT.dbn[.zst]\n"
      << "\n"
      << "Options:\n"
      << "  --force                Replace an existing output file\n"
      << "  --allow-bad-book       Continue when MAYBE_BAD_BOOK is set\n"
      << "  --allow-partial        Do not require an initial action=R clear\n"
      << "  --allow-other-dataset  Disable the GLBX.MDP3 metadata check\n"
      << "  --quiet                Do not print conversion statistics\n"
      << "  --help                  Show this help\n"
      << "  --version               Show the converter version\n";
}

}  // namespace

int main(int argc, char** argv) {
  mbo_mbp10::FileConversionOptions options{};
  bool quiet = false;
  std::vector<std::string> positional;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      PrintUsage(std::cout);
      return EXIT_SUCCESS;
    }
    if (argument == "--version") {
      std::cout << "mbo-mbp10 " << MBO_MBP10_VERSION << '\n';
      return EXIT_SUCCESS;
    }
    if (argument == "--force") {
      options.overwrite = true;
    } else if (argument == "--allow-bad-book") {
      options.converter.reject_maybe_bad_book = false;
    } else if (argument == "--allow-partial") {
      options.converter.require_initial_clear = false;
    } else if (argument == "--allow-other-dataset") {
      options.require_glbx_mdp3 = false;
    } else if (argument == "--quiet") {
      quiet = true;
    } else if (!argument.empty() && argument.front() == '-') {
      std::cerr << "error: unknown option: " << argument << "\n\n";
      PrintUsage(std::cerr);
      return EXIT_FAILURE;
    } else {
      positional.push_back(argument);
    }
  }

  if (positional.size() != 2) {
    PrintUsage(std::cerr);
    return EXIT_FAILURE;
  }

  if (!options.converter.require_initial_clear && !quiet) {
    std::cerr
        << "warning: --allow-partial can produce incomplete MBP-10 state\n";
  }

  try {
    const auto started = std::chrono::steady_clock::now();
    const auto stats = mbo_mbp10::ConvertFile(
        std::filesystem::path{positional[0]},
        std::filesystem::path{positional[1]}, options);
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    if (!quiet) {
      const double rate =
          elapsed > 0.0
              ? static_cast<double>(stats.input_records) / elapsed
              : 0.0;
      std::cout << "converted " << stats.input_records << " MBO records into "
                << stats.output_records << " MBP-10 records across "
                << stats.instruments << " books in " << std::fixed
                << std::setprecision(3) << elapsed << " s ("
                << std::setprecision(0) << rate << " input records/s)\n";
      std::cout << "events=" << stats.completed_events
                << ", snapshots=" << stats.completed_snapshots
                << ", invisible_updates=" << stats.invisible_book_updates
                << ", coalesced_updates=" << stats.coalesced_book_updates
                << ", modifies_as_add=" << stats.modifies_as_add
                << ", peak_buffered_outputs="
                << stats.peak_buffered_output_records << '\n';
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
