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
    return EXIT_SUCCESS;
}
