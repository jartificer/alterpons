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

// A minimal native VST2 host used by `run-smoke-test.sh`. It loads a yabridge
// VST2 plugin library that bridges `test-plugin.cpp`, processes some audio,
// changes a parameter, fetches a state chunk, and optionally opens the editor
// to check that the Wine window gets embedded.
//
// Usage: test-host <plugin.so> [--editor] [--blocks N] [--alongside <other.so>]
//
// With `--alongside`, the other plugin is loaded into the same process first
// and it will be processing audio on a background thread while the main
// plugin is being tested. This is used to test 32-bit and 64-bit plugins
// running at the same time.

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <vestige/aeffectx.h>

using namespace std::chrono_literals;

namespace {

constexpr int sample_rate = 48000;
constexpr int block_size = 512;
constexpr size_t expected_chunk_size = 1 << 20;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        printf("[ OK ] %s\n", message.c_str());
    } else {
        printf("[FAIL] %s\n", message.c_str());
        failures++;
    }
    fflush(stdout);
}

VstTimeInfo time_info{};

intptr_t VST_CALL_CONV host_callback(AEffect* /*effect*/,
                                     int opcode,
                                     int /*index*/,
                                     intptr_t /*value*/,
                                     void* /*data*/,
                                     float /*option*/) {
    switch (opcode) {
        case audioMasterVersion:
            return 2400;
        case audioMasterGetTime:
            time_info.sampleRate = sample_rate;
            time_info.tempo = 120.0;
            return reinterpret_cast<intptr_t>(&time_info);
        case audioMasterGetSampleRate:
            return sample_rate;
        case audioMasterGetBlockSize:
            return block_size;
        case audioMasterSizeWindow:
            return 1;
        default:
            return 0;
    }
}

/**
 * Process a block of audio and check that the output equals the input times
 * `expected_gain`. Returns the time it took to process the block.
 */
std::chrono::duration<double> process_block(AEffect* effect,
                                            float expected_gain,
                                            int block_index,
                                            bool& correct) {
    std::vector<float> left_in(block_size), right_in(block_size),
        left_out(block_size), right_out(block_size);
    for (int i = 0; i < block_size; i++) {
        const float t =
            static_cast<float>((block_index * block_size) + i) / sample_rate;
        left_in[i] = std::sin(2.0f * 3.14159265f * 440.0f * t);
        right_in[i] = std::cos(2.0f * 3.14159265f * 220.0f * t);
    }

    float* inputs[] = {left_in.data(), right_in.data()};
    float* outputs[] = {left_out.data(), right_out.data()};

    const auto start = std::chrono::steady_clock::now();
    effect->processReplacing(effect, inputs, outputs, block_size);
    const auto end = std::chrono::steady_clock::now();

    correct = true;
    for (int i = 0; i < block_size; i++) {
        if (std::abs(left_out[i] - (left_in[i] * expected_gain)) > 1e-6f ||
            std::abs(right_out[i] - (right_in[i] * expected_gain)) > 1e-6f) {
            correct = false;
            break;
        }
    }

    return end - start;
}

/**
 * Call `dispatcher()` from several threads at once, like Renoise does when it
 * isn't sandboxing plugins. yabridge then has to set up additional socket
 * connections to the Wine plugin host. Meanwhile we'll keep interrupting those
 * threads with a signal, since the host's threads may receive signals as well.
 * Neither of those should cause yabridge to think the Wine plugin host died.
 */
void test_concurrent_dispatch(AEffect* effect) {
    constexpr int num_threads = 4;
    constexpr int calls_per_thread = 250;

    struct sigaction action {};
    action.sa_handler = [](int) {};
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &action, nullptr);

    std::atomic_int wrong_results = 0;
    std::atomic_int threads_done = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&]() {
            for (int j = 0; j < calls_per_thread; j++) {
                char name[256] = {};
                effect->dispatcher(effect, effGetEffectName, 0, 0, name, 0.0f);
                if (!strstr(name, "yabridge smoke test")) {
                    wrong_results++;
                }
            }
            threads_done++;
        });
    }

    while (threads_done < num_threads) {
        for (auto& thread : threads) {
            pthread_kill(thread.native_handle(), SIGUSR2);
        }
        std::this_thread::sleep_for(200us);
    }
    for (auto& thread : threads) {
        thread.join();
    }

    signal(SIGUSR2, SIG_DFL);

    char name[256] = {};
    effect->dispatcher(effect, effGetEffectName, 0, 0, name, 0.0f);
    check(wrong_results == 0 && strstr(name, "yabridge smoke test"),
          "Concurrent dispatcher() calls from " + std::to_string(num_threads) +
              " threads while being interrupted by signals");
}

