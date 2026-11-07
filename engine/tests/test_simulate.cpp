// Tests for the simulator and PythonRandom.
// PythonRandom values verified against:
//   python -c "import random; r=random.Random(SEED); print([r.random() for _ in range(5)])"
//   python -c "import random; r=random.Random(SEED); print([r.gauss(0,1) for _ in range(3)])"
//   python -c "import random; r=random.Random(SEED); print([r.expovariate(1.5) for _ in range(3)])"
// For seeds 0, 1, 2, 123456.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "python_random.h"
#include "simulate.h"
#include "test_support.h"
#include "trace.h"
#include "json.h"

using namespace sessionscope;

namespace fs = std::filesystem;

// Expected random() values for seeds 0, 1, 2, 123456 (first 5 values each)
// Verified against: python -c "import random; r=random.Random(SEED); print([r.random() for _ in range(5)])"
static const double EXPECTED_RANDOM[4][5] = {
    // seed 0
    {0.8444218515250481, 0.7579544029403025, 0.420571580830845, 0.25891675029296335, 0.5112747213686085},
    // seed 1
    {0.13436424411240122, 0.84743373693723267, 0.76377461897661403, 0.2550690257394217, 0.49543508709194095},
    // seed 2
    {0.95603427188924939, 0.94782748705934938, 0.056551367726808688, 0.084871995158921631, 0.83549887812944956},
    // seed 123456
    {0.80562713625890003, 0.79405901051809813, 0.029425761106168014, 0.17465638335376021, 0.0022298761599784944}
};

void test_python_random() {
    // Test random() for seeds 0, 1, 2, 123456
    for (int si = 0; si < 4; ++si) {
        uint64_t seed = (si == 3) ? 123456 : si;
        PythonRandom rng(seed);
        for (int i = 0; i < 5; ++i) {
            double expected = EXPECTED_RANDOM[si][i];
            double actual = rng.random();
            CHECK_NEAR(actual, expected, 0.0);  // exact match for random()
        }
    }

    // Test gauss and expovariate in a mixed sequence for seed 1
    // Python: r=random.Random(1); [r.gauss(0,1), r.expovariate(1.5), r.gauss(0,1), r.random(), r.expovariate(1.5)]
    {
        PythonRandom rng(1);
        double g1 = rng.gauss(0.0, 1.0);
        double e1 = rng.expovariate(1.5);
        double g2 = rng.gauss(0.0, 1.0);
        double r1 = rng.random();
        double e2 = rng.expovariate(1.5);
        // Expected from Python (seed 1):
        CHECK_NEAR(g1, 1.2881847531554629, 1e-12);
        CHECK_NEAR(e1, 0.96197928356444196, 1e-12);
        CHECK_NEAR(g2, 1.4494456086997709, 1e-12);
        CHECK_NEAR(r1, 0.2550690257394217, 0.0);
        CHECK_NEAR(e2, 0.45603918643973351, 1e-12);
    }

    // Test gauss for seed 0
    {
        PythonRandom rng(0);
        double g1 = rng.gauss(0.0, 1.0);
        double g2 = rng.gauss(0.0, 1.0);
        double g3 = rng.gauss(0.0, 1.0);
        CHECK_NEAR(g1, 0.94171540468066439, 1e-12);
        CHECK_NEAR(g2, -1.3965781047011498, 1e-12);
        CHECK_NEAR(g3, -0.67971444807842107, 1e-12);
    }

    // Test expovariate for seed 0
    {
        PythonRandom rng(0);
        double e1 = rng.expovariate(1.0);
        double e2 = rng.expovariate(1.0);
        double e3 = rng.expovariate(1.0);
        CHECK_NEAR(e1, 1.8606071110652234, 1e-12);
        CHECK_NEAR(e2, 1.4186291529717618, 1e-12);
        CHECK_NEAR(e3, 0.54571314554159278, 1e-12);
    }
}

