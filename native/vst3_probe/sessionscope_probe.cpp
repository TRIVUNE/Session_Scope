// Windows VST3 wrapper for the SessionScope timing core. See handoff/task-manager-plan.md.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/base/ibstream.h"
#include "hosted_vst3.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;
using sessionscope::HostedVst3;
using VST3::Hosting::Module;

const FUID ProbeUID(0x8B2E807B, 0xC9014C31, 0xA9788B53, 0xB4EA30F2);
constexpr uint32 StateVersion = 1;
constexpr uint32 MaxStateBytes = 64 * 1024 * 1024;

struct EffectChoice {
    std::string path;
    VST3::UID id;
    std::string name;
};

bool transfer(IBStream* stream, void* data, uint32 size, bool write) {
    int32 count = 0;
    const auto result = write ? stream->write(data, size, &count) : stream->read(data, size, &count);
    return result == kResultOk && count == static_cast<int32>(size);
}

// Serialize lengths explicitly in little endian so saved sessions are portable.
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

bool transfer_blob(IBStream* stream, std::string& blob, bool write, uint32 limit) {
    if (write && blob.size() > limit) return false;
    uint32 size = write ? static_cast<uint32>(blob.size()) : 0;
    if (!transfer_number(stream, size, write) || size > limit) return false;
    if (!write) blob.resize(size);
    return size == 0 || transfer(stream, blob.data(), size, write);
}

template <typename Sample>
void copy_audio(Sample** source, Sample** destination, int32 channels, int32 frames) {
    if (!source || !destination) return;
    for (int32 channel = 0; channel < channels; ++channel) {
        if (source[channel] && destination[channel] && source[channel] != destination[channel])
            std::memcpy(destination[channel], source[channel], sizeof(Sample) * frames);
    }
}

class Probe final : public SingleComponentEffect, public IMidiMapping {
public:
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new Probe); }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IMidiMapping::iid)) {
            *obj = static_cast<IMidiMapping*>(this);
            addRef();
            return kResultOk;
        }
        return SingleComponentEffect::queryInterface(iid, obj);
    }
    REFCOUNT_METHODS(SingleComponentEffect)

    tresult PLUGIN_API initialize(FUnknown* context) override {
        const auto result = SingleComponentEffect::initialize(context);
        if (result == kResultOk) {
            addAudioInput(STR16("Audio In"), SpeakerArr::kStereo);
            addAudioOutput(STR16("Audio Out"), SpeakerArr::kStereo);
        }
        return result;
    }

    tresult PLUGIN_API terminate() override {
        if (auto* c = controller()) c->setComponentHandler(nullptr);
        active_effect_ = nullptr;
        effect_configured_ = false;
        effect_.reset();
        return SingleComponentEffect::terminate();
    }

    bool select(const EffectChoice& choice, std::string& error) {
        if (effect_) { error = "This Probe already has an effect."; return false; }
        auto effect = std::make_unique<HostedVst3>();
        if (!effect->load(choice.path, choice.id, error)) return false;
        install(std::move(effect), choice);
        // The host renegotiates buses, parameters and sample format using the new effect.
        if (auto* handler = getComponentHandler()) {
            if (FUnknownPtr<IComponentHandler2> extended{handler}; extended) extended->setDirty(true);
            handler->restartComponent(kReloadComponent | kIoChanged | kLatencyChanged | kParamTitlesChanged);
        }
        return true;
    }

    IPlugView* PLUGIN_API createView(FIDString name) override;

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
        if (ins != 1 || outs != 1 || in[0] != out[0] || !in[0]) return kResultFalse;
        return SingleComponentEffect::setBusArrangements(in, ins, out, outs);
    }
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
        return effect_ ? effect_->processor()->getBusArrangement(dir, index, arr)
                       : SingleComponentEffect::getBusArrangement(dir, index, arr);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        if (effect_) return effect_->processor()->canProcessSampleSize(size);
        // Some hosts choose the format before restoring the wrapped effect, which may be 32-bit only.
        return size == kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return effect_ ? effect_->processor()->getLatencySamples() : 0; }
    uint32 PLUGIN_API getTailSamples() override { return effect_ ? effect_->processor()->getTailSamples() : kNoTail; }
    uint32 PLUGIN_API getProcessContextRequirements() override {
        if (!effect_) return 0;
        FUnknownPtr<IProcessContextRequirements> requirements(effect_->processor());
        return requirements ? requirements->getProcessContextRequirements() : 0;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        const auto base = SingleComponentEffect::setupProcessing(setup);
        if (!effect_ || base != kResultOk) return base;
        const auto result = effect_->setup(setup);
        effect_configured_ = result == kResultOk;
        return result;
    }
    tresult PLUGIN_API setActive(TBool active) override {
        auto* hosted = active ? effect_.get() : active_effect_;
        if (active && hosted && !effect_configured_) {
            const auto result = hosted->setup(processSetup);
            if (result != kResultOk) return result;
            effect_configured_ = true;
        }
        if (hosted) {
            const auto result = hosted->component()->setActive(active);
            if (result != kResultOk) return result;
        }
        active_effect_ = active ? hosted : nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        return active_effect_ ? active_effect_->processor()->setProcessing(state) : kResultOk;
    }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (active_effect_) return active_effect_->process(data);
        if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
        const auto& in = data.inputs[0];
        auto& out = data.outputs[0];
        const auto channels = std::min(in.numChannels, out.numChannels);
        if (data.symbolicSampleSize == kSample64)
            copy_audio(in.channelBuffers64, out.channelBuffers64, channels, data.numSamples);
        else
            copy_audio(in.channelBuffers32, out.channelBuffers32, channels, data.numSamples);
        out.silenceFlags = in.silenceFlags;
        return kResultOk;
    }

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

    tresult PLUGIN_API getState(IBStream* stream) override;
    tresult PLUGIN_API setState(IBStream* stream) override;

