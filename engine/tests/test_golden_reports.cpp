// Golden reports: the text the original analyser printed for simulated traces, kept as files in
// engine/tests/golden/. The engine must keep printing exactly this, whoever changes it later.
// A difference here means the report's numbers or wording changed: if that was intended, replace
// the golden file on purpose, in its own commit.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "render.h"
#include "report.h"
#include "simulate.h"
#include "test_support.h"
#include "trace.h"

namespace fs = std::filesystem;
using namespace sessionscope;

namespace {

std::string read_whole_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void check_report_matches_golden(const char *scenario, const SimulateOptions &options, const char *golden_name) {
    const fs::path golden_folder = fs::path(__FILE__).parent_path() / "golden";
    const std::string golden = read_whole_file(golden_folder / golden_name);
    CHECK(!golden.empty());

    // The folder's name appears in the report's first line, so it must match the golden file's.
    const fs::path temporary = fs::temp_directory_path() / "sessionscope_test_golden";
    fs::remove_all(temporary);
    const fs::path folder = temporary / fs::path(golden_name).stem();
    write_simulated_trace(folder, scenario, options);

    const TraceReader trace = load_trace(folder);
    const Style plain_unicode(false, true);
    const std::string report = report_text(trace, "lateness", DEFAULT_MIN_BLOCKS, DEFAULT_Q, &plain_unicode);
    CHECK(report + "\n" == golden);
    if (report + "\n" != golden) std::fprintf(stderr, "  which report: %s\n", golden_name);

    fs::remove_all(temporary);
}

}  // namespace

int main() {
    check_report_matches_golden("song", {}, "song.txt");
    check_report_matches_golden("staircase", {}, "staircase.txt");

    SimulateOptions lost_records;
    lost_records.drop_every = 2;
    check_report_matches_golden("song", lost_records, "song_many_lost_records.txt");

    SimulateOptions restarted;
    restarted.reset_at = 5000;
    check_report_matches_golden("song", restarted, "song_sensor_restart.txt");

    return finish_tests();
}
