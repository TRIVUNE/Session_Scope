// The VST3 Probe's hosted effect and audio-block timing.
// Uses Steinberg's VST3 hosting helpers; see handoff/task-manager-plan.md.
#pragma once

#include "../sensor_core/probe_table.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"

namespace sessionscope {

class HostedVst3 {
public:
    explicit HostedVst3(const char* table_name = probes::TABLE_NAME);
    ~HostedVst3();

    // Create and initialize the selected effect on the control thread, before processing.
    bool load(const std::string& path, const VST3::UID& class_id, std::string& error);
    Steinberg::Vst::IComponent* component() const;
    Steinberg::Vst::IEditController* controller() const;
    Steinberg::Vst::IAudioProcessor* processor() const { return processor_.get(); }
    Steinberg::tresult setup(Steinberg::Vst::ProcessSetup& setup);
    Steinberg::tresult process(Steinberg::Vst::ProcessData& data);

private:
    class Provider;
    Steinberg::Vst::HostApplication host_;
    VST3::Hosting::Module::Ptr module_;
    std::unique_ptr<Provider> provider_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    probes::Writer writer_;
    double sample_rate_ = 0;
};

}  // namespace sessionscope
