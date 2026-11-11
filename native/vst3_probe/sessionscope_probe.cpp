// =================================================================================================
// sessionscope_probe.cpp - the SessionScope Probe: a VST3 plug-in that measures another plug-in
// =================================================================================================
//
// A plug-in can't see what other plug-ins cost. So the Probe goes where the plug-in would go, loads
// that plug-in inside itself (the "hosted effect"), and passes everything through to it:
//
//   the DAW asks the Probe ...               the Probe answers ...
//   which audio buses do you have?           whatever the hosted effect has
//   which parameters do you have?            the hosted effect's parameters, with the same ids
//   show your window                         the hosted effect's own window
//   give me your state to save               which effect is inside + that effect's state
//   what is your latency / tail?             the hosted effect's
//   process this audio block                 the hosted effect processes it, and we time it
//
// The timing goes into the probes' shared table (../sensor_core/probe_table.h), which the HUD's
// Task Manager reads. hosted_vst3.h does the loading and the timing; this file is the VST3 side:
// the plug-in the DAW sees, and the small window for picking the effect to load.
//
// Before an effect is picked, the Probe is a plain stereo pass-through with a picker window.
//
// Built on Steinberg's VST3 SDK (MIT): SingleComponentEffect is its base class for a plug-in whose
// audio part and parameter part are one object. The interfaces are documented at
// steinbergmedia.github.io/vst3_dev_portal. The window uses plain Win32 controls (a list box and a
// button: learn.microsoft.com, "List Box Controls"), so this file is Windows-only for now.
//
// Finding out what a DAW does: set the environment variable SESSIONSCOPE_PROBE_LOG to a file path
// before starting the DAW, and the Probe writes one line per call it gets from the DAW there
// (never from the audio thread). Plan and risks: handoff/task-manager-plan.md.

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/commonstringconvert.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstchannelcontextinfo.h"
#include "hosted_vst3.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;
using sessionscope::HostedVst3;
using VST3::Hosting::Module;

// The Probe's own id. Never to change: DAWs remember plug-ins in saved projects by this number.
const FUID ProbeUID(0x8B2E807B, 0xC9014C31, 0xA9788B53, 0xB4EA30F2);
constexpr uint32 StateVersion = 1;                     // the layout of our saved state, below
constexpr uint32 MaxStateBytes = 64 * 1024 * 1024;     // refuse absurd sizes from a damaged project file

// Which effect is inside: the .vst3 it comes from, its id in that file, and its name.
struct EffectChoice {
    std::string path;
    VST3::UID id;
    std::string name;
};

// ---- the diagnostic log ---------------------------------------------------------------------------