private:
    IEditController* controller() const { return effect_ ? effect_->controller() : nullptr; }
    void install(std::unique_ptr<HostedVst3> effect, const EffectChoice& choice) {
        choice_ = choice;
        effect_ = std::move(effect);
        effect_configured_ = false;
        if (auto* c = controller()) c->setComponentHandler(getComponentHandler());
    }
    EffectChoice choice_{};
    std::unique_ptr<HostedVst3> effect_;
    HostedVst3* active_effect_ = nullptr;
    bool effect_configured_ = false;
};

std::string snapshot(tresult result, MemoryStream& stream) {
    if (result != kResultOk || stream.getSize() == 0) return {};
    return {stream.getData(), static_cast<std::size_t>(stream.getSize())};
}

tresult Probe::getState(IBStream* stream) {
    if (!stream) return kInvalidArgument;
    uint32 version = StateVersion;
    auto path = choice_.path;
    auto id = path.empty() ? std::string{} : choice_.id.toString();
    std::string component, editor;
    if (effect_) {
        MemoryStream state;
        component = snapshot(effect_->component()->getState(&state), state);
        if (auto* c = controller()) {
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

    if (path.empty()) return effect_ ? kResultFalse : kResultOk;
    const auto uid = VST3::UID::fromString(id);
    if (!uid || *uid == VST3::UID(ProbeUID.toTUID())) return kResultFalse;

    std::unique_ptr<HostedVst3> pending;
    if (!effect_) {
        pending = std::make_unique<HostedVst3>();
        std::string error;
        if (!pending->load(path, *uid, error)) return kResultFalse;
    } else if (choice_.path != path || choice_.id != *uid) {
        return kResultFalse;
    }
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
        c->setEditorState(&state);
    }
    if (pending) install(std::move(pending), {path, *uid, {}});
    return kResultOk;
}

std::wstring wide(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (!length) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
    result.pop_back();
    return result;
}

class Picker final : public CPluginView {
public:
    explicit Picker(Probe* probe, std::wstring message = {}) : probe_(probe), message_(std::move(message)) {
        setRect({0, 0, 460, 270});
    }
    ~Picker() override { if (window_) DestroyWindow(window_); }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (!parent || window_ || isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
        HINSTANCE instance = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Picker::window_proc), &instance);
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"TRIVUNE.SessionScope.ProbePicker";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return kResultFalse;
        window_ = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0,
                                  rect.getWidth(), rect.getHeight(), static_cast<HWND>(parent), nullptr, instance, this);
        return window_ ? CPluginView::attached(parent, type) : kResultFalse;
    }
    tresult PLUGIN_API removed() override {
        if (window_) DestroyWindow(window_);
        window_ = nullptr;
        return CPluginView::removed();
    }

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* picker = reinterpret_cast<Picker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (picker && message == WM_CREATE) picker->populate(window);
        if (picker && message == WM_COMMAND &&
            (LOWORD(wparam) == 2 || (LOWORD(wparam) == 1 && HIWORD(wparam) == LBN_DBLCLK))) {
            auto keep_alive = shared(picker);
            picker->choose(window);
            return 0;
        }
        if (picker && message == WM_NCDESTROY) picker->window_ = nullptr;
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void populate(HWND window) {
        if (!message_.empty()) {
            CreateWindowW(L"STATIC", message_.c_str(), WS_CHILD | WS_VISIBLE, 12, 12, 430, 40,
                          window, nullptr, nullptr, nullptr);
            return;
        }
        label_ = CreateWindowW(L"STATIC", L"Choose an installed VST3 effect", WS_CHILD | WS_VISIBLE,
                               12, 10, 430, 20, window, nullptr, nullptr, nullptr);
        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                               WS_TABSTOP | LBS_NOTIFY, 12, 36, 436, 187, window,
                               reinterpret_cast<HMENU>(1), nullptr, nullptr);
        button_ = CreateWindowW(L"BUTTON", L"Load effect", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                12, 230, 120, 28, window, reinterpret_cast<HMENU>(2), nullptr, nullptr);

        for (const auto& path : Module::getModulePaths()) {
            std::string error;
            auto module = Module::create(path, error);
            if (!module) continue;
            for (const auto& info : module->getFactory().classInfos()) {
                if (info.category() != kVstAudioEffectClass || info.ID() == VST3::UID(ProbeUID.toTUID())) continue;
                const auto& categories = info.subCategories();
                if (std::find(categories.begin(), categories.end(), "Instrument") != categories.end()) continue;
                choices_.push_back({path, info.ID(), info.name()});
            }
        }
        std::stable_sort(choices_.begin(), choices_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        for (const auto& choice : choices_) {
            const auto name = wide(choice.name);
            SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        if (choices_.empty()) {
            SetWindowTextW(label_, L"No installed VST3 effects were found.");
            EnableWindow(button_, false);
        } else {
            SendMessageW(list_, LB_SETCURSEL, 0, 0);
        }
    }

    void choose(HWND window) {
        const auto index = SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (index < 0 || static_cast<std::size_t>(index) >= choices_.size()) return;
        std::string error;
        if (!probe_->select(choices_[index], error)) {
            const auto message = wide(error);
            if (IsWindow(window)) MessageBoxW(window, message.c_str(), L"SessionScope Probe", MB_OK | MB_ICONWARNING);
            return;
        }
        if (IsWindow(window)) {
            SetWindowTextW(label_, L"Effect loaded. Reopen this editor to see its controls.");
            EnableWindow(button_, false);
            EnableWindow(list_, false);
        }
    }

    IPtr<Probe> probe_;
    std::wstring message_;
    std::vector<EffectChoice> choices_;
    HWND window_ = nullptr;
    HWND list_ = nullptr;
    HWND label_ = nullptr;
    HWND button_ = nullptr;
};

IPlugView* Probe::createView(FIDString name) {
    if (!name || std::strcmp(name, ViewType::kEditor) != 0) return nullptr;
    if (auto* c = controller()) {
        if (auto* view = c->createView(name)) return view;
    }
    return effect_ ? new Picker(this, L"The selected VST3 effect has no custom editor.") : new Picker(this);
}
}  // namespace

BEGIN_FACTORY_DEF("TRIVUNE", "https://github.com/TRIVUNE/Session_Scope", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(ProbeUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SessionScope Probe", 0, PlugType::kFxAnalyzer, "0.1.0", kVstVersionString, Probe::create)
END_FACTORY

bool InitModule() { return true; }
bool DeinitModule() { return true; }
