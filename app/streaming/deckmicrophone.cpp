#include "deckmicrophone.h"

#include "streaming/audio/sdlaudiosubsystem.h"

#include <Limelight.h>

#include <algorithm>
#include <array>

namespace {
constexpr int SampleRate = static_cast<int>(DeckProtocol::DeckMicrophoneSampleRate);
constexpr int FramesPerPacket = DeckProtocol::DeckMicrophoneFrameSamples;
constexpr std::size_t MaxQueuedSamples = SampleRate / 10;
constexpr int OpusBitrate = 48000;
constexpr auto NegotiationTimeout = std::chrono::seconds(3);
constexpr unsigned int NegotiationAttemptLimit = 20;
constexpr auto NegotiationRetryBase = std::chrono::milliseconds(250);
}

DeckMicrophone::DeckMicrophone()
    : m_Device(0),
      m_AudioSubsystemReference(false),
      m_Running(false),
      m_Connected(false),
      m_HostActive(false),
      m_ConnectionGeneration(0),
      m_NextRequestId(1),
      m_NegotiationRequested(false),
      m_NegotiationPending(false),
      m_PendingRequestId(0),
      m_NegotiationAttempts(0),
      m_Sequence(1),
      m_Encoder(nullptr)
{
}

DeckMicrophone::~DeckMicrophone()
{
    stop();
}

bool DeckMicrophone::start()
{
    if (m_Running.load()) {
        return true;
    }

    m_AudioSubsystemReference = SdlAudioSubsystem::acquire();
    if (!m_AudioSubsystemReference) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Deck microphone audio initialization failed: %s",
                     SDL_GetError());
        return false;
    }

    int opusError = OPUS_OK;
    m_Encoder = opus_encoder_create(SampleRate,
                                    DeckProtocol::DeckMicrophoneChannels,
                                    OPUS_APPLICATION_AUDIO,
                                    &opusError);
    if (m_Encoder == nullptr || opusError != OPUS_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to initialize Deck microphone Opus: %s",
                     opus_strerror(opusError));
        if (m_Encoder != nullptr) {
            opus_encoder_destroy(m_Encoder);
            m_Encoder = nullptr;
        }
        SdlAudioSubsystem::release();
        m_AudioSubsystemReference = false;
        return false;
    }

    const std::array<int, 7> controls = {
        opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(OpusBitrate)),
        opus_encoder_ctl(m_Encoder, OPUS_SET_VBR(1)),
        opus_encoder_ctl(m_Encoder, OPUS_SET_VBR_CONSTRAINT(1)),
        opus_encoder_ctl(m_Encoder, OPUS_SET_COMPLEXITY(5)),
        opus_encoder_ctl(m_Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)),
        opus_encoder_ctl(m_Encoder, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)),
        opus_encoder_ctl(m_Encoder, OPUS_SET_LSB_DEPTH(16)),
    };
    const auto controlFailure = std::find_if(
                controls.begin(), controls.end(),
                [](int status) { return status != OPUS_OK; });
    if (controlFailure != controls.end()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to configure Deck microphone Opus: %s",
                     opus_strerror(*controlFailure));
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
        SdlAudioSubsystem::release();
        m_AudioSubsystemReference = false;
        return false;
    }

    SDL_AudioSpec requested = {};
    requested.freq = SampleRate;
    requested.format = AUDIO_S16SYS;
    requested.channels = DeckProtocol::DeckMicrophoneChannels;
    requested.samples = FramesPerPacket;
    requested.callback = captureCallback;
    requested.userdata = this;

    SDL_AudioSpec obtained = {};
    m_Device = SDL_OpenAudioDevice(nullptr,
                                   SDL_TRUE,
                                   &requested,
                                   &obtained,
                                   0);
    if (m_Device == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to open the Deck microphone: %s",
                     SDL_GetError());
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
        SdlAudioSubsystem::release();
        m_AudioSubsystemReference = false;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        m_Samples.clear();
        m_NegotiationRequested = false;
        m_NegotiationPending = false;
        m_PendingRequestId = 0;
        m_NegotiationAttempts = 0;
        m_Sequence = 1;
    }
    m_HostActive.store(false);
    m_Connected.store(false);
    m_Running.store(true);
    m_Thread = std::thread(&DeckMicrophone::run, this);
    SDL_PauseAudioDevice(m_Device, 0);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Deck microphone capture opened at %d Hz, %u channel(s), Opus %d Kbps",
                obtained.freq,
                obtained.channels,
                OpusBitrate / 1000);
    return true;
}

