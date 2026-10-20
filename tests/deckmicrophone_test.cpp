// Exercise the real microphone worker with synthetic capture and transport.
// No audio hardware or host connection is needed; Opus itself is not mocked.
#include "streaming/deckmicrophone.h"
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <vector>
using namespace std::chrono_literals;
namespace {
SDL_AudioSpec captureSpec {};
std::mutex transportMutex;
std::vector<std::uint32_t> requests;
std::atomic_uint packets {0};
std::atomic_bool failSend {false};
DeckMicrophone* immediateReply = nullptr;
bool immediateAccepted = false;
void waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    assert(predicate());
}
unsigned count()
{
    std::lock_guard<std::mutex> lock(transportMutex);
    return static_cast<unsigned>(requests.size());
}
std::uint32_t lastRequest()
{
    std::lock_guard<std::mutex> lock(transportMutex);
    assert(!requests.empty());
    return requests.back();
}
void clearTransport()
{
    std::lock_guard<std::mutex> lock(transportMutex);
    requests.clear(); packets = 0; failSend = false;
    immediateReply = nullptr; immediateAccepted = false;
}
void feedAndCheckAudio()
{
    const auto before = packets.load();
    std::array<std::int16_t,960> samples {};
    samples.fill(1000);
    captureSpec.callback(captureSpec.userdata, reinterpret_cast<Uint8*>(samples.data()), sizeof(samples));
    waitFor([before]() { return packets.load() > before; });
}
void logMessage(const char* format, va_list args)
{
    std::vprintf(format,args); std::puts("");
}
}
const char* SDL_GetError() { return "test capture"; }
SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* requested, SDL_AudioSpec* obtained, int)
{
    captureSpec = *requested; *obtained = *requested; return 1;
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
void SDL_CloseAudioDevice(SDL_AudioDeviceID) {}
void SDL_LogError(int, const char* format, ...) { va_list args; va_start(args,format); logMessage(format,args); va_end(args); }
void SDL_LogWarn(int, const char* format, ...) { va_list args; va_start(args,format); logMessage(format,args); va_end(args); }
void SDL_LogInfo(int, const char* format, ...) { va_list args; va_start(args,format); logMessage(format,args); va_end(args); }
int LiSendExtensionMessage(std::uint8_t, std::uint8_t opcode, std::uint32_t id, const void* payload, std::uint16_t, int)
{
    if (opcode == DeckProtocol::DeckMicrophoneConfigure && static_cast<const std::uint8_t*>(payload)[0]) {
        if (immediateReply) {
            // Force the reply-before-send-return interleaving deterministically.
            immediateAccepted = immediateReply->handleStatus(id, DeckProtocol::MicrophoneStatus::Active);
        }
        std::lock_guard<std::mutex> lock(transportMutex);
        requests.push_back(id);
        return failSend ? -1 : 0;
    }
    if (opcode == DeckProtocol::DeckMicrophoneOpus) ++packets;
    return 0;
}
int main()
{
    DeckMicrophone mic;
    assert(mic.start()); immediateReply = &mic; mic.setConnected(true);
    waitFor([]() { return count() == 1; });
    assert(immediateAccepted); feedAndCheckAudio();
    mic.stop(); clearTransport();
    std::puts("PASS immediate ACK and audio forwarding");

    assert(mic.start()); mic.setConnected(true);
    waitFor([]() { return count() == 1; });
    auto oldRequest = lastRequest();
    waitFor([]() { return count() == 2; });
    assert(!mic.handleStatus(oldRequest, DeckProtocol::MicrophoneStatus::Active));
    assert(mic.handleStatus(lastRequest(), DeckProtocol::MicrophoneStatus::Active));
    feedAndCheckAudio();
    mic.stop(); clearTransport();
    std::puts("PASS handshake timeout retries and rejects superseded reply");

    assert(mic.start()); mic.setConnected(true);
    waitFor([]() { return count() == 1; });
    std::this_thread::sleep_for(3100ms);
    assert(count() == 1);
    assert(mic.handleStatus(lastRequest(), DeckProtocol::MicrophoneStatus::Active));
    std::this_thread::sleep_for(300ms);
    assert(count() == 1); feedAndCheckAudio();
    mic.stop(); clearTransport();
    std::puts("PASS late ACK during backoff cancels retry");

    failSend = true; assert(mic.start()); mic.setConnected(true);
    waitFor([]() { return count() == 1; });
    failSend = false;
    waitFor([]() { return count() == 2; });
    assert(mic.handleStatus(lastRequest(), DeckProtocol::MicrophoneStatus::Failed));
    waitFor([]() { return count() == 3; });
    assert(mic.handleStatus(lastRequest(), DeckProtocol::MicrophoneStatus::Active));
    feedAndCheckAudio();
    mic.stop(); clearTransport();
    std::puts("PASS send failure and transient host failure recover");

    assert(mic.start()); mic.setConnected(true);
    waitFor([]() { return count() == 1; });
    assert(mic.handleStatus(lastRequest(), DeckProtocol::MicrophoneStatus::Unsupported));
    std::this_thread::sleep_for(500ms); assert(count() == 1);
    mic.stop(); clearTransport();
    std::puts("PASS unsupported host does not retry");

    assert(mic.start()); mic.setConnected(true);
    waitFor([]() { return count() == 1; }); oldRequest = lastRequest();
    assert(mic.handleStatus(oldRequest, DeckProtocol::MicrophoneStatus::Failed));
    mic.setConnected(false);
    std::this_thread::sleep_for(400ms); assert(count() == 1);
    assert(!mic.handleStatus(oldRequest, DeckProtocol::MicrophoneStatus::Active));
    mic.setConnected(true); waitFor([]() { return count() == 2; });
    assert(!mic.handleStatus(oldRequest, DeckProtocol::MicrophoneStatus::Active));
    assert(mic.handleStatus(lastRequest(), DeckProtocol::MicrophoneStatus::Active));
    feedAndCheckAudio(); mic.stop(); clearTransport();
    std::puts("PASS disconnect cancels retry and reconnect rejects stale ACK");

    failSend = true; assert(mic.start()); mic.setConnected(true);
    waitFor([]() { return count() == 20; }, 45s);
    std::this_thread::sleep_for(2200ms); assert(count() == 20);
    mic.stop(); clearTransport();
    assert(mic.start()); mic.setConnected(true);
    waitFor([]() { return count() == 1; });
    assert(mic.handleStatus(lastRequest(), DeckProtocol::MicrophoneStatus::Active));
    feedAndCheckAudio(); mic.stop();
    std::puts("PASS bounded retries and explicit restart resets budget");
}
