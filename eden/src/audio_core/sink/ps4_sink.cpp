// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <orbis/AudioOut.h>
#include <unistd.h>
#include "audio_core/sink/ps4_sink.h"
#include "common/logging.h"
#include "common/ps4_diagnostics.h"
#include "common/thread.h"

namespace AudioCore::Sink {
namespace {
// The device drains one buffer per frames/TargetSampleRate, and the guest's audio renderer can
// never get ahead of it, so this period is the quantisation of any frame that is waiting on
// audio. At 1024 frames that is 21.33 ms — 64% of a 30 Hz frame — and it shows up in the frame
// log as gaps clustered on multiples of 21.3 ms while the attendant speaks. 512 halves the
// quantisation to 10.67 ms for one extra sceAudioOutOutput call per period. The open call is
// offered the preferred width first and the old one second: the accepted widths are a property
// of the hardware, so a rejected 512 must not leave the sink dead.
constexpr u32 kPreferredFramesPerBuffer = 512;
constexpr u32 kFallbackFramesPerBuffer = 1024;
constexpr u32 kQueueBuffers = 8;
class PS4SinkStream final : public SinkStream {
public:
    PS4SinkStream(Core::System& system_, u32 channels, StreamType type_)
        : SinkStream{system_, type_} {
        system_channels = channels;
        device_channels = 2;
        if (type != StreamType::In) {
            port = sceAudioOutOpen(0xff, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kPreferredFramesPerBuffer,
                TargetSampleRate, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            if (port >= 0) {
                frames_per_buffer = kPreferredFramesPerBuffer;
            } else {
                LOG_WARNING(Audio_Sink, "PS4 AudioOut rejected {} frames ({:#x}), retrying with {}",
                            kPreferredFramesPerBuffer, u32(port), kFallbackFramesPerBuffer);
                port = sceAudioOutOpen(0xff, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                    kFallbackFramesPerBuffer, TargetSampleRate, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                if (port >= 0) {
                    frames_per_buffer = kFallbackFramesPerBuffer;
                }
            }
            if (port < 0) {
                LOG_ERROR(Audio_Sink, "PS4 AudioOut open failed: {:#x}", u32(port));
                throw std::runtime_error{"PS4 AudioOut unavailable"};
            }
            LOG_INFO(Audio_Sink,
                     "PS4 AudioOut opened: port={}, 48000 Hz stereo, {} frames/buffer ({:.2f} ms), "
                     "{} guest channels",
                     port, frames_per_buffer, 1000.0 * frames_per_buffer / TargetSampleRate,
                     channels);
        } else {
            LOG_WARNING(Audio_Sink, "PS4 microphone capture supplies silence");
        }
    }
    ~PS4SinkStream() override { Finalize(); }
    void Start(bool = false) override {
        if (running.exchange(true)) return;
        if (worker.joinable()) worker.join();
        paused = false;
        try {
            worker = std::thread([this] { Output(); });
        } catch (...) {
            running = false;
            paused = true;
            throw;
        }
    }
    void Stop() override {
        running = false;
        paused = true;
        SignalPause();
        if (worker.joinable()) worker.join();
    }
    void Finalize() override {
        Stop();
        if (port >= 0) {
            sceAudioOutClose(port);
            port = -1;
        }
    }
private:
    void Output() {
        Common::SetCurrentThreadName("PS4AudioOut");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        std::array<std::array<s16, kFallbackFramesPerBuffer * 2>, 2> buffers{};
        size_t index{};
        while (running) {
            auto& buffer = buffers[index];
            if (type == StreamType::In) {
                ProcessAudioIn(buffer, frames_per_buffer);
                usleep(frames_per_buffer * 1000000ULL / TargetSampleRate);
            } else {
                ProcessAudioOutAndRender(buffer, frames_per_buffer);
                const int result = sceAudioOutOutput(port, buffer.data());
                if (result < 0) {
                    LOG_ERROR(Audio_Sink, "PS4 AudioOut output failed: {:#x}", u32(result));
                    break;
                }
                if (++Common::PS4Diagnostics::audio_buffers == 1) {
                    LOG_INFO(Audio_Sink, "PS4 first audio buffer submitted");
                }
            }
            index ^= 1;
        }
        if (port >= 0) sceAudioOutOutput(port, nullptr);
        running = false;
        paused = true;
        SignalPause();
    }
    int port{-1};
    u32 frames_per_buffer{kFallbackFramesPerBuffer};
    std::atomic<bool> running{};
    std::thread worker;
};
}

PS4Sink::PS4Sink(std::string_view) {
    const int result = sceAudioOutInit();
    if (result != 0 && u32(result) != 0x8026000e) {
        LOG_ERROR(Audio_Sink, "PS4 AudioOut initialization failed: {:#x}", u32(result));
        throw std::runtime_error{"PS4 AudioOut initialization failed"};
    }
}
PS4Sink::~PS4Sink() { CloseStreams(); }
SinkStream* PS4Sink::AcquireSinkStream(Core::System& system, u32 channels,
                                      const std::string&, StreamType type) {
    auto stream = std::make_unique<PS4SinkStream>(system, channels, type);
    // SinkStream::WaitFreeSpace waits for `queued_buffers < max_queue_size`, and nothing in this
    // tree ever calls SetRingSize, so the default 0 makes that condition unsatisfiable: every
    // ADSP render command pays the full 7 ms timeout, and once more than three buffers are queued
    // the wait cannot end at all — the app's RequestUpdateAudioRenderer would never get its reply.
    // Eight buffers is more than the guest ever keeps in flight, so the wait returns at once and
    // the send-ahead bound is left to the guest's own buffer count.
    stream->SetRingSize(kQueueBuffers);
    stream->SetDeviceVolume(device_volume);
    stream->SetSystemVolume(system_volume);
    auto* result = stream.get();
    streams.push_back(std::move(stream));
    return result;
}
void PS4Sink::CloseStream(SinkStream* stream) {
    std::erase_if(streams, [stream](const auto& entry) { return entry.get() == stream; });
}
void PS4Sink::CloseStreams() { streams.clear(); }
void PS4Sink::SetDeviceVolume(f32 volume) {
    device_volume = volume;
    for (auto& stream : streams) stream->SetDeviceVolume(volume);
}
void PS4Sink::SetSystemVolume(f32 volume) {
    system_volume = volume;
    for (auto& stream : streams) stream->SetSystemVolume(volume);
}
}