/**
 * Open the editor in a new X11 window, and check whether a window has been
 * embedded into it.
 */
void test_editor(AEffect* effect) {
    xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection)) {
        check(false, "Connect to the X11 server for the editor test");
        return;
    }

    const xcb_screen_t* screen =
        xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    const xcb_window_t window = xcb_generate_id(connection);
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0,
                      0, 400, 300, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, nullptr);
    xcb_map_window(connection, window);
    xcb_flush(connection);

    VstRect* rect = nullptr;
    effect->dispatcher(effect, effEditGetRect, 0, 0, &rect, 0.0f);
    check(rect && rect->right == 240 && rect->bottom == 120,
          "effEditGetRect() returns the plugin's editor size");

    const intptr_t open_result = effect->dispatcher(
        effect, effEditOpen, 0, 0,
        reinterpret_cast<void*>(static_cast<uintptr_t>(window)), 0.0f);
    check(open_result == 1, "effEditOpen() succeeds");

    // Give the Wine plugin host some time to embed its window
    int num_children = 0;
    for (int i = 0; i < 100 && num_children == 0; i++) {
        effect->dispatcher(effect, effEditIdle, 0, 0, nullptr, 0.0f);
        std::this_thread::sleep_for(50ms);

        xcb_query_tree_reply_t* reply = xcb_query_tree_reply(
            connection, xcb_query_tree(connection, window), nullptr);
        if (reply) {
            num_children = xcb_query_tree_children_length(reply);
            free(reply);
        }
    }
    check(num_children > 0,
          "The Wine window has been embedded into the host's window");

    // Let it run for a bit longer so we exercise the X11 event handling
    for (int i = 0; i < 20; i++) {
        effect->dispatcher(effect, effEditIdle, 0, 0, nullptr, 0.0f);
        std::this_thread::sleep_for(50ms);
    }

    check(effect->dispatcher(effect, effEditClose, 0, 0, nullptr, 0.0f) == 1,
          "effEditClose() succeeds");

    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <plugin.so> [--editor] [--blocks N] [--alongside "
                "<other.so>]\n",
                argv[0]);
        return 2;
    }

    const std::string plugin_path = argv[1];
    std::string alongside_path;
    bool with_editor = false;
    int num_blocks = 1000;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--editor") == 0) {
            with_editor = true;
        } else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
            num_blocks = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--alongside") == 0 && i + 1 < argc) {
            alongside_path = argv[++i];
        }
    }

    using VstEntry = AEffect* (*)(audioMasterCallback);

    // The other plugin processes audio on a background thread for the entire
    // duration of the test
    void* alongside_library = nullptr;
    AEffect* alongside_effect = nullptr;
    std::jthread alongside_thread;
    std::atomic_int alongside_blocks = 0;
    std::atomic_bool alongside_correct = true;
    if (!alongside_path.empty()) {
        alongside_library =
            dlopen(alongside_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        auto alongside_entry = alongside_library
                                   ? reinterpret_cast<VstEntry>(dlsym(
                                         alongside_library, "VSTPluginMain"))
                                   : nullptr;
        alongside_effect =
            alongside_entry ? alongside_entry(host_callback) : nullptr;
        check(alongside_effect != nullptr,
              "The other plugin (" + alongside_path + ") initialized");
        if (!alongside_effect) {
            return 1;
        }

        char name[256] = {};
        alongside_effect->dispatcher(alongside_effect, effOpen, 0, 0, nullptr,
                                     0.0f);
        alongside_effect->dispatcher(alongside_effect, effGetEffectName, 0, 0,
                                     name, 0.0f);
        printf("       Running '%s' alongside the main plugin\n", name);
        alongside_effect->dispatcher(alongside_effect, effSetSampleRate, 0, 0,
                                     nullptr, sample_rate);
        alongside_effect->dispatcher(alongside_effect, effSetBlockSize, 0,
                                     block_size, nullptr, 0.0f);
        alongside_effect->dispatcher(alongside_effect, effMainsChanged, 0, 1,
                                     nullptr, 0.0f);

        alongside_thread = std::jthread([&](std::stop_token stop_token) {
            for (int i = 0; !stop_token.stop_requested(); i++) {
                bool correct;
                process_block(alongside_effect, 0.5f, i, correct);
                if (!correct) {
                    alongside_correct = false;
                }
                alongside_blocks++;
                std::this_thread::sleep_for(5ms);
            }
        });
    }

    void* library = dlopen(plugin_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "Could not load '%s': %s\n", plugin_path.c_str(),
                dlerror());
        return 1;
    }

    auto entry = reinterpret_cast<VstEntry>(dlsym(library, "VSTPluginMain"));
    check(entry != nullptr, "The plugin exports VSTPluginMain()");
    if (!entry) {
        return 1;
    }

    const auto load_start = std::chrono::steady_clock::now();
    AEffect* effect = entry(host_callback);
    const auto load_end = std::chrono::steady_clock::now();
    check(effect != nullptr, "The plugin initialized");
    if (!effect) {
        return 1;
    }
    printf("       Loading the plugin took %.0f ms\n",
           std::chrono::duration<double, std::milli>(load_end - load_start)
               .count());

    check(effect->numInputs == 2 && effect->numOutputs == 2 &&
              effect->numParams == 1,
          "The plugin reports 2 inputs, 2 outputs, and 1 parameter");

    effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);

    char name[256] = {};
    effect->dispatcher(effect, effGetEffectName, 0, 0, name, 0.0f);
    check(strstr(name, "yabridge smoke test") != nullptr,
          std::string("effGetEffectName() returns '") + name + "'");

    effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, sample_rate);
    effect->dispatcher(effect, effSetBlockSize, 0, block_size, nullptr, 0.0f);
    effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);

    bool all_correct = true;
    std::vector<double> block_times_us;
    for (int i = 0; i < num_blocks; i++) {
        bool correct;
        const auto block_time = process_block(effect, 0.5f, i, correct);
        all_correct &= correct;
        block_times_us.push_back(
            std::chrono::duration<double, std::micro>(block_time).count());

        // Simulate a realistic audio callback rate of 512 samples at 48 kHz,
        // since the latency of waking up an idle thread is what matters
        std::this_thread::sleep_until(
            std::chrono::steady_clock::now() +
            std::chrono::microseconds(1'000'000 * block_size / sample_rate) -
            block_time);
    }
    check(all_correct, "Processing " + std::to_string(num_blocks) +
                           " blocks applies the default 0.5 gain");

    std::sort(block_times_us.begin(), block_times_us.end());
    const auto percentile = [&](double p) {
        return block_times_us[std::min(
            block_times_us.size() - 1,
            static_cast<size_t>(p *
                                static_cast<double>(block_times_us.size())))];
    };
    printf(
        "       Processing round trip: median %.1f us, p99 %.1f us, max %.1f "
        "us\n",
        percentile(0.5), percentile(0.99), block_times_us.back());

    effect->setParameter(effect, 0, 0.25f);
    check(std::abs(effect->getParameter(effect, 0) - 0.25f) < 1e-6f,
          "Parameter changes round trip");

    bool correct;
    process_block(effect, 0.25f, 0, correct);
    check(correct, "Processing uses the new gain");

    void* chunk_data = nullptr;
    const intptr_t chunk_size =
        effect->dispatcher(effect, effGetChunk, 0, 0, &chunk_data, 0.0f);
    bool chunk_correct =
        chunk_data && chunk_size == static_cast<intptr_t>(expected_chunk_size);
    for (size_t i = 0; chunk_correct && i < expected_chunk_size; i++) {
        chunk_correct = static_cast<unsigned char*>(chunk_data)[i] ==
                        static_cast<unsigned char>(i * 7);
    }
    check(chunk_correct, "effGetChunk() returns a correct 1 MiB chunk");

    test_concurrent_dispatch(effect);

    if (with_editor) {
        test_editor(effect);
    }

    effect->dispatcher(effect, effMainsChanged, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    dlclose(library);

    if (alongside_effect) {
        alongside_thread.request_stop();
        alongside_thread.join();
        check(alongside_correct && alongside_blocks > 0,
              "The other plugin correctly processed " +
                  std::to_string(alongside_blocks) +
                  " blocks at the same time");

        alongside_effect->dispatcher(alongside_effect, effMainsChanged, 0, 0,
                                     nullptr, 0.0f);
        alongside_effect->dispatcher(alongside_effect, effClose, 0, 0, nullptr,
                                     0.0f);
        dlclose(alongside_library);
    }

    if (failures > 0) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }

    printf("\nAll checks passed\n");
    return 0;
}
