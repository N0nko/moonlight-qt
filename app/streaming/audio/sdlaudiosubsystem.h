#pragma once

#include "SDL_compat.h"

#include <mutex>

namespace SdlAudioSubsystem {
struct State {
    std::mutex mutex;
    unsigned int references = 0;
    bool owned = false;
};

inline State& state()
{
    static State sharedState;
    return sharedState;
}

inline bool acquire()
{
    State& sharedState = state();
    std::lock_guard<std::mutex> lock(sharedState.mutex);
    if (sharedState.references == 0) {
        sharedState.owned = (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0;
        if (sharedState.owned && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            sharedState.owned = false;
            return false;
        }
    }

    ++sharedState.references;
    return true;
}

inline void release()
{
    State& sharedState = state();
    std::lock_guard<std::mutex> lock(sharedState.mutex);
    SDL_assert(sharedState.references != 0);
    if (sharedState.references == 0) {
        return;
    }

    if (--sharedState.references == 0 && sharedState.owned) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sharedState.owned = false;
    }
}
} // namespace SdlAudioSubsystem
