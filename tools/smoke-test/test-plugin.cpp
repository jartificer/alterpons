// yabridge: a Wine plugin bridge
// Copyright (C) 2020-2026 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// A minimal Windows VST2 plugin used by `run-smoke-test.sh`. It applies a gain
// to its input, has a single gain parameter, returns a large state chunk, calls
// back into the host from both the GUI and the audio thread, and has a simple
// Win32 editor. It can be built as both a 32-bit and a 64-bit DLL to test
// yabridge's 32-bit PE host and 64-bit Winelib host.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include <vestige/aeffectx.h>

namespace {

constexpr int test_plugin_id = CCONST('y', 'b', 't', 's');
constexpr size_t chunk_size = 1 << 20;

constexpr wchar_t editor_class_name[] = L"yabridge smoke test editor";

// REAPER asks for a plugin's gain reduction using these vendor specific
// opcodes. The plugin claims to support this only while the gain parameter is
// set to 1.0. The test host can ask how many of these queries reached the
// plugin using the second set of codes.
constexpr int eff_vendor_specific = 50;
constexpr int presonus_vendor_id = CCONST('P', 'r', 'e', 'S');
constexpr intptr_t presonus_gain_reduction = CCONST('G', 'R', 'd', 'B');
constexpr intptr_t gain_reduction_query_count = CCONST('G', 'R', 'q', 'c');
// The test host can ask how many MIDI note on events the plugin has received
constexpr intptr_t note_on_count = CCONST('N', 'o', 't', 'e');
constexpr int eff_process_events = 25;

struct TestPlugin {
    AEffect effect{};
    audioMasterCallback master = nullptr;
    float gain = 0.5f;
    HWND editor = nullptr;
    VstRect editor_rect{0, 0, 120, 240};
    std::vector<unsigned char> chunk;
    int gain_reduction_queries = 0;
    int note_ons = 0;
};

TestPlugin* get_plugin(AEffect* effect) {
    return static_cast<TestPlugin*>(effect->ptr3);
}

LRESULT CALLBACK editor_proc(HWND window,
                             UINT message,
                             WPARAM wparam,
                             LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT rect;
        GetClientRect(window, &rect);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

intptr_t VST_CALL_CONV dispatcher(AEffect* effect,
                                  int opcode,
                                  int index,
                                  intptr_t value,
                                  void* data,
                                  float option) {
    TestPlugin* plugin = get_plugin(effect);
    switch (opcode) {
        case effOpen: {
            const intptr_t host_version =
                plugin->master(effect, audioMasterVersion, 0, 0, nullptr, 0.0f);
            fprintf(stderr,
                    "Smoke test plugin running as a %d-bit plugin, the host "
                    "reports VST version %d\n",
                    static_cast<int>(sizeof(void*) * 8),
                    static_cast<int>(host_version));
            fflush(stderr);
            return 0;
        }
        case effClose:
            delete plugin;
            return 0;
        case effGetEffectName:
        case effGetProductString:
            strcpy(static_cast<char*>(data),
                   sizeof(void*) == 4 ? "yabridge smoke test (32-bit)"
                                      : "yabridge smoke test (64-bit)");
            return 1;
        case effGetVendorString:
            strcpy(static_cast<char*>(data), "yabridge");
            return 1;
        case effGetVstVersion:
            return 2400;
        case effGetPlugCategory:
            return kPlugCategEffect;
        case effGetChunk: {
            // A large chunk to make sure large messages work
            plugin->chunk.resize(chunk_size);
            for (size_t i = 0; i < chunk_size; i++) {
                plugin->chunk[i] = static_cast<unsigned char>(i * 7);
            }
            *static_cast<void**>(data) = plugin->chunk.data();
            return static_cast<intptr_t>(plugin->chunk.size());
        }
        case effEditGetRect:
            *static_cast<VstRect**>(data) = &plugin->editor_rect;
            return 1;
        case effEditOpen: {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = editor_proc;
            window_class.hInstance = GetModuleHandleW(nullptr);
            window_class.lpszClassName = editor_class_name;
            RegisterClassExW(&window_class);

            plugin->editor = CreateWindowExW(
                0, editor_class_name, L"", WS_CHILD | WS_VISIBLE, 0, 0,
                plugin->editor_rect.right, plugin->editor_rect.bottom,
                static_cast<HWND>(data), nullptr, GetModuleHandleW(nullptr),
                nullptr);

            // The host should be able to handle callbacks from the GUI thread
            plugin->master(effect, audioMasterSizeWindow,
                           plugin->editor_rect.right,
                           plugin->editor_rect.bottom, nullptr, 0.0f);

            return plugin->editor ? 1 : 0;
        }
        case effEditClose:
            if (plugin->editor) {
                DestroyWindow(plugin->editor);
                plugin->editor = nullptr;
            }
            return 1;
        case eff_vendor_specific:
            if (index == presonus_vendor_id &&
                value == presonus_gain_reduction) {
                plugin->gain_reduction_queries++;
                return plugin->gain >= 1.0f ? 1 : 0;
            } else if (index == test_plugin_id &&
                       value == gain_reduction_query_count) {
                return plugin->gain_reduction_queries;
            } else if (index == test_plugin_id && value == note_on_count) {
                return plugin->note_ons;
            }
            return 0;
        case eff_process_events: {
            const auto* events = static_cast<const VstEvents*>(data);
            for (int i = 0; i < events->numEvents; i++) {
                const auto* event =
                    reinterpret_cast<const VstMidiEvent*>(events->events[i]);
                if (event->type == kVstMidiType &&
                    (event->midiData[0] & 0xf0) == 0x90 &&
                    event->midiData[2] > 0) {
                    plugin->note_ons++;
                }
            }
            return 1;
        }
        default:
            return 0;
    }
}

void VST_CALL_CONV process_replacing(AEffect* effect,
                                     float** inputs,
                                     float** outputs,
                                     int sample_frames) {
    TestPlugin* plugin = get_plugin(effect);

    // Plugins often query the transport information from the audio thread
    plugin->master(effect, audioMasterGetTime, 0, 0, nullptr, 0.0f);

    for (int channel = 0; channel < 2; channel++) {
        for (int i = 0; i < sample_frames; i++) {
            outputs[channel][i] = inputs[channel][i] * plugin->gain;
        }
    }
}

void VST_CALL_CONV set_parameter(AEffect* effect, int index, float value) {
    if (index == 0) {
        get_plugin(effect)->gain = value;
    }
}

float VST_CALL_CONV get_parameter(AEffect* effect, int index) {
    return index == 0 ? get_plugin(effect)->gain : 0.0f;
}

}  // namespace

extern "C" __declspec(dllexport) AEffect* VST_CALL_CONV
VSTPluginMain(audioMasterCallback master) {
    auto* plugin = new TestPlugin();
    plugin->master = master;

    AEffect& effect = plugin->effect;
    effect.magic = CCONST('V', 's', 't', 'P');
    effect.dispatcher = dispatcher;
    effect.setParameter = set_parameter;
    effect.getParameter = get_parameter;
    effect.numPrograms = 0;
    effect.numParams = 1;
    effect.numInputs = 2;
    effect.numOutputs = 2;
    effect.flags = effFlagsHasEditor | effFlagsCanReplacing;
    effect.ptr3 = plugin;
    effect.uniqueID = test_plugin_id;
    effect.version = 1;
    effect.processReplacing = process_replacing;

    return &effect;
}
