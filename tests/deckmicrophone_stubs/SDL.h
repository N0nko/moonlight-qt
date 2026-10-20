#pragma once
#include <cstdint>
using Uint8 = std::uint8_t;
using SDL_AudioDeviceID = unsigned;
#define SDLCALL
#define SDL_TRUE 1
#define AUDIO_S16SYS 0x8010
#define SDL_LOG_CATEGORY_AUDIO 4
struct SDL_AudioSpec {
    int freq; unsigned short format; unsigned char channels;
    unsigned short samples;
    void (*callback)(void*, Uint8*, int); void* userdata;
};
const char* SDL_GetError();
SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int);
void SDL_CloseAudioDevice(SDL_AudioDeviceID);
void SDL_LogError(int, const char*, ...);
void SDL_LogWarn(int, const char*, ...);
void SDL_LogInfo(int, const char*, ...);
#define SDL_LOG_CATEGORY_APPLICATION 0
