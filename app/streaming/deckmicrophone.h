#pragma once

#include "streaming/extensions/deckprotocol.h"

#include <SDL.h>
#include <opus/opus.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

class DeckMicrophone
{
public:
    DeckMicrophone();
    ~DeckMicrophone();

    bool start();
    void stop();
    void setConnected(bool connected);
    bool handleStatus(std::uint32_t requestId,
                      DeckProtocol::MicrophoneStatus status);

private:
    static void SDLCALL captureCallback(void* context, Uint8* stream, int length);
    void capture(const Uint8* stream, int length);
    void run();
    bool sendConfiguration(bool enabled,
                           std::uint32_t generation,
                           std::uint32_t* requestId);
    std::uint32_t nextRequestId();

    SDL_AudioDeviceID m_Device;
    bool m_AudioSubsystemReference;
    std::atomic_bool m_Running;
    std::atomic_bool m_Connected;
    std::atomic_bool m_HostActive;
    std::atomic_uint32_t m_ConnectionGeneration;
    std::atomic_uint32_t m_NextRequestId;
    std::mutex m_ConnectionCallLock;
    std::mutex m_StateMutex;
    std::condition_variable m_Wake;
    std::deque<std::int16_t> m_Samples;
    bool m_NegotiationRequested;
    bool m_NegotiationPending;
    std::uint32_t m_PendingRequestId;
    std::uint32_t m_Sequence;
    std::chrono::steady_clock::time_point m_NegotiationDeadline;
    std::thread m_Thread;
    OpusEncoder* m_Encoder;
};