// Appends one line to the file named by SESSIONSCOPE_PROBE_LOG; does nothing when that isn't set.
// Main-thread calls only: opening and writing a file must never happen on the audio thread.
__attribute__((format(printf, 1, 2)))
void log_line(const char* format, ...) {
    static FILE* file = [] {                           // opened once, the first time something is logged
        const char* path = std::getenv("SESSIONSCOPE_PROBE_LOG");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    if (!file) return;
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(file, format, arguments);
    va_end(arguments);
    std::fputc('\n', file);
    std::fflush(file);                                 // so the line is there even if the DAW crashes next
}

// ---- reading and writing the saved state ---------------------------------------------------------
//
// The Probe's state, as the DAW stores it in the project:
//   4 bytes   version (1)
//   blob      the hosted effect's .vst3 path (empty: no effect picked yet)
//   blob      its class id, 32 hex characters
//   blob      the effect's own component state (what its getState wrote)
//   blob      the effect's own controller ("editor") state
// A blob is 4 bytes of length, then that many bytes. Numbers are little endian (low byte first)
// on every system, so a project saved on one computer opens on another.

// Reads or writes exactly `size` bytes. False if the stream gave or took fewer.
bool transfer(IBStream* stream, void* data, uint32 size, bool write) {
    int32 count = 0;
    const auto result = write ? stream->write(data, size, &count) : stream->read(data, size, &count);
    return result == kResultOk && count == static_cast<int32>(size);
}

// Reads or writes one 32-bit number, low byte first.
bool transfer_number(IBStream* stream, uint32& value, bool write) {
    unsigned char bytes[4]{};
    if (write)
        for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<unsigned char>(value >> (8 * i));
    if (!transfer(stream, bytes, sizeof bytes, write)) return false;
    if (!write) {
        value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32>(bytes[i]) << (8 * i);
    }
    return true;
}

// Reads or writes one blob. `limit`: the largest size accepted (a damaged file can claim anything).
bool transfer_blob(IBStream* stream, std::string& blob, bool write, uint32 limit) {
    if (write && blob.size() > limit) return false;
    uint32 size = write ? static_cast<uint32>(blob.size()) : 0;
    if (!transfer_number(stream, size, write) || size > limit) return false;
    if (!write) blob.resize(size);
    return size == 0 || transfer(stream, blob.data(), size, write);
}

// What a getState call wrote into `stream`, as bytes; empty if the call failed.
std::string snapshot(tresult result, MemoryStream& stream) {
    if (result != kResultOk || stream.getSize() == 0) return {};
    return {stream.getData(), static_cast<std::size_t>(stream.getSize())};
}

// Copies input to output when no effect is loaded. The DAW may hand us the same buffer for both
// ("in place"), or no buffers at all; then there is nothing to copy.
template <typename Sample>
void copy_audio(Sample** source, Sample** destination, int32 channels, int32 frames) {
    if (!source || !destination) return;
    for (int32 channel = 0; channel < channels; ++channel) {
        if (source[channel] && destination[channel] && source[channel] != destination[channel])
            std::memcpy(destination[channel], source[channel], sizeof(Sample) * frames);
    }
}

// ---- the plug-in ---------------------------------------------------------------------------------

// Besides the usual plug-in interfaces (from SingleComponentEffect), the Probe answers two more:
//   IMidiMapping    which parameter a MIDI controller moves: asked of the hosted effect
//   IInfoListener   the DAW tells the plug-in about its track (name, position): we keep the name
//                   for the HUD, and pass the call on
class Probe final : public SingleComponentEffect, public IMidiMapping, public ChannelContext::IInfoListener {
public:
    // What the factory at the end of this file calls to make a Probe.
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new Probe); }

    // "Do you support interface X?": yes for our two extras, otherwise whatever the base class supports.
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IMidiMapping::iid)) {
            *obj = static_cast<IMidiMapping*>(this);
            addRef();
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(iid, ChannelContext::IInfoListener::iid)) {
            *obj = static_cast<ChannelContext::IInfoListener*>(this);
            addRef();
            return kResultOk;
        }
        return SingleComponentEffect::queryInterface(iid, obj);
    }
    REFCOUNT_METHODS(SingleComponentEffect)

    // Called once after creation. The empty Probe has one stereo input and one stereo output.
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const auto result = SingleComponentEffect::initialize(context);
        if (result == kResultOk) {
            addAudioInput(STR16("Audio In"), SpeakerArr::kStereo);
            addAudioOutput(STR16("Audio Out"), SpeakerArr::kStereo);
        }
        log_line("%p initialize -> %d", static_cast<void*>(this), static_cast<int>(result));
        return result;
    }

    // Called once before the Probe is destroyed: let go of the hosted effect first.
    tresult PLUGIN_API terminate() override {
        log_line("%p terminate", static_cast<void*>(this));
        if (auto* c = controller()) c->setComponentHandler(nullptr);   // it must not call the DAW any more
        active_effect_.store(nullptr);
        effect_configured_ = false;
        effect_.reset();
        return SingleComponentEffect::terminate();
    }

    // The picker's "Load effect" button. Main thread. False, with the reason, if it can't be loaded.
    bool select(const EffectChoice& choice, std::string& error) {
        if (effect_) { error = "This Probe already has an effect."; return false; }
        auto effect = std::make_unique<HostedVst3>();
        if (!effect->load(choice.path, choice.id, error)) {
            log_line("%p select %s failed: %s", static_cast<void*>(this), choice.path.c_str(), error.c_str());
            return false;
        }
        install(std::move(effect), choice);
        const bool started = start_now();
        log_line("%p select %s (%s), %s", static_cast<void*>(this), choice.path.c_str(), choice.name.c_str(),
                 started ? "started at once" : "waiting for the DAW to restart us");
        // The Probe just became a different plug-in: other buses, parameters, latency. Tell the DAW
        // to ask us about those again; setDirty: the project has unsaved changes.
        // Not kReloadComponent ("unload me and load me again"): REAPER 7.83 answers it by making
        // a fresh Probe without handing over our state, so the effect just picked was gone again
        // and the picker came back (seen in the first test of this window, November 2026).
        if (auto* handler = getComponentHandler()) {
            if (FUnknownPtr<IComponentHandler2> extended{handler}; extended) extended->setDirty(true);
            handler->restartComponent(kIoChanged | kLatencyChanged | kParamTitlesChanged | kParamValuesChanged);
        }
        return true;
    }

    // The DAW wants our window: the hosted effect's own, or the picker (defined after the Picker class).
    IPlugView* PLUGIN_API createView(FIDString name) override;

    // ---- buses and audio format: answered by the hosted effect once there is one -----------------

    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        return effect_ ? effect_->component()->getBusCount(type, dir) : SingleComponentEffect::getBusCount(type, dir);
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& info) override {
        return effect_ ? effect_->component()->getBusInfo(type, dir, index, info)
                       : SingleComponentEffect::getBusInfo(type, dir, index, info);
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo& input, RoutingInfo& output) override {
        return effect_ ? effect_->component()->getRoutingInfo(input, output) : kNotImplemented;
    }
    tresult PLUGIN_API setIoMode(IoMode mode) override {
        return effect_ ? effect_->component()->setIoMode(mode) : kNotImplemented;
    }
    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) override {
        return effect_ ? effect_->component()->activateBus(type, dir, index, state)
                       : SingleComponentEffect::activateBus(type, dir, index, state);
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 ins, SpeakerArrangement* out, int32 outs) override {
        if (effect_) return effect_->processor()->setBusArrangements(in, ins, out, outs);
        // Empty: any layout, as long as input and output are the same (we only copy).
        if (ins != 1 || outs != 1 || in[0] != out[0] || !in[0]) return kResultFalse;
        return SingleComponentEffect::setBusArrangements(in, ins, out, outs);
    }
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
        return effect_ ? effect_->processor()->getBusArrangement(dir, index, arr)
                       : SingleComponentEffect::getBusArrangement(dir, index, arr);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        if (effect_) return effect_->processor()->canProcessSampleSize(size);
        // Some DAWs choose 32 or 64 bit before they restore the hosted effect, which may be 32-bit
        // only. So the empty Probe says 32-bit only: every effect can do that.
        return size == kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return effect_ ? effect_->processor()->getLatencySamples() : 0; }
    uint32 PLUGIN_API getTailSamples() override { return effect_ ? effect_->processor()->getTailSamples() : kNoTail; }
    uint32 PLUGIN_API getProcessContextRequirements() override {   // which transport facts the effect needs (tempo, bar...)
        if (!effect_) return 0;
        FUnknownPtr<IProcessContextRequirements> requirements(effect_->processor());
        return requirements ? requirements->getProcessContextRequirements() : 0;
    }

    // ---- starting and stopping audio -------------------------------------------------------------
    //
    // The DAW's order: setupProcessing (sample rate, block size) -> setActive(true) ->
    // setProcessing(true) -> process, process, ... -> setProcessing(false) -> setActive(false).
    //
    // The audio thread only ever uses active_effect_: the effect that has been fully set up and
    // switched on. It is set in setActive, when the DAW starts us with an effect inside. An
    // effect that arrives while the DAW already has us running (picked in the window, or handed
    // over as saved state) is started by start_now() below, because not every DAW restarts a
    // plug-in when asked to (REAPER 7 doesn't).

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        const auto base = SingleComponentEffect::setupProcessing(setup);   // keeps a copy in processSetup
        if (!effect_ || base != kResultOk) return base;
        const auto result = effect_->setup(setup);
        effect_configured_ = result == kResultOk;
        log_line("%p setupProcessing %.0f Hz, max %d samples, %d bit -> %d", static_cast<void*>(this), setup.sampleRate,
                 static_cast<int>(setup.maxSamplesPerBlock), setup.symbolicSampleSize == kSample64 ? 64 : 32, static_cast<int>(result));
        return result;
    }
    tresult PLUGIN_API setActive(TBool active) override {
        // Switching on: the effect we hold now. Switching off: the one that is switched on.
        auto* running = active_effect_.load();
        auto* hosted = active ? effect_.get() : running;
        log_line("%p setActive(%d), effect %s", static_cast<void*>(this), active ? 1 : 0, hosted ? "present" : "none");
        host_active_ = active != 0;
        if (active && hosted == running) return kResultOk;   // start_now() already switched it on (or there is none)
        if (active && hosted && !effect_configured_) {       // loaded after the DAW's setupProcessing: pass that setup on now
            const auto result = hosted->setup(processSetup);
            if (result != kResultOk) return result;
            effect_configured_ = true;
        }
        if (hosted) {
            const auto result = hosted->component()->setActive(active);
            if (result != kResultOk) return result;
        }
        active_effect_.store(active ? hosted : nullptr);
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        host_processing_ = state != 0;
        auto* running = active_effect_.load();
        return running ? running->processor()->setProcessing(state) : kResultOk;
    }

    // One audio block. Audio thread: nothing here may wait, allocate or log.
    tresult PLUGIN_API process(ProcessData& data) override {
        // The effect does the work; HostedVst3 times it. Reading the pointer once per block is
        // what makes start_now() safe: a block uses either no effect or a fully started one.
        if (auto* running = active_effect_.load(std::memory_order_acquire)) return running->process(data);
        // No effect: copy input to output.
        if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
        const auto& in = data.inputs[0];
        auto& out = data.outputs[0];
        const auto channels = std::min(in.numChannels, out.numChannels);
        if (data.symbolicSampleSize == kSample64)
            copy_audio(in.channelBuffers64, out.channelBuffers64, channels, data.numSamples);
        else
            copy_audio(in.channelBuffers32, out.channelBuffers32, channels, data.numSamples);
        out.silenceFlags = in.silenceFlags;   // silent in, silent out
        return kResultOk;
    }

    // ---- parameters: the hosted effect's, passed straight through --------------------------------

    // The "component handler" is the DAW's side of the conversation: a plug-in tells it "the user
    // moved this knob". The hosted effect's controller gets the DAW's handler directly, so its
    // knob moves reach the DAW with the same parameter ids the DAW got from us.
    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override {
        const auto result = SingleComponentEffect::setComponentHandler(handler);
        if (auto* c = controller()) c->setComponentHandler(handler);
        return result;
    }
    int32 PLUGIN_API getParameterCount() override { return controller() ? controller()->getParameterCount() : 0; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        return controller() ? controller()->getParameterInfo(index, info) : kResultFalse;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 text) override {
        return controller() ? controller()->getParamStringByValue(id, value, text) : kResultFalse;
    }
    tresult PLUGIN_API getParamValueByString(ParamID id, TChar* text, ParamValue& value) override {
        return controller() ? controller()->getParamValueByString(id, text, value) : kResultFalse;
    }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue value) override {
        return controller() ? controller()->normalizedParamToPlain(id, value) : value;
    }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue value) override {
        return controller() ? controller()->plainParamToNormalized(id, value) : value;
    }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
        return controller() ? controller()->getParamNormalized(id) : 0;
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        return controller() ? controller()->setParamNormalized(id, value) : kResultFalse;
    }
    tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16 channel, CtrlNumber control, ParamID& id) override {
        FUnknownPtr<IMidiMapping> mapping(controller());
        return mapping ? mapping->getMidiControllerAssignment(bus, channel, control, id) : kResultFalse;
    }

    // ---- the track we sit on ---------------------------------------------------------------------

    // The DAW tells us about our track (REAPER, Cubase and Studio One do; Ableton Live doesn't).
    // `list` holds named values; we want the track's name and its position.
    tresult PLUGIN_API setChannelContextInfos(IAttributeList* list) override {
        if (!list) return kInvalidArgument;
        TChar name[128]{};                                           // UTF-16 text, as VST3 passes it
        if (list->getString(ChannelContext::kChannelNameKey, name, sizeof name) == kResultOk) {
            name[127] = 0;                                           // always ends in a zero
            track_name_ = StringConvert::convert(std::u16string(name));   // to UTF-8, as the table stores it
        }
        int64 index = 0;
        if (list->getInt(ChannelContext::kChannelIndexKey, index) == kResultOk) track_order_ = index;
        log_line("%p track '%s', index %lld", static_cast<void*>(this), track_name_.c_str(), static_cast<long long>(track_order_));
        if (effect_) effect_->set_track(track_name_, track_order_);
        // The hosted effect may want to know its track too (some show the track name in their window).
        if (FUnknownPtr<ChannelContext::IInfoListener> listener(controller()); listener) listener->setChannelContextInfos(list);
        return kResultOk;
    }

    // ---- saving and loading (layout at the top of this file; defined below) ----------------------

    tresult PLUGIN_API getState(IBStream* stream) override;
    tresult PLUGIN_API setState(IBStream* stream) override;

