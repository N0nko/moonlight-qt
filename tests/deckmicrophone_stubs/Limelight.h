#pragma once
#include <cstdint>
#define LI_EXTENSION_MAX_PAYLOAD 1024
#define LI_EXTENSION_MESSAGE_RELIABLE 1
int LiSendExtensionMessage(std::uint8_t, std::uint8_t, std::uint32_t, const void*, std::uint16_t, int);
