// The REAPER recorder script (hosts/reaper/Scripts/SessionScope/lib/recorder.lua) and the engine's
// trace reader (engine/src/trace.h) must name the same columns in the same order. This test reads
// the Lua file and fails if the two lists drift apart.

#include "trace.h"
#include "test_support.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sessionscope;

namespace {

std::string read_whole_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// The quoted names in `M.<name> = { 'a', 'b', ... }`, in order. Empty if the list is not there.
std::vector<std::string> lua_list(const std::string &source, const std::string &name) {
    std::vector<std::string> names;
    const std::size_t start = source.find("M." + name + " = {");
    if (start == std::string::npos) return names;
    const std::size_t end = source.find('}', start);
    if (end == std::string::npos) return names;

    std::size_t quote = source.find('\'', start);
    while (quote != std::string::npos && quote < end) {
        const std::size_t close = source.find('\'', quote + 1);
        if (close == std::string::npos || close > end) break;
        names.push_back(source.substr(quote + 1, close - quote - 1));
        quote = source.find('\'', close + 1);
    }
    return names;
}

void check_same(const std::string &source, const char *name, const std::vector<std::string> &engine) {
    const std::vector<std::string> lua = lua_list(source, name);
    CHECK(!lua.empty());
    CHECK(lua == engine);
    if (lua != engine) std::fprintf(stderr, "  which list: %s\n", name);
}

}  // namespace

int main() {
    const auto repo = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const std::string source = read_whole_file(repo / "hosts" / "reaper" / "Scripts" / "SessionScope" / "lib" / "recorder.lua");
    CHECK(!source.empty());

    check_same(source, "SENSOR_COLUMNS", SENSOR_COLUMNS);
    check_same(source, "TRANSPORT_COLUMNS", TRANSPORT_COLUMNS);
    check_same(source, "EVENT_COLUMNS", EVENT_COLUMNS);

    // The check itself must be able to fail.
    CHECK(lua_list("M.X = { 'a', 'b' }", "X") == std::vector<std::string>({"a", "b"}));
    CHECK(lua_list(source, "NO_SUCH_LIST").empty());

    return finish_tests();
}