private:
    // The hosted effect's parameter and window part, or nullptr without an effect.
    IEditController* controller() const { return effect_ ? effect_->controller() : nullptr; }

    // Makes `effect` our hosted effect. It is not processing yet: that starts at the DAW's next setActive(true).
    void install(std::unique_ptr<HostedVst3> effect, const EffectChoice& choice) {
        choice_ = choice;
        effect_ = std::move(effect);
        effect_configured_ = false;
        effect_->set_track(track_name_, track_order_);               // the DAW may have named the track already
        if (auto* c = controller()) c->setComponentHandler(getComponentHandler());
    }

    // Starts the effect just installed without waiting for the DAW, if the DAW has us running and
    // the effect can take over as things are: with the audio layout the DAW is already sending
    // (what the empty Probe agreed to) and the DAW's sample rate and block size. True if the
    // effect is now processing. False leaves the Probe passing audio through until the DAW
    // restarts us (setActive), which starts the effect the normal way. Main thread.
    bool start_now() {
        if (!host_active_ || !effect_ || active_effect_.load()) return false;
        SpeakerArrangement input = 0, output = 0;                    // the layout the DAW gave the empty Probe
        if (SingleComponentEffect::getBusArrangement(kInput, 0, input) != kResultOk ||
            SingleComponentEffect::getBusArrangement(kOutput, 0, output) != kResultOk)
            return false;
        if (!effect_->use_layout(input, output)) return false;       // the effect wants another layout: the DAW must sort that out
        if (effect_->setup(processSetup) != kResultOk) return false; // the DAW's sample rate, block size, 32 or 64 bit
        effect_configured_ = true;
        if (effect_->component()->setActive(true) != kResultOk) return false;
        if (host_processing_) effect_->processor()->setProcessing(true);
        active_effect_.store(effect_.get(), std::memory_order_release);   // from its next block on, the audio thread uses the effect
        return true;
    }

    EffectChoice choice_{};                     // which effect is inside (saved with the project)
    std::unique_ptr<HostedVst3> effect_;        // the hosted effect; empty until one is picked or restored
    // The effect the audio thread uses: fully set up and switched on, or nullptr. Written on the
    // main thread (setActive, start_now), read on the audio thread, hence atomic.
    std::atomic<HostedVst3*> active_effect_{nullptr};
    bool effect_configured_ = false;            // the effect has been given the DAW's audio setup
    bool host_active_ = false;                  // the DAW has switched us on (setActive(true))
    bool host_processing_ = false;              // the DAW has announced audio (setProcessing(true))
    std::string track_name_;                    // our track's name, from the DAW (empty: not told)
    int64 track_order_ = -1;                    // our track's position, from the DAW (-1: not told)
};