void DeckMicrophone::stop()
{
    if (!m_Running.exchange(false)) {
        return;
    }

    if (m_Connected.load()) {
        sendConfiguration(false,
                          m_ConnectionGeneration.load(),
                          nextRequestId());
    }
    setConnected(false);
    m_Wake.notify_all();

    if (m_Thread.joinable()) {
        m_Thread.join();
    }
    if (m_Device != 0) {
        SDL_PauseAudioDevice(m_Device, 1);
        SDL_CloseAudioDevice(m_Device);
        m_Device = 0;
    }
    if (m_Encoder != nullptr) {
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
    }
    if (m_AudioSubsystemReference) {
        SdlAudioSubsystem::release();
        m_AudioSubsystemReference = false;
    }
}

bool DeckMicrophone::isRunning() const
{
    return m_Running.load(std::memory_order_acquire);
}

void DeckMicrophone::setConnected(bool connected)
{
    if (!connected) {
        m_Connected.store(false, std::memory_order_release);
        m_HostActive.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> callLock(m_ConnectionCallLock);
        }
        {
            std::lock_guard<std::mutex> stateLock(m_StateMutex);
            m_NegotiationRequested = false;
            m_NegotiationPending = false;
            m_PendingRequestId = 0;
            m_Samples.clear();
        }
        m_Wake.notify_all();
        return;
    }

    if (!m_Running.load()) {
        return;
    }

    m_ConnectionGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_Connected.store(true, std::memory_order_release);
    m_HostActive.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> stateLock(m_StateMutex);
        m_NegotiationRequested = true;
        m_NegotiationPending = false;
        m_PendingRequestId = 0;
        m_NegotiationAttempts = 0;
        m_NegotiationRetryAt = std::chrono::steady_clock::now();
        m_Sequence = 1;
        m_Samples.clear();
    }
    m_Wake.notify_all();
}

bool DeckMicrophone::handleStatus(std::uint32_t requestId,
                                  DeckProtocol::MicrophoneStatus status)
{
    std::lock_guard<std::mutex> stateLock(m_StateMutex);
    if (!m_Running.load(std::memory_order_acquire) ||
            !m_Connected.load(std::memory_order_acquire) ||
            requestId == 0 ||
            requestId != m_PendingRequestId) {
        return false;
    }

    m_NegotiationPending = false;
    m_NegotiationRequested = false;
    m_PendingRequestId = 0;
    m_Samples.clear();
    const bool active = status == DeckProtocol::MicrophoneStatus::Active;
    m_HostActive.store(active, std::memory_order_release);
    if (active) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Native Deck microphone is active");
    }
    else if (status == DeckProtocol::MicrophoneStatus::Failed) {
        scheduleNegotiationRetry("host initialization failed");
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Native Deck microphone is unavailable on this host (status %u)",
                    static_cast<unsigned int>(status));
    }
    m_Wake.notify_all();
    return true;
}

void SDLCALL DeckMicrophone::captureCallback(void* context,
                                             Uint8* stream,
                                             int length)
{
    static_cast<DeckMicrophone*>(context)->capture(stream, length);
}

