// Loads a VST3 effect and times its process call without allocating on the audio thread.
#include "hosted_vst3.h"

#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include <algorithm>
#include <cmath>

namespace sessionscope {

using namespace Steinberg;
using namespace Steinberg::Vst;
using VST3::Hosting::Module;

class HostedVst3::Provider : public PlugProvider {
public:
    using PlugProvider::PlugProvider;
    using PlugProvider::setupPlugin;
};

HostedVst3::HostedVst3(const char* table_name) : writer_(this, table_name) {}
HostedVst3::~HostedVst3() = default;

bool HostedVst3::load(const std::string& path, const VST3::UID& class_id, std::string& error) {
    if (module_) {
        error = "This Probe already has a hosted effect.";
        return false;
    }

    error.clear();
    auto module = Module::create(path, error);
    if (!module) return false;

    const auto& factory = module->getFactory();
    const auto classes = factory.classInfos();
    const auto selected = std::find_if(classes.begin(), classes.end(), [&](const auto& info) {
        return info.ID() == class_id && info.category() == kVstAudioEffectClass;
    });
    if (selected == classes.end()) {
        error = "The selected VST3 audio effect was not found in this module.";
        return false;
    }

    factory.setHostContext(&host_);
    auto provider = std::make_unique<Provider>(factory, *selected);
    if (!provider->setupPlugin(&host_)) {
        error = "The selected VST3 effect could not be initialized.";
        return false;
    }

    FUnknownPtr<IAudioProcessor> processor(provider->getComponentPtr().get());
    if (!processor) {
        error = "The selected VST3 effect has no audio processor.";
        return false;
    }

    module_ = std::move(module);
    provider_ = std::move(provider);
    processor_ = processor;
    if (writer_.claim()) {
#ifdef _WIN32
        const auto process_id = GetCurrentProcessId();
#else
        const auto process_id = getpid();
#endif
        writer_.set_names("Unknown track", selected->name().c_str(), -1, process_id);
    }
    return true;
}

IComponent* HostedVst3::component() const {
    return provider_ ? provider_->getComponentPtr().get() : nullptr;
}

IEditController* HostedVst3::controller() const {
    return provider_ ? provider_->getControllerPtr().get() : nullptr;
}

tresult HostedVst3::setup(ProcessSetup& setup) {
    if (!processor_ || !std::isfinite(setup.sampleRate) || setup.sampleRate <= 0)
        return kInvalidArgument;

    const tresult result = processor_->setupProcessing(setup);
    if (result == kResultOk) sample_rate_ = setup.sampleRate;
    return result;
}

tresult HostedVst3::process(ProcessData& data) {
    if (!processor_ || sample_rate_ <= 0) return kNotInitialized;
    if (data.numSamples <= 0) return processor_->process(data);  // Parameter flushes still go through.

    const auto start = probes::now_ns();
    const tresult result = processor_->process(data);
    const auto elapsed = probes::now_ns() - start;
    const auto budget = static_cast<std::uint64_t>(data.numSamples * 1e9 / sample_rate_);
    writer_.record_block(elapsed, budget);
    return result;
}

}  // namespace sessionscope