tresult Probe::getState(IBStream* stream) {
    if (!stream) return kInvalidArgument;
    uint32 version = StateVersion;
    auto path = choice_.path;
    auto id = path.empty() ? std::string{} : choice_.id.toString();
    std::string component, editor;
    if (effect_) {   // ask the hosted effect for its own state, in both of its halves
        MemoryStream state;
        component = snapshot(effect_->component()->getState(&state), state);
        if (auto* c = controller()) {
            // "getEditorState" is the controller's getState: the SDK header we build on renames
            // it, because in a single-object plug-in the audio part's getState has the same name.
            MemoryStream editor_state;
            editor = snapshot(c->getEditorState(&editor_state), editor_state);
        }
    }
    return transfer_number(stream, version, true) && transfer_blob(stream, path, true, 32768) &&
           transfer_blob(stream, id, true, 32) && transfer_blob(stream, component, true, MaxStateBytes) &&
           transfer_blob(stream, editor, true, MaxStateBytes) ? kResultOk : kResultFalse;
}

tresult Probe::setState(IBStream* stream) {
    uint32 version = 0;
    std::string path, id, component, editor;
    if (!stream || !transfer_number(stream, version, false) || version != StateVersion ||
        !transfer_blob(stream, path, false, 32768) || !transfer_blob(stream, id, false, 32) ||
        !transfer_blob(stream, component, false, MaxStateBytes) || !transfer_blob(stream, editor, false, MaxStateBytes))
        return kResultFalse;

    if (path.empty()) return effect_ ? kResultFalse : kResultOk;   // "no effect": fine only if we have none
    const auto uid = VST3::UID::fromString(id);
    if (!uid || *uid == VST3::UID(ProbeUID.toTUID())) return kResultFalse;   // a Probe inside a Probe: refuse

    // Load the effect the state names, unless we already hold exactly that one.
    std::unique_ptr<HostedVst3> pending;
    if (!effect_) {
        pending = std::make_unique<HostedVst3>();
        std::string error;
        if (!pending->load(path, *uid, error)) {
            log_line("%p setState: %s could not be loaded: %s", static_cast<void*>(this), path.c_str(), error.c_str());
            return kResultFalse;
        }
    } else if (choice_.path != path || choice_.id != *uid) {
        return kResultFalse;   // a different effect: can't be swapped while the DAW may be running audio
    }

    // Hand the effect its own state back: the component's first, then (as a DAW would) tell the
    // controller about it, then the controller's own.
    auto& hosted = pending ? *pending : *effect_;
    if (!component.empty()) {
        MemoryStream state(component.data(), component.size());
        if (hosted.component()->setState(&state) != kResultOk) return kResultFalse;
        if (auto* c = hosted.controller()) {
            MemoryStream copy(component.data(), component.size());
            c->setComponentState(&copy);
        }
    }
    if (auto* c = hosted.controller(); c && !editor.empty()) {
        MemoryStream state(editor.data(), editor.size());
        c->setEditorState(&state);   // the controller's setState (renamed by the SDK header, see getState above)
    }
    log_line("%p setState %s (%s), DAW had us %s", static_cast<void*>(this), path.c_str(), pending ? "loaded now" : "already loaded",
             host_active_ ? "active" : "inactive");
    if (pending) {
        const bool was_active = host_active_;
        install(std::move(pending), {path, *uid, {}});
        // A DAW that switched us on before giving us this state is running the empty Probe.
        // Start the effect ourselves if we can, and tell the DAW that buses, latency and
        // parameters changed so it asks again. Not kReloadComponent: the reloaded Probe would get
        // this same state, ask again, and never finish.
        if (was_active) {
            const bool started = start_now();
            log_line("%p setState: %s", static_cast<void*>(this), started ? "started at once" : "waiting for the DAW to restart us");
            if (auto* handler = getComponentHandler())
                handler->restartComponent(kIoChanged | kLatencyChanged | kParamTitlesChanged | kParamValuesChanged);
        }
    }
    return kResultOk;
}

