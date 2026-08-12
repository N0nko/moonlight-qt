#include "../app/streaming/extensions/deckprotocol.h"

#include <cstdlib>
#include <iostream>

namespace {
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)
}

int main()
{
    const auto request = DeckProtocol::makeBitrateRequest(300000);
    CHECK(DeckProtocol::readLe32(request.data()) == 300000);

    std::array<std::uint8_t, 8> response {};
    response[0] = static_cast<std::uint8_t>(DeckProtocol::BitrateStatus::Clamped);
    DeckProtocol::writeLe32(response.data() + 4, 40000);

    DeckProtocol::BitrateResult result {};
    CHECK(DeckProtocol::parseBitrateResult(response.data(), response.size(), &result));
    CHECK(result.status == DeckProtocol::BitrateStatus::Clamped);
    CHECK(result.appliedBitrateKbps == 40000);

    response[1] = 1;
    CHECK(!DeckProtocol::parseBitrateResult(response.data(), response.size(), &result));

    const auto displayRequest = DeckProtocol::makeDisplayApply(
                DeckProtocol::DisplayProfile::Remote);
    CHECK(displayRequest[0] == static_cast<std::uint8_t>(
              DeckProtocol::DisplayProfile::Remote));

    const auto displayPolicy = DeckProtocol::makeDisplayPolicy(true, false);
    CHECK(displayPolicy[0] == 0x01);

    std::array<std::uint8_t, 4> displayResponse {};
    displayResponse[0] = static_cast<std::uint8_t>(
                DeckProtocol::DisplayStatus::Applied);
    displayResponse[1] = static_cast<std::uint8_t>(
                DeckProtocol::DisplayProfile::Tv);
    DeckProtocol::DisplayResult displayResult {};
    CHECK(DeckProtocol::parseDisplayResult(displayResponse.data(),
                                           displayResponse.size(),
                                           &displayResult));
    CHECK(displayResult.status == DeckProtocol::DisplayStatus::Applied);
    CHECK(displayResult.profile == DeckProtocol::DisplayProfile::Tv);
    displayResponse[1] = 4;
    CHECK(!DeckProtocol::parseDisplayResult(displayResponse.data(),
                                            displayResponse.size(),
                                            &displayResult));
    return EXIT_SUCCESS;
}
