#ifndef SESSIONSCOPE_SIMULATE_H
#define SESSIONSCOPE_SIMULATE_H

// Synthetic traces in the recorder's exact format, for tests and demos.
//
// Nothing here is a measurement: the load model is invented so the pipeline has something with
// known answers to chew on. meta.json says so ("synthetic": true), and so does every report of it.
// The damage options produce the F6 fixtures: lost records, a sensor restart, a cut-off file.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sessionscope {

// What gets played: bars first up to, but not including, end. A "start" begins with a second of
// stopped transport; a "loop" follows the previous part with no gap.
struct PlanPart {
    int first_bar;
    int end_bar;
    std::string kind;
};

struct Scenario {
    std::string name;
    int bars;
    std::vector<std::pair<int, int>> tempo;   // (bar, bpm), 4/4 throughout
    std::vector<PlanPart> plan;
    std::function<double(int)> load;          // bar -> share of the deadline the session needs
    std::function<double(int)> workload;      // bar -> known Workload share (fixture F7). Empty if there
                                              // is none, which is different from a workload of zero.
};

const Scenario *scenario_by_name(std::string_view name);   // nullptr if unknown
std::vector<std::string> scenario_names();

struct SimulateOptions {
    int seed = 1;
    int srate = 48000;
    int frames = 256;
    int drop_every = 0;             // lose every Nth sensor record
    std::optional<int> reset_at;    // restart the sensor's sequence once, after N records
    bool truncate = false;          // cut the last sensor row in half
    bool host = true;               // write host.csv
};

// Writes a synthetic trace folder and returns its path.
std::filesystem::path write_simulated_trace(const std::filesystem::path &out, const Scenario &scenario,
                                            const SimulateOptions &options = {});

// The same, by scenario name. Throws std::invalid_argument if there is no such scenario.
std::filesystem::path write_simulated_trace(const std::filesystem::path &out, std::string_view scenario_name,
                                            const SimulateOptions &options = {});

}  // namespace sessionscope

#endif  // SESSIONSCOPE_SIMULATE_H