// ---- the picker window ---------------------------------------------------------------------------

// UTF-8 (what the SDK's hosting code uses) to UTF-16 (what Windows wants).
std::wstring wide(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);   // first call: how long?
    if (!length) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
    result.pop_back();   // the zero at the end was counted
    return result;
}

// A .vst3's file name without folder and ending: "C:\...\VST3\FabFilter\Pro-Q 3.vst3" -> "Pro-Q 3".
std::string module_title(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (const auto dot = name.rfind('.'); dot != std::string::npos) name.erase(dot);
    return name;
}

// The Probe's window while it has no effect: a list of the installed VST3 plug-ins and a button.
//
// Step 1 lists the .vst3 files in the standard VST3 folders by file name. Nothing is loaded for
// that, so the window opens at once however many plug-ins are installed, and a plug-in that
// crashes when loaded can't take the DAW down just because the list was shown.
// "Load effect" opens only the chosen file. If it holds one effect, that one is loaded. If it
// holds several (a "shell"), step 2 lists them and the button loads the one picked.
//
// With `message` set, the window only shows that text (an effect without a window of its own).
class Picker final : public CPluginView {
public:
    explicit Picker(Probe* probe, std::wstring message = {}) : probe_(probe), message_(std::move(message)) {
        setRect({0, 0, 460, 270});   // the window's size, in pixels
    }
    ~Picker() override { if (window_) DestroyWindow(window_); }