void DeckMicrophone::capture(const Uint8* stream, int length)
{
    if (!m_Running.load(std::memory_order_acquire) ||
            !m_HostActive.load(std::memory_order_acquire) ||
            stream == nullptr || length < static_cast<int>(sizeof(std::int16_t))) {
        return;
    }

    const auto* samples = reinterpret_cast<const std::int16_t*>(stream);
    const std::size_t sampleCount = static_cast<std::size_t>(length) /
                                    sizeof(std::int16_t);
    std::lock_guard<std::mutex> stateLock(m_StateMutex);
    if (!m_Connected.load(std::memory_order_acquire) ||
            !m_HostActive.load(std::memory_order_acquire)) {
        return;
    }
    if (sampleCount >= MaxQueuedSamples) {
        m_Samples.clear();
        m_Samples.insert(m_Samples.end(),
                         samples + sampleCount - MaxQueuedSamples,
                         samples + sampleCount);
    }
    else {
        const std::size_t overflow =
                m_Samples.size() + sampleCount > MaxQueuedSamples ?
                    m_Samples.size() + sampleCount - MaxQueuedSamples : 0;
        for (std::size_t index = 0; index < overflow; ++index) {
            m_Samples.pop_front();
        }
        m_Samples.insert(m_Samples.end(), samples, samples + sampleCount);
    }
    m_Wake.notify_one();
}

std::uint32_t DeckMicrophone::nextRequestId()
{
    std::uint32_t requestId = m_NextRequestId.fetch_add(1);
    if (requestId == 0) {
        requestId = m_NextRequestId.fetch_add(1);
    }
    return requestId;
}

bool DeckMicrophone::sendConfiguration(bool enabled,
                                       std::uint32_t generation,
                                       std::uint32_t requestId)
{
    const auto payload = DeckProtocol::makeMicrophoneConfiguration(enabled);
    std::lock_guard<std::mutex> callLock(m_ConnectionCallLock);
    if ((enabled && !m_Running.load(std::memory_order_acquire)) ||
            !m_Connected.load(std::memory_order_acquire) ||
            m_ConnectionGeneration.load(std::memory_order_acquire) != generation) {
        return false;
    }

    if (LiSendExtensionMessage(DeckProtocol::DeckMicrophoneFeature,
                               DeckProtocol::DeckMicrophoneConfigure,
                               requestId,
                               payload.data(),
                               static_cast<std::uint16_t>(payload.size()),
                               LI_EXTENSION_MESSAGE_RELIABLE) != 0) {
        return false;
    }

    return true;
}

void DeckMicrophone::scheduleNegotiationRetry(const char* reason)
{
    m_NegotiationPending = false;
    m_HostActive.store(false, std::memory_order_release);
    if (!m_Running.load() || !m_Connected.load()) {
        m_NegotiationRequested = false;
        return;
    }
    if (m_NegotiationAttempts >= NegotiationAttemptLimit) {
        m_NegotiationRequested = false;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Deck microphone unavailable after %u attempts: %s; toggle microphone to retry",
                    m_NegotiationAttempts, reason);
        return;
    }
    const auto delay = NegotiationRetryBase *
            (1u << std::min(m_NegotiationAttempts - 1, 3u));
    m_NegotiationRetryAt = std::chrono::steady_clock::now() + delay;
    m_NegotiationRequested = true;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Deck microphone %s; retrying in %u ms (attempt %u/%u)",
                reason, static_cast<unsigned int>(delay.count()),
                m_NegotiationAttempts + 1, NegotiationAttemptLimit);
}

