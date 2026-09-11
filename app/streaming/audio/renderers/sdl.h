#pragma once

#include "renderer.h"
#include "SDL_compat.h"
#include "../adaptivebuffer.h"

class SdlAudioRenderer : public IAudioRenderer
{
public:
    explicit SdlAudioRenderer(bool adaptive = false, bool diagnostics = false);

    virtual ~SdlAudioRenderer();

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig);

    virtual void* getAudioBuffer(int* size);

    virtual bool submitAudio(int bytesWritten);

    virtual AudioFormat getAudioBufferFormat();

private:
    static void audioCallback(void* context, Uint8* stream, int length);
    AdaptiveAudioBuffer m_AdaptiveBuffer;
    bool m_Adaptive;
    bool m_Diagnostics;
    Uint32 m_LastSubmitTicks = 0;
    Uint32 m_LastDiagnosticTicks = 0;
    std::atomic<Uint32> m_LastCallbackTicks {0};
    Uint32 m_Channels = 0;
    SDL_AudioDeviceID m_AudioDevice;
    bool m_AudioSubsystemReference;
    void* m_AudioBuffer;
    Uint32 m_FrameSize;
    Uint32 m_FrameDurationMs;
};
