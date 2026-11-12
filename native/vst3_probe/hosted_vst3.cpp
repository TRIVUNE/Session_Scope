// hosted_vst3.cpp - see hosted_vst3.h: loads a VST3 effect and times its process() call.
#include "hosted_vst3.h"

#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include <algorithm>
#include <cmath>

#ifndef _WIN32
#include <unistd.h>   // getpid
#endif

namespace sessionscope {

using namespace Steinberg;
using namespace Steinberg::Vst;
using VST3::Hosting::Module;

// PlugProvider keeps setupPlugin() protected (it normally runs when first asked for the
// component). We call it ourselves, so a plug-in that won't start is found out in load().
class HostedVst3::Provider : public PlugProvider {
public:
    using PlugProvider::PlugProvider;   // the same constructors
    using PlugProvider::setupPlugin;    // made public
};

// The writer takes this object's address as part of its id, so two probes never share one.
HostedVst3::HostedVst3(const char* table_name) : writer_(this, table_name) {}
HostedVst3::~HostedVst3() = default;   // members go in reverse order: the effect before its module

bool HostedVst3::load(const std::string& path, const VST3::UID& class_id, std::string& error) {
    return load_matching(path, error, [&](const VST3::Hosting::ClassInfo& info) { return info.ID() == class_id; });
}

bool HostedVst3::load_by_name(const std::string& path, const std::string& name, std::string& error) {
    return load_matching(path, error,
                         [&](const VST3::Hosting::ClassInfo& info) { return name.empty() || info.name() == name; });
}

bool HostedVst3::load_matching(const std::string& path, std::string& error,
                               const std::function<bool(const VST3::Hosting::ClassInfo&)>& wanted) {
    if (module_) {   // one effect per object: swapping would need the DAW to stop audio first
        error = "This Probe already has a hosted effect.";
        return false;
    }

    error.clear();
    auto module = Module::create(path, error);   // opens the .vst3 (loads its DLL); fills `error` on failure
    if (!module) return false;

    // Find the wanted plug-in among the ones the file holds. It must be an audio processor
    // (an effect or an instrument: both are "Audio Module Class"): a .vst3 also lists its
    // controller classes, which can't process audio.
    const auto& factory = module->getFactory();
    const auto classes = factory.classInfos();
    const auto selected = std::find_if(classes.begin(), classes.end(), [&](const auto& info) {
        return info.category() == kVstAudioEffectClass && wanted(info);
    });
    if (selected == classes.end()) {
        error = "The selected VST3 audio effect was not found in this module.";
        return false;
    }
    sub_categories_ = selected->subCategoriesString();
    vendor_ = selected->vendor();
    version_ = selected->version();
    class_id_ = selected->ID();

    factory.setHostContext(&host_);   // the factory may ask who its host is
    auto provider = std::make_unique<Provider>(factory, *selected);
    if (!provider->setupPlugin(&host_)) {   // creates and initializes component and controller, connects them
        error = "The selected VST3 effect could not be initialized.";
        return false;
    }

    // The component must also be an audio processor (every effect is; this guards against odd ones).
    FUnknownPtr<IAudioProcessor> processor(provider->getComponentPtr().get());
    if (!processor) {
        error = "The selected VST3 effect has no audio processor.";
        return false;
    }

    // Everything worked: keep it. From here on the effect lives as long as this object.
    module_ = std::move(module);
    provider_ = std::move(provider);
    processor_ = processor;
    plugin_name_ = selected->name();
    count_buses();
    if (writer_.claim()) publish_names();   // take a slot in the shared table (if it is full, we just don't report)
    return true;
}

void HostedVst3::set_track(const std::string& name, std::int64_t order) {
    track_name_ = name;
    track_order_ = order;
    publish_names();   // does nothing before load(): there is no slot yet
}

void HostedVst3::publish_names() {
    if (!writer_.active()) return;
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();   // the DAW's process: the HUD shows only its own DAW's probes
#else
    const auto process_id = getpid();
#endif
    // A DAW that doesn't tell plug-ins their track (Ableton Live) leaves the name empty; the HUD
    // then lists the plug-in under "Unknown track".
    writer_.set_names(track_name_.empty() ? "Unknown track" : track_name_.c_str(), plugin_name_.c_str(),
                      track_order_, process_id);
}

IComponent* HostedVst3::component() const {
    return provider_ ? provider_->getComponentPtr().get() : nullptr;
}

IEditController* HostedVst3::controller() const {
    return provider_ ? provider_->getControllerPtr().get() : nullptr;
}

tresult HostedVst3::setup(ProcessSetup& setup) {
    // A sample rate of 0, or not a number, would make every block's length a division by zero.
    if (!processor_ || !std::isfinite(setup.sampleRate) || setup.sampleRate <= 0)
        return kInvalidArgument;

    const tresult result = processor_->setupProcessing(setup);
    if (result == kResultOk) sample_rate_ = setup.sampleRate;   // only once the effect agreed to it
    count_buses();   // the DAW has settled the buses by now
    return result;
}

void HostedVst3::count_buses() {
    auto* effect = component();
    if (!effect) return;
    const auto inputs = static_cast<std::size_t>(std::max<int32>(0, effect->getBusCount(kAudio, kInput)));
    const auto outputs = static_cast<std::size_t>(std::max<int32>(0, effect->getBusCount(kAudio, kOutput)));
    padded_inputs_.assign(inputs, AudioBusBuffers{});
    padded_outputs_.assign(outputs, AudioBusBuffers{});
}

bool HostedVst3::use_layout(SpeakerArrangement input, SpeakerArrangement output) {
    auto* effect = component();
    if (!effect || !processor_) return false;
    const int32 inputs = effect->getBusCount(kAudio, kInput), outputs = effect->getBusCount(kAudio, kOutput);
    if (inputs < 1 || outputs < 1) return false;   // nothing to connect the DAW's audio to

    // setBusArrangements wants a layout for every bus: ours for the main ones (always bus 0), and
    // for the others whatever they have now.
    std::vector<SpeakerArrangement> in(static_cast<std::size_t>(inputs)), out(static_cast<std::size_t>(outputs));
    for (int32 i = 0; i < inputs; ++i) processor_->getBusArrangement(kInput, i, in[static_cast<std::size_t>(i)]);
    for (int32 i = 0; i < outputs; ++i) processor_->getBusArrangement(kOutput, i, out[static_cast<std::size_t>(i)]);
    in[0] = input;
    out[0] = output;
    processor_->setBusArrangements(in.data(), inputs, out.data(), outputs);   // its answer is checked below, by asking
    for (int32 i = 0; i < inputs; ++i) effect->activateBus(kAudio, kInput, i, i == 0);     // only the main buses on
    for (int32 i = 0; i < outputs; ++i) effect->activateBus(kAudio, kOutput, i, i == 0);
    count_buses();

    // Some effects refuse a layout, or pick a near one (mono in, stereo out). Only an exact match counts.
    SpeakerArrangement got_in = 0, got_out = 0;
    return processor_->getBusArrangement(kInput, 0, got_in) == kResultOk && got_in == input &&
           processor_->getBusArrangement(kOutput, 0, got_out) == kResultOk && got_out == output;
}

tresult HostedVst3::process(ProcessData& data) {
    if (!processor_ || sample_rate_ <= 0) return kNotInitialized;
    // The effect must get one entry for every audio bus it has. A DAW that set us up before the
    // effect was picked may send fewer (it doesn't know about a sidechain input yet). Then the
    // effect gets a copy of the block's description with the missing buses added as empty ones
    // (no channels). The copy holds the same pointers: the audio itself is not copied.
    const auto host_inputs = static_cast<std::size_t>(std::max<int32>(0, data.numInputs));
    const auto host_outputs = static_cast<std::size_t>(std::max<int32>(0, data.numOutputs));
    const bool pad = host_inputs < padded_inputs_.size() || host_outputs < padded_outputs_.size();
    ProcessData padded;
    if (pad) {
        padded = data;
        if (host_inputs < padded_inputs_.size()) {
            for (std::size_t i = 0; i < padded_inputs_.size(); ++i)
                padded_inputs_[i] = i < host_inputs ? data.inputs[i] : AudioBusBuffers{};
            padded.inputs = padded_inputs_.data();
            padded.numInputs = static_cast<int32>(padded_inputs_.size());
        }
        if (host_outputs < padded_outputs_.size()) {
            for (std::size_t i = 0; i < padded_outputs_.size(); ++i)
                padded_outputs_[i] = i < host_outputs ? data.outputs[i] : AudioBusBuffers{};
            padded.outputs = padded_outputs_.data();
            padded.numOutputs = static_cast<int32>(padded_outputs_.size());
        }
    }
    ProcessData& block = pad ? padded : data;

    // A call with no samples only delivers parameter changes; it is not an audio block, so it is
    // passed on but not timed.
    if (data.numSamples <= 0) return processor_->process(block);

    const auto start = probes::now_ns();                          // the clock, before
    const tresult result = processor_->process(block);            // the real plug-in does its work
    const auto elapsed = probes::now_ns() - start;                // time spent inside it
    // The effect marks silent outputs in the bus entries; ours were copies, so hand the marks back.
    if (pad && padded.outputs == padded_outputs_.data()) {
        for (std::size_t i = 0; i < host_outputs; ++i) data.outputs[i].silenceFlags = padded_outputs_[i].silenceFlags;
    }
    // How long this block lasts when played: samples / samples per second, in nanoseconds.
    const auto budget = static_cast<std::uint64_t>(data.numSamples * 1e9 / sample_rate_);
    writer_.record_block(elapsed, budget);                        // atomic adds only: safe on the audio thread
    return result;
}

}  // namespace sessionscope