void DeckMicrophone::run()
{
    std::array<std::int16_t, FramesPerPacket> samples {};
    std::array<std::uint8_t, LI_EXTENSION_MAX_PAYLOAD> packet {};

    while (m_Running.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> stateLock(m_StateMutex);
        if (m_NegotiationPending) {
            const auto deadline = m_NegotiationDeadline;
            if (!m_Wake.wait_until(stateLock, deadline, [this]() {
                    return !m_Running.load(std::memory_order_acquire) ||
                           !m_Connected.load(std::memory_order_acquire) ||
                           !m_NegotiationPending;
                }) && m_NegotiationPending) {
                // Keep the request ID until the next attempt so a late success
                // during backoff can still activate this connection.
                scheduleNegotiationRetry("handshake timed out");
            }
            continue;
        }

        m_Wake.wait(stateLock, [this]() {
            return !m_Running.load(std::memory_order_acquire) ||
                   m_NegotiationRequested ||
                   (m_Connected.load(std::memory_order_acquire) &&
                    m_HostActive.load(std::memory_order_acquire) &&
                    m_Samples.size() >= FramesPerPacket);
        });
        if (!m_Running.load(std::memory_order_acquire)) {
            break;
        }

        if (m_NegotiationRequested) {
            const auto generation = m_ConnectionGeneration.load();
            if (m_Wake.wait_until(stateLock, m_NegotiationRetryAt, [this, generation]() {
                    return !m_Running.load() || !m_Connected.load() ||
                           !m_NegotiationRequested ||
                           m_ConnectionGeneration.load() != generation;
                })) {
                continue;
            }
            m_NegotiationRequested = false;
            m_Samples.clear();
            const bool firstAttempt = m_NegotiationAttempts++ == 0;
            const std::uint32_t requestId = nextRequestId();
            // Publish pending state before sending: a fast reply may arrive
            // on the event thread before LiSendExtensionMessage returns.
            m_NegotiationPending = true;
            m_PendingRequestId = requestId;
            m_NegotiationDeadline = std::chrono::steady_clock::now() + NegotiationTimeout;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Deck microphone configuration request %u (attempt %u/%u, connection %u)",
                        requestId, m_NegotiationAttempts, NegotiationAttemptLimit, generation);
            stateLock.unlock();
            if (firstAttempt) {
                opus_encoder_ctl(m_Encoder, OPUS_RESET_STATE);
            }

            const bool sent = sendConfiguration(true, generation, requestId);
            stateLock.lock();
            if (!sent && m_Connected.load(std::memory_order_acquire) &&
                    m_ConnectionGeneration.load(std::memory_order_acquire) == generation &&
                    m_PendingRequestId == requestId) {
                m_PendingRequestId = 0;
                scheduleNegotiationRetry("configuration send failed");
            }
            continue;
        }

        if (!m_Connected.load(std::memory_order_acquire) ||
                !m_HostActive.load(std::memory_order_acquire) ||
                m_Samples.size() < FramesPerPacket) {
            continue;
        }

        for (std::size_t index = 0; index < samples.size(); ++index) {
            samples[index] = m_Samples.front();
            m_Samples.pop_front();
        }
        const std::uint32_t generation = m_ConnectionGeneration.load();
        std::uint32_t sequence = m_Sequence++;
        if (sequence == 0) {
            sequence = m_Sequence++;
        }
        stateLock.unlock();

        DeckProtocol::writeLe16(packet.data(), DeckProtocol::DeckMicrophoneFrameSamples);
        const int encodedBytes = opus_encode(
                    m_Encoder,
                    samples.data(),
                    FramesPerPacket,
                    packet.data() + sizeof(std::uint16_t),
                    static_cast<opus_int32>(packet.size() - sizeof(std::uint16_t)));
        if (encodedBytes < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Deck microphone Opus encode failed: %s",
                        opus_strerror(encodedBytes));
            continue;
        }

        std::lock_guard<std::mutex> callLock(m_ConnectionCallLock);
        if (m_Connected.load(std::memory_order_acquire) &&
                m_ConnectionGeneration.load(std::memory_order_acquire) == generation) {
            LiSendExtensionMessage(
                        DeckProtocol::DeckMicrophoneFeature,
                        DeckProtocol::DeckMicrophoneOpus,
                        sequence,
                        packet.data(),
                        static_cast<std::uint16_t>(encodedBytes + sizeof(std::uint16_t)),
                        0);
        }
    }
}
