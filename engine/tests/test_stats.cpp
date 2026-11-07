// Tests for the per-bar statistics.
// Each test block corresponds to a Python test with the same name in comments.

#include "stats.h"
#include "test_support.h"
#include "trace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <vector>

using namespace sessionscope;

int main() {
    // Quantiles::test_nearest_rank
    {
        std::vector<double> v(100);
        for (int i = 0; i < 100; ++i) v[i] = static_cast<double>(i + 1);
        CHECK_NEAR(quantile(v, 0.99), 99.0, 1e-9);
        CHECK_NEAR(quantile(v, 0.5), 50.0, 1e-9);
        CHECK_NEAR(quantile(v, 1.0), 100.0, 1e-9);
        CHECK_NEAR(quantile(std::vector<double>{5.0}, 0.99), 5.0, 1e-9);
        CHECK(std::isnan(quantile(std::vector<double>{}, 0.99)));
    }

    // Quantiles::test_p99_of_fewer_than_100_values_is_the_max
    {
        std::vector<double> v(50);
        for (int i = 0; i < 50; ++i) v[i] = static_cast<double>(i);
        CHECK_NEAR(quantile(v, 0.99), 49.0, 1e-9);
    }

    // Quantiles::test_median
    {
        CHECK_NEAR(median(std::vector<double>{3.0, 1.0, 2.0}), 2.0, 1e-9);
        CHECK_NEAR(median(std::vector<double>{4.0, 1.0, 3.0, 2.0}), 2.5, 1e-9);
    }

    // Tiers::test_lateness_tiers_match_the_prototype
    {
        const Metric &m = metric_by_key("lateness");
        CHECK(tier(m, 0.05) == 0);
        CHECK(tier(m, 0.10) == 1);
        CHECK(tier(m, 0.25) == 2);
        CHECK(tier(m, 0.40) == 3);
        CHECK(tier(m, std::numeric_limits<double>::quiet_NaN()) == -1);
    }

    // Tiers::test_workload_tiers_are_shares_of_the_deadline
    {
        const Metric &m = metric_by_key("workload");
        CHECK(tier(m, 0.69) == 0);
        CHECK(tier(m, 1.0) == 3);
    }

    // AcrossPasses::test_median_of_per_pass_values_ignores_bars_without_enough_blocks
    {
        PassCells heat;
        heat[1][33] = Cell{0.1, 300, 1};
        heat[1][34] = Cell{std::nullopt, 10, 1};
        heat[2][33] = Cell{0.3, 300, 1};
        heat[2][34] = Cell{0.2, 300, 1};
        heat[3][33] = Cell{0.2, 300, 1};

        BarCells across = across_passes(heat);
        CHECK_NEAR(across[33].value.value_or(NAN), 0.2, 1e-9);
        CHECK(across[33].passes == 3);
        CHECK_NEAR(across[34].value.value_or(NAN), 0.2, 1e-9);
        CHECK(across[34].passes == 1);
        CHECK(across[34].n == 310);
    }

    // AcrossPasses::test_few_and_none_states
    {
        CHECK(std::string(Cell{std::nullopt, 10, 1}.state()) == "few");
        CHECK(std::string(Cell{std::nullopt, 0, 1}.state()) == "none");
        CHECK(std::string(Cell{0.1, 100, 1}.state()) == "ok");
    }

    // Hottest::test_longest_run_at_risk_or_worse
    {
        const Metric &m = metric_by_key("lateness");
        BarCells heat;
        heat[1] = Cell{0.05, 100, 1};
        heat[2] = Cell{0.3, 100, 1};
        heat[3] = Cell{0.05, 100, 1};
        heat[10] = Cell{0.25, 100, 1};
        heat[11] = Cell{0.5, 100, 1};
        heat[12] = Cell{0.21, 100, 1};
        heat[13] = Cell{0.0, 100, 1};

        auto result = hottest(heat, m);
        CHECK(result.has_value());
        CHECK(result->first == 10);
        CHECK(result->second == 12);
    }

    // Hottest::test_none_when_calm
    {
        const Metric &m = metric_by_key("lateness");
        BarCells heat;
        heat[1] = Cell{0.01, 100, 1};
        auto result = hottest(heat, m);
        CHECK(!result.has_value());
    }

    // Spearman::test_monotonic
    {
        CHECK_NEAR(spearman({1.0, 2.0, 3.0, 4.0}, {10.0, 20.0, 25.0, 100.0}), 1.0, 1e-9);
        CHECK_NEAR(spearman({1.0, 2.0, 3.0, 4.0}, {4.0, 3.0, 2.0, 1.0}), -1.0, 1e-9);
    }

    // Spearman::test_ties_and_constants
    {
        CHECK_NEAR(spearman({1.0, 1.0, 2.0, 2.0}, {1.0, 1.0, 2.0, 2.0}), 1.0, 1e-9);
        CHECK(std::isnan(spearman({1.0, 2.0, 3.0}, {5.0, 5.0, 5.0})));
        CHECK(std::isnan(spearman({1.0, 2.0}, {1.0, 2.0})));
    }

    // Spearman::test_nan_pairs_are_skipped
    {
        CHECK_NEAR(spearman({1.0, 2.0, std::numeric_limits<double>::quiet_NaN(), 3.0},
                            {1.0, 2.0, 9.0, 3.0}), 1.0, 1e-9);
    }

    // Additional: metric_by_key throws on unknown
    {
        bool threw = false;
        try {
            metric_by_key("unknown");
        } catch (const std::out_of_range &) {
            threw = true;
        }
        CHECK(threw);
    }

    // Additional: metric_values throws on a metric the source does not have
    {
        Table sensor(SENSOR_COLUMNS);
        Table host(HOST_COLUMNS);
        Table transport(TRANSPORT_COLUMNS);
        const Blocks sensor_blocks(sensor);
        const HostBlocks host_blocks(host, transport);

        bool sensor_threw = false;
        try {
            metric_values(sensor_blocks, metric_by_key("host_rt"));
        } catch (const std::out_of_range &) {
            sensor_threw = true;
        }
        CHECK(sensor_threw);

        bool host_threw = false;
        try {
            metric_values(host_blocks, metric_by_key("lateness"));
        } catch (const std::out_of_range &) {
            host_threw = true;
        }
        CHECK(host_threw);
    }

    // Additional: bar_cells and across_passes integration
    {
        // Create simple test data
        std::vector<int> pass_ids = {1, 1, 1, 1, 2, 2, 2, 2};
        std::vector<int> bars = {1, 1, 2, 2, 1, 1, 2, 2};
        std::vector<double> values = {0.1, 0.2, 0.3, 0.4, 0.15, 0.25, 0.35, 0.45};

        // q=0.5 with nearest-rank on 2 elements gives the first (index 0 after sort)
        PassCells cells = bar_cells(pass_ids, bars, values, 0.5, 2);
        CHECK(cells[1][1].value.has_value());
        CHECK_NEAR(cells[1][1].value.value(), 0.1, 1e-9);   // nearest-rank median of {0.1, 0.2} -> 0.1
        CHECK(cells[1][2].value.has_value());
        CHECK_NEAR(cells[1][2].value.value(), 0.3, 1e-9);   // nearest-rank median of {0.3, 0.4} -> 0.3
        CHECK(cells[2][1].value.has_value());
        CHECK_NEAR(cells[2][1].value.value(), 0.15, 1e-9);  // nearest-rank median of {0.15, 0.25} -> 0.15
        CHECK(cells[2][2].value.has_value());
        CHECK_NEAR(cells[2][2].value.value(), 0.35, 1e-9);  // nearest-rank median of {0.35, 0.45} -> 0.35

        BarCells across = across_passes(cells);
        CHECK(across[1].value.has_value());
        CHECK_NEAR(across[1].value.value(), 0.125, 1e-9);  // true median of {0.1, 0.15} -> 0.125
        CHECK(across[2].value.has_value());
        CHECK_NEAR(across[2].value.value(), 0.325, 1e-9);  // true median of {0.3, 0.35} -> 0.325
        CHECK(across[1].passes == 2);
        CHECK(across[2].passes == 2);
    }

    // Additional: bar_cells skips pid<=0 or bar<=0
    {
        std::vector<int> pass_ids = {0, 1, -1, 1};
        std::vector<int> bars = {1, 0, 1, 2};
        std::vector<double> values = {1.0, 2.0, 3.0, 4.0};

        PassCells cells = bar_cells(pass_ids, bars, values, 0.5, 1);
        CHECK(cells.size() == 1);
        CHECK(cells[1].size() == 1);
        CHECK(cells[1][2].n == 1);
        CHECK_NEAR(cells[1][2].value.value(), 4.0, 1e-9);
    }

    // Additional: bar_cells NaN values count as seen but give no number
    {
        std::vector<int> pass_ids = {1, 1, 1, 1};
        std::vector<int> bars = {1, 1, 1, 1};
        std::vector<double> values = {1.0, std::numeric_limits<double>::quiet_NaN(),
                                      std::numeric_limits<double>::quiet_NaN(),
                                      std::numeric_limits<double>::quiet_NaN()};

        PassCells cells = bar_cells(pass_ids, bars, values, 0.5, 2);
        CHECK(cells[1][1].n == 4);  // all 4 seen
        CHECK(!cells[1][1].value.has_value());  // only 1 non-NaN, min_blocks=2
    }

    // ------------------------------------------------------------
    // LiveBarCells: fed in uneven chunks, it gives exactly what bar_cells gives over all the blocks
    // ------------------------------------------------------------
    {
        // Two passes over bars 1-6 with stops between them, a few NaNs and unplaced blocks.
        std::vector<int> pass_ids, bars;
        std::vector<double> values;
        unsigned seed = 7;
        const auto next_random = [&seed] { seed = seed * 1103515245u + 12345u; return (seed >> 8) % 1000; };
        for (int pass = 1; pass <= 2; ++pass) {
            for (int i = 0; i < 6 * 40; ++i) {
                pass_ids.push_back(pass);
                bars.push_back(i % 97 == 5 ? 0 : 1 + i / 40);   // now and then a block not placed in a bar
                values.push_back(next_random() < 30 ? std::nan("") : next_random() / 1000.0);
            }
            for (int i = 0; i < 10; ++i) {   // stopped
                pass_ids.push_back(0);
                bars.push_back(0);
                values.push_back(0.5);
            }
        }

        LiveBarCells live(0.99, 20);
        const std::size_t chunks[] = {1, 39, 2, 150, 7, 300, 11};
        std::size_t given = 0;
        for (std::size_t chunk : chunks) {
            const std::size_t end = std::min(given + chunk, values.size());
            const std::vector<int> ids_so_far(pass_ids.begin(), pass_ids.begin() + end);
            const std::vector<int> bars_so_far(bars.begin(), bars.begin() + end);
            live.add(ids_so_far, bars_so_far, std::vector<double>(values.begin() + given, values.begin() + end));
            given = end;
            const std::vector<double> values_so_far(values.begin(), values.begin() + end);
            const PassCells expected = bar_cells(ids_so_far, bars_so_far, values_so_far, 0.99, 20);
            const PassCells &got = live.cells();
            bool same = expected.size() == got.size();
            for (const auto &[pass, cells] : expected) {
                for (const auto &[bar, cell] : cells) {
                    const auto pass_found = got.find(pass);
                    if (pass_found == got.end() || !pass_found->second.count(bar)) {
                        same = false;
                        continue;
                    }
                    const Cell &other = pass_found->second.at(bar);
                    same = same && other.n == cell.n && other.value == cell.value;
                }
            }
            CHECK(same);
        }
        CHECK(given == values.size());
        CHECK(live.done() == values.size());

        // One bar's blocks, pass by pass, from the same values.
        {
            LiveBarCells detail_cells(0.99, 2);
            // Pass 1, bar 3: 0.1, 0.5, NaN, 0.2. Pass 2, bar 3: 0.45 alone. Bar 4 must not show up.
            detail_cells.add({1, 1, 1, 1, 1, 2}, {3, 3, 3, 3, 4, 3},
                             {0.1, 0.5, std::nan(""), 0.2, 0.9, 0.45});
            const std::vector<BarPassDetail> d = detail_cells.bar_detail(3, 0.40);
            CHECK(d.size() == 2);
            CHECK(d.size() == 2 && d[0].pass == 1 && d[0].blocks == 4 && d[0].median == 0.2 && d[0].worst == 0.5);
            CHECK(d.size() == 2 && d[0].p99 == 0.5 && d[0].overruns == 1);   // 0.5 is at or over 0.40
            CHECK(d.size() == 2 && d[1].pass == 2 && d[1].blocks == 1 && !d[1].p99 && d[1].overruns == 1);
            CHECK(detail_cells.bar_detail(7, 0.40).empty());
        }

        // Shorter columns mean the data started over: so does the live view.
        live.add({1, 1}, {1, 1}, {0.1, 0.2});
        CHECK(live.done() == 2);
        CHECK(live.cells().size() == 1 && live.cells().at(1).at(1).n == 2);
    }

    return finish_tests();
}
