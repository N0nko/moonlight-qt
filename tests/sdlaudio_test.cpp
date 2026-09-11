#include "../app/streaming/audio/renderers/sdl.h"
#include <algorithm>
#include <cassert>
#include <iostream>

// Stand in for the transport queue; the real renderer and SDL callback are used.
extern "C" int LiGetPendingAudioDuration(void) { return 0; }

int main()
{
    assert(SDL_setenv("SDL_AUDIODRIVER", "dummy", 1) == 0);
    for (bool adaptive : {false, true}) {
        for (int channels : {2, 6, 8}) {
            OPUS_MULTISTREAM_CONFIGURATION config {};
            config.sampleRate = 48000;
            config.channelCount = channels;
            config.samplesPerFrame = 240;
            SdlAudioRenderer renderer(adaptive, true);
            assert(renderer.prepareForPlayback(&config));
            auto data = static_cast<float*>(renderer.getAudioBuffer(nullptr));
            assert(data != nullptr);
            std::fill(data, data + channels * config.samplesPerFrame, 0.25f);
            for (int packet = 0; packet < 50; ++packet) {
                assert(renderer.submitAudio(channels * config.samplesPerFrame * sizeof(float)));
                SDL_Delay(5);
            }
            SDL_Delay(300); // callback keeps running, producer resumes after a gap
            renderer.flushAudio(); // real lifecycle event, not a wall-clock guess
            for (int packet = 0; packet < 10; ++packet) {
                assert(renderer.submitAudio(channels * config.samplesPerFrame * sizeof(float)));
                SDL_Delay(5);
            }
        }
    }
    std::cout << "SDL audio: legacy/adaptive, stereo/surround, producer gap and repeated teardown passed\n";
}