void test_simulate_basic() {
    fs::path tmp = fs::temp_directory_path() / "sessionscope_test_simulate";
    fs::remove_all(tmp);

    SimulateOptions opts;
    opts.seed = 1;
    opts.srate = 48000;
    opts.frames = 256;
    opts.drop_every = 0;
    opts.truncate = false;
    opts.host = true;

    auto folder = write_simulated_trace(tmp, "song", opts);

    // Check that all expected files exist
    CHECK(fs::exists(folder / "sensor.csv"));
    CHECK(fs::exists(folder / "transport.csv"));
    CHECK(fs::exists(folder / "events.csv"));
    CHECK(fs::exists(folder / "host.csv"));
    CHECK(fs::exists(folder / "meta.json"));

    // Load the trace and verify basics
    {
        TraceReader trace(folder);
        trace.update();

        // synthetic flag in meta.json
        std::string meta_text;
        {
            std::ifstream f(folder / "meta.json", std::ios::binary);
            meta_text = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        auto meta = sessionscope::parse_json(meta_text);
        CHECK(meta.has_value());
        CHECK(meta->operator[]("synthetic").boolean() == true);
        CHECK(meta->operator[]("scenario").text() == "song");
        CHECK(meta->operator[]("seed").number() == 1.0);

        // sensor records > 0
        CHECK(trace.sensor().size() > 0);

        // transport records > 0
        CHECK(trace.transport().size() > 0);

        // events include recorder_start, sensor, recorder_stop
        const auto &events = trace.events();
        bool has_recorder_start = false, has_sensor = false, has_recorder_stop = false;
        for (const auto &e : events) {
            if (e.kind == "recorder_start") has_recorder_start = true;
            if (e.kind == "sensor") has_sensor = true;
            if (e.kind == "recorder_stop") has_recorder_stop = true;
        }
        CHECK(has_recorder_start);
        CHECK(has_sensor);
        CHECK(has_recorder_stop);
    }

    fs::remove_all(tmp);
}

void test_simulate_drop_every() {
    fs::path tmp = fs::temp_directory_path() / "sessionscope_test_simulate_drop";
    fs::remove_all(tmp);

    SimulateOptions opts;
    opts.seed = 1;
    opts.drop_every = 97;  // lose every 97th record

    auto folder = write_simulated_trace(tmp, "song", opts);

    {
        TraceReader trace(folder);
        trace.update();

        // Check that blocks are missing (dropped records)
        // The Blocks class will detect gaps in seq
        // We can check the summary in meta.json
        std::string meta_text;
        {
            std::ifstream f(folder / "meta.json", std::ios::binary);
            meta_text = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        auto meta = sessionscope::parse_json(meta_text);
        CHECK(meta.has_value());
        double dropped = meta->operator[]("summary")["dropped_records"].number();
        CHECK(dropped > 0);
    }

    fs::remove_all(tmp);
}

void test_simulate_reset_at() {
    fs::path tmp = fs::temp_directory_path() / "sessionscope_test_simulate_reset";
    fs::remove_all(tmp);

    SimulateOptions opts;
    opts.seed = 1;
    opts.reset_at = 5000;

    auto folder = write_simulated_trace(tmp, "song", opts);

    {
        TraceReader trace(folder);
        trace.update();

        std::string meta_text;
        {
            std::ifstream f(folder / "meta.json", std::ios::binary);
            meta_text = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        auto meta = sessionscope::parse_json(meta_text);
        CHECK(meta.has_value());
        // The simulator writes a ring_reset event
        bool has_ring_reset = false;
        for (const auto &e : trace.events()) {
            if (e.kind == "ring_reset") has_ring_reset = true;
        }
        CHECK(has_ring_reset);
    }

    fs::remove_all(tmp);
}

void test_simulate_truncate() {
    fs::path tmp = fs::temp_directory_path() / "sessionscope_test_simulate_truncate";
    fs::remove_all(tmp);

    SimulateOptions opts;
    opts.seed = 1;
    opts.truncate = true;

    auto folder = write_simulated_trace(tmp, "song", opts);

    {
        TraceReader trace(folder);
        trace.update();

        // The sensor.csv should be truncated (last 7 bytes removed)
        // This will cause the last row to be incomplete when parsed
        // We can check that the file size is 7 bytes smaller than it would be
        // For now just verify it runs without error
        CHECK(fs::exists(folder / "sensor.csv"));
    }

    fs::remove_all(tmp);
}

void test_simulate_no_host() {
    fs::path tmp = fs::temp_directory_path() / "sessionscope_test_simulate_nohost";
    fs::remove_all(tmp);

    SimulateOptions opts;
    opts.seed = 1;
    opts.host = false;

    auto folder = write_simulated_trace(tmp, "song", opts);

    CHECK(!fs::exists(folder / "host.csv"));
    CHECK(fs::exists(folder / "sensor.csv"));
    CHECK(fs::exists(folder / "transport.csv"));
    CHECK(fs::exists(folder / "events.csv"));
    CHECK(fs::exists(folder / "meta.json"));

    {
        TraceReader trace(folder);
        trace.update();
        CHECK(trace.host().size() == 0);
    }

    std::string meta_text;
    {
        std::ifstream f(folder / "meta.json", std::ios::binary);
        meta_text = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    auto meta = sessionscope::parse_json(meta_text);
    CHECK(meta.has_value());

    fs::remove_all(tmp);
}

void test_simulate_deterministic() {
    fs::path tmp1 = fs::temp_directory_path() / "sessionscope_test_simulate_det1";
    fs::path tmp2 = fs::temp_directory_path() / "sessionscope_test_simulate_det2";
    fs::remove_all(tmp1);
    fs::remove_all(tmp2);

    SimulateOptions opts;
    opts.seed = 42;

    write_simulated_trace(tmp1, "song", opts);
    write_simulated_trace(tmp2, "song", opts);

    // sensor.csv should be byte-identical
    std::string s1, s2;
    {
        std::ifstream f1(tmp1 / "sensor.csv", std::ios::binary);
        std::ifstream f2(tmp2 / "sensor.csv", std::ios::binary);
        s1 = std::string((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
        s2 = std::string((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    }
    CHECK(s1 == s2);

    // Different seed gives different text
    fs::path tmp3 = fs::temp_directory_path() / "sessionscope_test_simulate_det3";
    fs::remove_all(tmp3);
    opts.seed = 43;
    write_simulated_trace(tmp3, "song", opts);
    std::string s3;
    {
        std::ifstream f3(tmp3 / "sensor.csv", std::ios::binary);
        s3 = std::string((std::istreambuf_iterator<char>(f3)), std::istreambuf_iterator<char>());
    }
    CHECK(s1 != s3);

    fs::remove_all(tmp1);
    fs::remove_all(tmp2);
    fs::remove_all(tmp3);
}

void test_staircase_scenario() {
    fs::path tmp = fs::temp_directory_path() / "sessionscope_test_staircase";
    fs::remove_all(tmp);

    SimulateOptions opts;
    opts.seed = 1;

    auto folder = write_simulated_trace(tmp, "staircase", opts);

    CHECK(fs::exists(folder / "sensor.csv"));
    CHECK(fs::exists(folder / "meta.json"));

    std::string meta_text;
    {
        std::ifstream f(folder / "meta.json", std::ios::binary);
        meta_text = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    auto meta = sessionscope::parse_json(meta_text);
    CHECK(meta.has_value());
    CHECK(meta->operator[]("scenario").text() == "staircase");

    {
        TraceReader trace(folder);
        trace.update();
        CHECK(trace.sensor().size() > 0);
    }

    fs::remove_all(tmp);
}

int main() {
    test_python_random();
    test_simulate_basic();
    test_simulate_drop_every();
    test_simulate_reset_at();
    test_simulate_truncate();
    test_simulate_no_host();
    test_simulate_deterministic();
    test_staircase_scenario();
    return finish_tests();
}