    // The DAW asks which kind of window we can make: only a Windows one (an HWND).
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
    }
    // The DAW gives us its window to draw into: make ours a child of it.
    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (!parent || window_ || isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
        HINSTANCE instance = nullptr;   // our DLL: window classes belong to the module that registers them
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Picker::window_proc), &instance);
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // 32512 = IDC_ARROW, the normal arrow
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);            // the usual dialog grey
        wc.lpszClassName = L"TRIVUNE.SessionScope.ProbePicker";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return kResultFalse;   // a second Probe finds it registered
        window_ = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0,
                                  rect.getWidth(), rect.getHeight(), static_cast<HWND>(parent), nullptr, instance, this);
        return window_ ? CPluginView::attached(parent, type) : kResultFalse;
    }
    // The DAW closes the window.
    tresult PLUGIN_API removed() override {
        if (window_) DestroyWindow(window_);
        window_ = nullptr;
        return CPluginView::removed();
    }

private:
    // Ids of the two controls, as Windows reports them in WM_COMMAND.
    enum : int { ListId = 1, ButtonId = 2 };

    // Windows calls this for everything that happens to our window.
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {   // the first message: remember which Picker this window belongs to
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* picker = reinterpret_cast<Picker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (picker && message == WM_CREATE) picker->populate(window);
        // The button was clicked, or a list entry double-clicked.
        if (picker && message == WM_COMMAND &&
            (LOWORD(wparam) == ButtonId || (LOWORD(wparam) == ListId && HIWORD(wparam) == LBN_DBLCLK))) {
            auto keep_alive = shared(picker);   // loading may make the DAW close this window; stay alive until we return
            picker->choose(window);
            return 0;
        }
        if (picker && message == WM_NCDESTROY) picker->window_ = nullptr;   // the last message
        return DefWindowProcW(window, message, wparam, lparam);
    }

    // Makes the controls and fills the list with the installed .vst3 files.
    void populate(HWND window) {
        if (!message_.empty()) {
            CreateWindowW(L"STATIC", message_.c_str(), WS_CHILD | WS_VISIBLE, 12, 12, 430, 40,
                          window, nullptr, nullptr, nullptr);
            return;
        }
        label_ = CreateWindowW(L"STATIC", L"Choose an installed VST3 effect", WS_CHILD | WS_VISIBLE,
                               12, 10, 430, 20, window, nullptr, nullptr, nullptr);
        // LBS_NOTIFY: tell us about double-clicks. Typing a letter jumps to the entries starting with it.
        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                               WS_TABSTOP | LBS_NOTIFY, 12, 36, 436, 187, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ListId)), nullptr, nullptr);
        button_ = CreateWindowW(L"BUTTON", L"Load effect", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                12, 230, 120, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ButtonId)), nullptr, nullptr);

        // Step 1: every .vst3 in the standard folders, by file name. No plug-in is loaded here.
        for (const auto& path : Module::getModulePaths()) {
            const std::string title = module_title(path);
            if (title.rfind("SessionScope", 0) == 0) continue;   // not ourselves, and not the Sensor (it belongs on the master)
            choices_.push_back({path, VST3::UID{}, title});
        }
        show_choices();
        if (choices_.empty()) {
            SetWindowTextW(label_, L"No installed VST3 plug-ins were found.");
            EnableWindow(button_, false);
        }
    }

    // Puts choices_ into the list box, sorted by name, the first one selected.
    void show_choices() {
        std::stable_sort(choices_.begin(), choices_.end(), [](const auto& a, const auto& b) {
            return _stricmp(a.name.c_str(), b.name.c_str()) < 0;   // A-Z without caring about capitals
        });
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (const auto& choice : choices_) {
            const auto name = wide(choice.name);
            SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        if (!choices_.empty()) SendMessageW(list_, LB_SETCURSEL, 0, 0);
    }

    // The effects inside one .vst3 file. Instruments are left out: the Probe is an effect, and a
    // DAW would not send it notes.
    std::vector<EffectChoice> effects_in(const std::string& path, std::string& error) {
        std::vector<EffectChoice> effects;
        auto module = Module::create(path, error);   // loads the plug-in's DLL
        if (!module) return effects;
        for (const auto& info : module->getFactory().classInfos()) {
            if (info.category() != kVstAudioEffectClass || info.ID() == VST3::UID(ProbeUID.toTUID())) continue;
            const auto& categories = info.subCategories();
            if (std::find(categories.begin(), categories.end(), "Instrument") != categories.end()) continue;
            effects.push_back({path, info.ID(), info.name()});
        }
        if (effects.empty() && error.empty()) error = "This plug-in has no audio effect in it (it may be an instrument).";
        return effects;
    }

    // "Load effect" was clicked.
    void choose(HWND window) {
        const auto index = SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (index < 0 || static_cast<std::size_t>(index) >= choices_.size()) return;
        EffectChoice choice = choices_[static_cast<std::size_t>(index)];
        std::string error;

        if (!listing_effects_) {   // step 1: a file was picked; look inside it
            const auto effects = effects_in(choice.path, error);
            if (effects.size() > 1) {   // several effects in one file: step 2, pick one of them
                choices_ = effects;
                listing_effects_ = true;
                show_choices();
                SetWindowTextW(label_, L"This plug-in holds several effects. Choose one");
                return;
            }
            if (effects.empty()) {
                show_problem(window, error);
                return;
            }
            choice = effects.front();
        }

        if (!probe_->select(choice, error)) {
            show_problem(window, error);
            return;
        }
        if (IsWindow(window)) {   // the DAW may have closed us while loading
            SetWindowTextW(label_, L"Effect loaded. Close and reopen this window to see its controls.");
            EnableWindow(button_, false);
            EnableWindow(list_, false);
        }
    }

    void show_problem(HWND window, const std::string& error) {
        const auto message = wide(error);
        if (IsWindow(window)) MessageBoxW(window, message.c_str(), L"SessionScope Probe", MB_OK | MB_ICONWARNING);
    }

    IPtr<Probe> probe_;                    // keeps the Probe alive as long as its window exists
    std::wstring message_;                 // set: show only this text
    std::vector<EffectChoice> choices_;    // what the list shows: files (step 1) or one file's effects (step 2)
    bool listing_effects_ = false;         // false: step 1, true: step 2
    HWND window_ = nullptr;                // our child window
    HWND list_ = nullptr;
    HWND label_ = nullptr;
    HWND button_ = nullptr;
};

IPlugView* Probe::createView(FIDString name) {
    if (!name || std::strcmp(name, ViewType::kEditor) != 0) return nullptr;   // "editor" is the only kind of window there is
    if (auto* c = controller()) {
        if (auto* view = c->createView(name)) return view;   // the hosted effect's own window, handed straight to the DAW
    }
    return effect_ ? new Picker(this, L"The selected VST3 effect has no custom editor.") : new Picker(this);
}
}  // namespace

// The "factory": what a DAW reads to learn which plug-ins this file holds. One: the Probe, an
// audio effect in the "analyzer" category.
BEGIN_FACTORY_DEF("TRIVUNE", "https://github.com/TRIVUNE/Session_Scope", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(ProbeUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SessionScope Probe", 0, PlugType::kFxAnalyzer, "0.1.0", kVstVersionString, Probe::create)
END_FACTORY

// Called when the DAW loads and unloads this file; nothing to set up.
bool InitModule() { return true; }
bool DeinitModule() { return true; }
