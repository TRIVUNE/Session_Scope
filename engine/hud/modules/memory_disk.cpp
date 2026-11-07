// =================================================================================================
// memory_disk.cpp - module "memory": the DAW's memory, the computer's free memory, disk reads
// =================================================================================================
//
// Why it matters:
//   - Big sample libraries live in memory. When free memory runs out, Windows starts moving memory
//     to disk ("paging"), which is slow and causes dropouts. Free RAM turns red under 10 %.
//   - Libraries that stream samples from disk need the disk to keep up; "disk reads" shows how fast
//     the DAW is reading right now.
//
// Where the numbers come from: ProcessResources and system_memory() (engine/src/process_resources.h),
// which ask Windows (GetProcessMemoryInfo, GetProcessIoCounters, GlobalMemoryStatusEx). These are
// read once a second (SystemMonitor), not ten times: they change slowly.

#include <string>

#include "modules/draw_helpers.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

class MemoryDisk final : public Card {
public:
    using Card::Card;
    const char *id() const override { return "memory"; }
    const char *title() const override { return "Memory and disk"; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.system.samples();
        if (samples.empty()) {
            draw_card(draw, box, frame.scale, "DAW memory", DASH, colour::TEXT_QUIET, {});
            return;
        }
        const SystemSample &now = samples.back();

        // Free RAM, "10.0 GB of 25.1 GB"; red when under a tenth is left.
        std::string free = DASH;
        ImU32 free_colour = colour::TEXT;
        if (now.memory.available_bytes && now.memory.total_bytes) {
            free = bytes(now.memory.available_bytes) + " of " + bytes(now.memory.total_bytes);
            if (*now.memory.available_bytes < *now.memory.total_bytes * 0.1) free_colour = colour::WARN;
        }
        const std::string reads = now.daw_read_bytes_per_s ? bytes(now.daw_read_bytes_per_s) + "/s" : DASH;

        draw_card(draw, box, frame.scale, "DAW memory", bytes(now.daw_memory_bytes),
                  now.daw_memory_bytes ? colour::TEXT : colour::TEXT_QUIET,
                  {{"free RAM", free, free_colour}, {"disk reads", reads}});
    }

protected:
    int lines() const override { return 2; }
};

}  // namespace

std::unique_ptr<Module> make_memory_disk(const Fonts &fonts) { return std::make_unique<MemoryDisk>(fonts); }

}  // namespace sessionscope
