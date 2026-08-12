#include "streaming/session.h"

#include "deckprotocol.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <utility>

namespace {
constexpr std::size_t MaxPendingExtensionMessages = 16;
}

void Session::clExtensionMessage(uint8_t feature, uint8_t opcode,
                                 uint32_t requestId, const uint8_t* payload,
                                 uint16_t payloadLength)
{
    Session* session = s_ActiveSession;
    if (session == nullptr || !session->m_EventLoopRunning.load() ||
            payloadLength > LI_EXTENSION_MAX_PAYLOAD ||
            (payloadLength != 0 && payload == nullptr)) {
        return;
    }

    PendingExtensionMessage message {
        feature,
        opcode,
        requestId,
        {},
    };
    if (payloadLength != 0) {
        message.payload.assign(payload, payload + payloadLength);
    }

    bool wakeSdl = false;
    {
        std::lock_guard<std::mutex> lock(session->m_ExtensionMutex);
        if (session->m_ExtensionMessages.size() == MaxPendingExtensionMessages) {
            session->m_ExtensionMessages.pop_front();
        }
        session->m_ExtensionMessages.emplace_back(std::move(message));
        wakeSdl = !session->m_ExtensionMessageQueued.exchange(true);
    }

    if (wakeSdl) {
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = kSdlCodeExtensionMessage;
        if (SDL_PushEvent(&event) != 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to wake SDL for extension message");
        }
    }
}

bool Session::requestLiveBitrate(int bitrateKbps, quint32* requestId)
{
    if (requestId == nullptr ||
            bitrateKbps < static_cast<int>(DeckProtocol::MinimumBitrateKbps) ||
            bitrateKbps > static_cast<int>(DeckProtocol::MaximumBitrateKbps) ||
            !m_EventLoopRunning.load() || m_RecoveryMode.load()) {
        return false;
    }

    uint32_t id = m_NextExtensionRequestId.fetch_add(1);
    if (id == 0) {
        id = m_NextExtensionRequestId.fetch_add(1);
    }

    const auto payload = DeckProtocol::makeBitrateRequest(
                static_cast<uint32_t>(bitrateKbps));
    if (LiSendExtensionMessage(DeckProtocol::LiveBitrateFeature,
                               DeckProtocol::LiveBitrateSet,
                               id,
                               payload.data(),
                               static_cast<uint16_t>(payload.size()),
                               LI_EXTENSION_MESSAGE_RELIABLE) != 0) {
        return false;
    }

    *requestId = id;
    return true;
}

void Session::processExtensionMessages()
{
    if (!m_ExtensionMessageQueued.load()) {
        return;
    }

    std::deque<PendingExtensionMessage> messages;
    {
        std::lock_guard<std::mutex> lock(m_ExtensionMutex);
        messages.swap(m_ExtensionMessages);
        m_ExtensionMessageQueued.store(false);
    }

    for (const PendingExtensionMessage& message : messages) {
        if (message.feature != DeckProtocol::LiveBitrateFeature ||
                message.opcode != DeckProtocol::LiveBitrateResult ||
                message.requestId == 0) {
            continue;
        }

        DeckProtocol::BitrateResult result {};
        if (!DeckProtocol::parseBitrateResult(message.payload.data(),
                                              message.payload.size(),
                                              &result)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Discarding malformed live bitrate result");
            continue;
        }

        if (result.status == DeckProtocol::BitrateStatus::Applied ||
                result.status == DeckProtocol::BitrateStatus::Clamped) {
            m_DesiredBitrateKbps.store(
                        static_cast<int>(result.appliedBitrateKbps));
            m_StreamConfig.bitrate = static_cast<int>(result.appliedBitrateKbps);
        }

        emit liveBitrateResult(message.requestId,
                               static_cast<int>(result.status),
                               static_cast<int>(result.appliedBitrateKbps));
    }
}
