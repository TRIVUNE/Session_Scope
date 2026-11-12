// =================================================================================================
// hosted_vst3.h - the plug-in inside the Probe: loading it, and timing every audio block
// =================================================================================================
//
// The SessionScope Probe (sessionscope_probe.cpp) is a plug-in that holds another plug-in, the
// "hosted effect", and passes everything through to it. This class is the part that
//   1. loads the hosted effect from its .vst3 file and sets it up, the way a DAW would, and
//   2. times each call into its process() and writes the result into the probes' shared table
//      (../sensor_core/probe_table.h), which the HUD's Task Manager reads.
//
// It knows nothing about being inside a plug-in: the tests (test_hosted_vst3.cpp) use it directly
// as a tiny host. See handoff/task-manager-plan.md for the plan.
//
// Loading uses Steinberg's own hosting helpers from the VST3 SDK (public.sdk/source/vst/hosting):
//   Module          opens a .vst3 file and gives its "factory", the list of plug-ins inside
//   PlugProvider    creates one plug-in from the factory: its "component" (the audio part) and its
//                   "edit controller" (the parameter and window part), and connects the two
//   HostApplication the object a plug-in sees as its host
#pragma once

#include "../sensor_core/probe_table.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sessionscope {

class HostedVst3 {
public:
    // `table_name`: the shared table to report into; tests use their own so they never mix with
    // real probes.
    explicit HostedVst3(const char* table_name = probes::TABLE_NAME);
    ~HostedVst3();

    // Loads the effect with the id `class_id` from the .vst3 at `path` and initializes it. Call on
    // the main (user interface) thread, before any audio. False, with a reason in `error`, if the
    // file can't be opened, the effect isn't in it, or it won't start. One effect per object.
    bool load(const std::string& path, const VST3::UID& class_id, std::string& error);

    // The same, but picks the plug-in by its name; an empty name takes the first plug-in in the
    // file. Instruments are allowed here (the bench measures them; the Probe uses load() and
    // checks for instruments itself).
    bool load_by_name(const std::string& path, const std::string& name, std::string& error);

    // What the factory said about the loaded plug-in: its name, its sub-categories ("Fx|Reverb",
    // "Instrument|Synth"), its vendor and its version. Empty before load().
    const std::string& name() const { return plugin_name_; }
    const std::string& sub_categories() const { return sub_categories_; }
    const std::string& vendor() const { return vendor_; }
    const std::string& version() const { return version_; }
    const VST3::UID& class_id() const { return class_id_; }   // a saved preset names the plug-in by this

    // The track the Probe sits on, as the DAW told us (name, and position counted from 0; -1 if
    // unknown). The HUD groups plug-ins by this name. Main thread; may be called before load().
    void set_track(const std::string& name, std::int64_t order);

    // The hosted effect's parts, or nullptr before load(): the Probe forwards the DAW's calls to them.
    Steinberg::Vst::IComponent* component() const;
    Steinberg::Vst::IEditController* controller() const;
    Steinberg::Vst::IAudioProcessor* processor() const { return processor_.get(); }

    // Passes the DAW's audio setup (sample rate, block size, 32 or 64 bit) on to the effect and
    // remembers the sample rate, which process() needs to know how long a block lasts.
    Steinberg::tresult setup(Steinberg::Vst::ProcessSetup& setup);

    // Makes the effect's main input and main output use these channel layouts (stereo, mono...)
    // and switches its other audio buses (a sidechain input, extra outputs) off. True if the
    // effect now really has these layouts: the Probe uses this to start an effect in the middle
    // of a session, with the layout the DAW is already sending. Main thread, effect not active.
    bool use_layout(Steinberg::Vst::SpeakerArrangement input, Steinberg::Vst::SpeakerArrangement output);

    // One audio block: calls the effect's process() and times it. If the DAW sends fewer audio
    // buses than the effect has (it doesn't know yet about the effect's sidechain), the missing
    // ones are handed to the effect as empty buses. On the audio thread: no locks, no memory
    // allocation (see the rules in probe_table.h).
    Steinberg::tresult process(Steinberg::Vst::ProcessData& data);

private:
    class Provider;                                   // PlugProvider with setupPlugin() made callable
    // Both load()s: opens the file and loads the first audio plug-in in it that `wanted` accepts.
    bool load_matching(const std::string& path, std::string& error,
                       const std::function<bool(const VST3::Hosting::ClassInfo&)>& wanted);
    void publish_names();                             // writes track and plug-in name into the table
    void count_buses();                               // sizes the two padded lists below to the effect's audio buses

    Steinberg::Vst::HostApplication host_;            // what the effect sees as its host
    VST3::Hosting::Module::Ptr module_;               // the opened .vst3 file; must outlive the effect
    std::unique_ptr<Provider> provider_;              // owns the effect's component and controller
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;   // the component's audio interface
    probes::Writer writer_;                           // this probe's slot in the shared table
    std::string plugin_name_;                         // the effect's name, for the table
    std::string sub_categories_, vendor_, version_;   // from the factory, for the bench's report
    VST3::UID class_id_;                              // the loaded plug-in's id
    std::string track_name_;                          // the track's name, empty until the DAW says
    std::int64_t track_order_ = -1;                   // the track's position, -1 until the DAW says
    double sample_rate_ = 0;                          // 0 until setup()
    // One entry per audio bus of the effect, filled in process() only when the DAW sent fewer
    // buses than that. Sized on the main thread (count_buses), so process() never allocates.
    std::vector<Steinberg::Vst::AudioBusBuffers> padded_inputs_, padded_outputs_;
};

}  // namespace sessionscope
