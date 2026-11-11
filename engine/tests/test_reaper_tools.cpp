// Tests for the REAPER tools (engine/src/reaper_tools.h): linking and copying the JSFX and scripts
// into a resource folder, the audio settings in reaper.ini, and the validation summary. A scratch
// folder stands in for both the repository and REAPER's resource folder; no REAPER is started and
// the real REAPER settings are never touched.

#include "reaper_tools.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace sessionscope;

static void write_file(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

static std::string read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

// How many times `part` appears in `text`.
static int count(const std::string &text, const std::string &part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}

int main() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path scratch = fs::temp_directory_path() / ("sessionscope_test_reaper_tools_" + std::to_string(now));

    // A stand-in repository with one JSFX and one script.
    const fs::path repo = scratch / "repo";
    write_file(repo / "hosts" / "reaper" / "Effects" / "SessionScope" / "Sensor.jsfx", "jsfx");
    write_file(repo / "hosts" / "reaper" / "Scripts" / "SessionScope" / "lib" / "util.lua", "lua");
    const fs::path resource = scratch / "resource";
    fs::create_directories(resource);
    const fs::path effects = resource / "Effects" / "SessionScope";
    const fs::path scripts = resource / "Scripts" / "SessionScope";

    // ---- linking: REAPER sees the repository's own files, and an edit shows through at once
    CHECK(reaper_install(repo, resource, false, true).empty());
    CHECK(read_file(effects / "Sensor.jsfx") == "jsfx");
    CHECK(read_file(scripts / "lib" / "util.lua") == "lua");
    write_file(repo / "hosts" / "reaper" / "Effects" / "SessionScope" / "Sensor.jsfx", "edited");
    CHECK(read_file(effects / "Sensor.jsfx") == "edited");
    CHECK(reaper_install(repo, resource, false, true).empty());      // installing again replaces our own link

    // ---- a repository path typed with "/" and ".." must still give a working link (on Windows a
    //      junction that stores "/" leads nowhere: this once broke the validation run)
    reaper_uninstall(repo, resource);
    const fs::path typed = fs::u8path((scratch / "repo" / ".." / "repo").generic_u8string());
    CHECK(reaper_install(typed, resource, false, true).empty());
    CHECK(read_file(effects / "Sensor.jsfx") == "edited");
    CHECK(read_file(scripts / "lib" / "util.lua") == "lua");

    // ---- uninstalling removes the links and leaves the repository's files where they are
    reaper_uninstall(repo, resource);
    CHECK(!fs::exists(effects));
    CHECK(!fs::exists(scripts));
    CHECK(read_file(repo / "hosts" / "reaper" / "Effects" / "SessionScope" / "Sensor.jsfx") == "edited");
    CHECK(read_file(repo / "hosts" / "reaper" / "Scripts" / "SessionScope" / "lib" / "util.lua") == "lua");

    // ---- copying: real files, marked as ours; an edit in the repository does not show through
    CHECK(reaper_install(repo, resource, true, true).empty());
    CHECK(read_file(effects / "Sensor.jsfx") == "edited");
    CHECK(fs::exists(effects / ".sessionscope-install"));
    write_file(repo / "hosts" / "reaper" / "Effects" / "SessionScope" / "Sensor.jsfx", "edited again");
    CHECK(read_file(effects / "Sensor.jsfx") == "edited");
    CHECK(reaper_install(repo, resource, false, true).empty());      // a copy of ours may be replaced by a link
    CHECK(read_file(effects / "Sensor.jsfx") == "edited again");
    reaper_uninstall(repo, resource);
    CHECK(!fs::exists(effects));

    // ---- a folder someone else made is never replaced or removed
    write_file(effects / "theirs.jsfx", "not ours");
    CHECK(!reaper_install(repo, resource, false, true).empty());     // refuses, and says why
    reaper_uninstall(repo, resource);
    CHECK(read_file(effects / "theirs.jsfx") == "not ours");

    // ---- reaper.ini: a new file gets REAPER's section and ours
    const fs::path work = scratch / "work";
    fs::create_directories(work);
    write_reaper_audio_config(work, 4);
    std::string ini = read_file(work / "reaper.ini");
    CHECK(contains(ini, "[reaper]\n"));
    CHECK(contains(ini, "[audioconfig]\n"));
    CHECK(contains(ini, "mode=4\n"));
    CHECK(contains(ini, "dummy_srate=48000\n"));
    CHECK(contains(ini, "dummy_blocksize=256\n"));

    // ---- writing again changes the value in place, without doubling any line
    write_reaper_audio_config(work, 5);
    ini = read_file(work / "reaper.ini");
    CHECK(contains(ini, "mode=5\n"));
    CHECK(count(ini, "mode=") == 1);
    CHECK(count(ini, "[audioconfig]") == 1);
    CHECK(count(ini, "dummy_srate=") == 1);

    // ---- REAPER's own lines are kept; our section in the middle gets its missing keys before the next section
    write_file(work / "reaper.ini", "[reaper]\r\nlastproject=x\r\n[AudioConfig]\r\nmode=1\r\nasio_driver=Komplete\r\n[midi]\r\nfoo=bar\r\n");
    write_reaper_audio_config(work, 4);
    ini = read_file(work / "reaper.ini");
    CHECK(contains(ini, "lastproject=x\n"));
    CHECK(contains(ini, "asio_driver=Komplete\n"));
    CHECK(contains(ini, "foo=bar\n"));
    CHECK(contains(ini, "mode=4\n"));
    CHECK(!contains(ini, "mode=1\n"));
    CHECK(ini.find("dummy_srate=48000") < ini.find("[midi]"));       // added inside our section, not at the end
    CHECK(count(ini, "[audioconfig]") + count(ini, "[AudioConfig]") == 1);

    // ---- the plug-in folder goes into REAPER's own section, next to what is there
    write_reaper_plugin_folder(work, "C:/somewhere/build/native");
    ini = read_file(work / "reaper.ini");
    CHECK(contains(ini, "vstpath64=C:/somewhere/build/native\n"));
    CHECK(ini.find("vstpath64=") < ini.find("[AudioConfig]"));       // inside [reaper], which comes first
    CHECK(contains(ini, "lastproject=x\n"));
    write_reaper_plugin_folder(work, "D:/other");
    ini = read_file(work / "reaper.ini");
    CHECK(contains(ini, "vstpath64=D:/other\n"));
    CHECK(count(ini, "vstpath64=") == 1);
    CHECK(count(ini, "mode=4") == 1);                                // the audio settings are untouched

    // ---- the summary of a result.json
    const char *json = R"json({
      "host": {"version": "7.83/win64"},
      "device": {"mode": "Dummy Audio", "srate": "48000", "bsize": "256", "audio_running": true},
      "host_extension": "0.1",
      "steps": [
        {"step": "build fixtures", "built": 8, "problems": ["F9: no such plug-in"]},
        {"step": "F4 inventory", "container_children": 2, "capabilities": {"pdc": true, "containers": true, "cpu": false},
         "fixed_latency": [
           {"where": "Vox", "slider": 512, "pdc": 512, "enabled": true, "offline": false},
           {"where": "Vox > container", "slider": 256, "pdc": 0, "enabled": false, "offline": false}]},
        {"step": "F4 bypass", "pdc": {"enabled": 512, "just_bypassed": 512, "bypassed_after_1_5_s": 0}},
        {"step": "identity", "passed": false, "checks": {"b_keeps": true, "a_breaks": false}},
        {"step": "probe", "passed": true, "checks": {"reports_blocks": true}},
        {"step": "F5 skipped", "reason": "audio device is not the dummy device"}
      ],
      "traces": {"F7b": {"folder": "does-not-exist", "summary": {"sensor_records": 9000, "dropped_records": 0,
                 "audio_xruns": 2, "sensor_connected": true, "second_sensor": false}}},
      "errors": ["watchdog: too slow"]
    })json";
    const std::string summary = validation_summary(parse_json(json).value_or(JsonValue()));
    CHECK(contains(summary, "REAPER 7.83/win64, audio: Dummy Audio 48000 Hz / 256 samples, running: true, extension: 0.1"));
    CHECK(contains(summary, "Fixtures built: 8"));
    CHECK(contains(summary, "! F9: no such plug-in"));
    CHECK(contains(summary, "active   slider    512  reported    512  match"));
    CHECK(contains(summary, "bypassed slider    256  reported      0  differs"));
    CHECK(contains(summary, "container children seen: 2"));
    CHECK(contains(summary, "capabilities: containers, pdc\n"));     // sorted, only the ones that are true
    CHECK(contains(summary, "enabled and running 512, right after bypass 512, 1.5 s later 0"));
    CHECK(contains(summary, "Spike A, identity stability: FAILED"));
    CHECK(summary.find("FAIL a_breaks") < summary.find("ok   b_keeps"));   // checks in name order
    CHECK(contains(summary, "Probe (Task Manager): passed"));
    CHECK(contains(summary, "F5 skipped: audio device is not the dummy device"));
    CHECK(contains(summary, "Trace F7b: 9000 sensor records, 0 dropped, 2 xruns, sensor seen, second sensor not reported, 0 REAPER blocks"));
    CHECK(contains(summary, "ERROR watchdog: too slow"));

    // ---- an empty or broken result gives a short summary, not a crash
    CHECK(contains(validation_summary(JsonValue()), "Fixtures built: 0"));

    std::error_code error;
    fs::remove_all(scratch, error);
    return finish_tests();
}
