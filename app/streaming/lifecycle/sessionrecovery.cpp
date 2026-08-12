#include "streaming/session.h"

#include "connectionstartthread.h"
#ifdef HAVE_LINUX_LIFECYCLE
#include "linuxlifecyclemonitor.h"
#endif

#include "SDL_compat.h"

#include <openssl/rand.h>

#define CONN_TEST_SERVER "qt.conntest.moonlight-stream.org"

void Session::regenerateRemoteInputCredentials()
{
    RAND_bytes(reinterpret_cast<unsigned char*>(m_StreamConfig.remoteInputAesKey),
               sizeof(m_StreamConfig.remoteInputAesKey));

    // Sunshine uses the first four bytes as the remote-input key ID.
    SDL_memset(m_StreamConfig.remoteInputAesIv, 0,
               sizeof(m_StreamConfig.remoteInputAesIv));
    RAND_bytes(reinterpret_cast<unsigned char*>(m_StreamConfig.remoteInputAesIv), 4);
}

bool Session::shouldRecoverConnection(int errorCode) const
{
    if (m_Computer->isNvidiaServerSoftware ||
            qEnvironmentVariableIntValue("MOONLIGHT_DISABLE_RECOVERY") != 0 ||
            m_ShouldExit) {
        return false;
    }

    switch (errorCode) {
    case ML_ERROR_GRACEFUL_TERMINATION:
    case ML_ERROR_PROTECTED_CONTENT:
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
    case ML_ERROR_FRAME_CONVERSION:
        return false;
    default:
        return true;
    }
}

void Session::reportConnectionTermination(int errorCode)
{
    unsigned int portFlags = LiGetPortFlagsFromTerminationErrorCode(errorCode);
    if (portFlags != 0) {
        m_PortTestResults = LiTestClientConnectivity(CONN_TEST_SERVER, 443, portFlags);
    }

    switch (errorCode) {
    case ML_ERROR_GRACEFUL_TERMINATION:
        break;

    case ML_ERROR_NO_VIDEO_TRAFFIC: {
        m_UnexpectedTermination = true;

        char ports[128];
        SDL_assert(portFlags != 0);
        LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));
        emit displayLaunchError(tr("No video received from host.") + "\n\n" +
                                tr("Check your firewall and port forwarding rules for port(s): %1").arg(ports));
        break;
    }

    case ML_ERROR_NO_VIDEO_FRAME:
        m_UnexpectedTermination = true;
        emit displayLaunchError(tr("Your network connection isn't performing well. Reduce your video bitrate setting or try a faster connection."));
        break;

    case ML_ERROR_VIDEO_STALL:
        m_UnexpectedTermination = true;
        emit displayLaunchError(tr("The video stream stalled and automatic recovery could not restore it."));
        break;

    case ML_ERROR_PROTECTED_CONTENT:
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        m_UnexpectedTermination = true;
        emit displayLaunchError(tr("Something went wrong on your host PC when starting the stream.") + "\n\n" +
                                tr("Make sure you don't have any DRM-protected content open on your host PC. You can also try restarting your host PC."));
        break;

    case ML_ERROR_FRAME_CONVERSION:
        m_UnexpectedTermination = true;
        emit displayLaunchError(tr("The host PC reported a fatal video encoding error.") + "\n\n" +
                                tr("Try disabling HDR mode, changing the streaming resolution, or changing your host PC's display resolution."));
        break;

    default: {
        m_UnexpectedTermination = true;
        const bool hexError = qAbs(errorCode) > 1000;
        emit displayLaunchError(tr("Connection terminated") + "\n\n" +
                                tr("Error code: %1").arg(errorCode,
                                                        hexError ? 8 : 0,
                                                        hexError ? 16 : 10,
                                                        QChar('0')));
        break;
    }
    }
}

void Session::stopConnectionForRecovery()
{
    // Release remote state and prevent timers or SDL events from writing into
    // common-c while its global connection generation is being replaced.
    m_InputHandler->setInputForwardingEnabled(false);

    // Pull-based renderers must be gone before common-c tears down its queues.
    destroyVideoDecoder();
    LiStopConnection();

    m_AudioSampleCount = 0;
    m_DropAudioEndTime = 0;
}

void Session::startRecoveryAttempt()
{
    SDL_assert(m_RecoveryThread == nullptr);

    m_RecoveryGamepadMask = m_InputHandler->getAttachedGamepadMask();
    regenerateRemoteInputCredentials();
    m_AsyncConnectionSuccess.store(false);
    m_AttemptTerminated.store(false);
    m_ConnectionLossQueued.store(false);
    m_FramePresentedQueued.store(false);

    // A host may reject or outlive a stale warm session. Bound the optimized
    // path, then use ordinary Sunshine resume for the remaining retries.
    const bool fastResume = m_RecoveryPolicy.attempts() <= 2;
    m_RecoveryThread = new AsyncConnectionStartThread(this, true, fastResume);
    m_RecoveryThread->start();
}

void Session::finishRecovery()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Stream recovery completed after the first presented frame");

    m_RecoveryMode.store(false);
    m_InputHandler->setInputForwardingEnabled(true);
    m_UnexpectedTermination = false;
    m_LastConnectionError.store(0);
    m_AttemptTerminated.store(false);
    m_ConnectionLossQueued.store(false);
    m_PortTestResults = 0;

    if (m_MouseEmulationRefCount > 0) {
        m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                           "Gamepad mouse mode active\nLong press Start to deactivate");
    }
    else {
        m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
    }

    m_InputHandler->updatePointerRegionLock();
}

#ifdef HAVE_LINUX_LIFECYCLE
void Session::queueLifecycleSleepState(bool sleeping)
{
    if (!m_EventLoopRunning.load()) {
        return;
    }

    std::atomic_bool& queued = sleeping ? m_LifecycleSleepQueued : m_LifecycleWakeQueued;
    if (!queued.exchange(true)) {
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = kSdlCodeLifecycleStateChanged;
        if (SDL_PushEvent(&event) != 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to wake SDL for lifecycle transition");
        }
    }
}

void Session::queueLifecycleNetworkState(bool available)
{
    if (!m_EventLoopRunning.load()) {
        return;
    }

    m_NetworkAvailable.store(available);
    if (!m_NetworkStateQueued.exchange(true)) {
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = kSdlCodeLifecycleStateChanged;
        if (SDL_PushEvent(&event) != 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to wake SDL for network transition");
        }
    }
}

void Session::suspendConnectionForLifecycle()
{
    m_RecoveryMode.store(true);
    m_RecoveryPolicy.suspend();

    if (m_RecoveryThread != nullptr) {
        LiInterruptConnection();
        m_RecoveryThread->wait();
        delete m_RecoveryThread;
        m_RecoveryThread = nullptr;
    }

    stopConnectionForRecovery();
    m_ConnectionLossQueued.store(false);
    m_FramePresentedQueued.store(false);
    m_AttemptTerminated.store(false);
    m_LastConnectionError.store(0);
}

bool Session::resumeConnectionForLifecycle()
{
    m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                       "Reconnecting to PC...");
    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
    return applyRecoveryAction(m_RecoveryPolicy.begin(SDL_GetTicks(), 0));
}

bool Session::processLifecycleState()
{
    if (m_LifecycleSleepQueued.exchange(false)) {
        if (!m_LifecycleSuspended.exchange(true)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Quiescing the stream before system sleep");
            if (!m_NetworkUnavailable.load()) {
                suspendConnectionForLifecycle();
            }
        }

        if (m_LinuxLifecycleMonitor != nullptr) {
            m_LinuxLifecycleMonitor->acknowledgeSleepReady();
        }
    }

    if (m_NetworkStateQueued.exchange(false)) {
        const bool available = m_NetworkAvailable.load();
        if (!available) {
            if (!m_NetworkUnavailable.exchange(true) &&
                    !m_LifecycleSuspended.load()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Network unavailable; pausing stream recovery");
                suspendConnectionForLifecycle();
                m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                   "Waiting for network...");
                m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
            }
        }
        else if (m_NetworkUnavailable.exchange(false) &&
                 !m_LifecycleSuspended.load()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Network available; resuming in the existing stream window");
            if (!resumeConnectionForLifecycle()) {
                return false;
            }
        }
    }

    if (m_LifecycleWakeQueued.exchange(false)) {
        if (m_LifecycleSuspended.exchange(false)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "System wake received in the existing stream window");

            if (m_NetworkUnavailable.load()) {
                m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                   "Waiting for network...");
                m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
            }
            else if (!resumeConnectionForLifecycle()) {
                return false;
            }
        }
    }

    return true;
}
#endif

bool Session::applyRecoveryAction(RecoveryPolicy::Action action)
{
    switch (action) {
    case RecoveryPolicy::Action::None:
        return true;

    case RecoveryPolicy::Action::StartConnection:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Starting recovery attempt %u",
                    m_RecoveryPolicy.attempts());
        startRecoveryAttempt();
        return true;

    case RecoveryPolicy::Action::AbortConnection:
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Recovery phase timed out");
        if (m_RecoveryThread != nullptr) {
            LiInterruptConnection();
            return true;
        }

        stopConnectionForRecovery();
        return applyRecoveryAction(
                    m_RecoveryPolicy.connectionFailed(SDL_GetTicks(),
                                                      m_LastConnectionError.load()));

    case RecoveryPolicy::Action::StreamReady:
        finishRecovery();
        return true;

    case RecoveryPolicy::Action::GiveUp: {
        m_RecoveryMode.store(false);
        m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);

        int errorCode = m_RecoveryPolicy.lastError();
        if (errorCode == 0) {
            errorCode = m_LastConnectionError.load();
        }
        if (errorCode == 0) {
            errorCode = ML_ERROR_VIDEO_STALL;
        }

        reportConnectionTermination(errorCode);
        return false;
    }
    }

    SDL_assert(false);
    return false;
}

bool Session::processRecoveryEvents()
{
#ifdef HAVE_LINUX_LIFECYCLE
    if (!processLifecycleState()) {
        return false;
    }

    if (m_LifecycleSuspended.load() || m_NetworkUnavailable.load()) {
        return true;
    }
#endif

    if (m_ConnectionLossQueued.exchange(false)) {
        const int errorCode = m_LastConnectionError.load();
        if (!shouldRecoverConnection(errorCode)) {
            reportConnectionTermination(errorCode);
            return false;
        }

        switch (m_RecoveryPolicy.state()) {
        case RecoveryPolicy::State::Streaming:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Recovering the stream in the existing window after error %d",
                        errorCode);
            m_RecoveryMode.store(true);
            stopConnectionForRecovery();
            m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                               "Reconnecting to PC...");
            m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
            if (!applyRecoveryAction(m_RecoveryPolicy.begin(SDL_GetTicks(), errorCode))) {
                return false;
            }
            break;

        case RecoveryPolicy::State::Connecting:
            // Wait for LiStartConnection() to return before resetting common-c.
            if (m_RecoveryThread != nullptr && !m_RecoveryThread->isFinished()) {
                LiInterruptConnection();
            }
            break;

        case RecoveryPolicy::State::AwaitingFrame:
            stopConnectionForRecovery();
            if (!applyRecoveryAction(
                        m_RecoveryPolicy.connectionFailed(SDL_GetTicks(), errorCode))) {
                return false;
            }
            break;

        default:
            // A callback from the generation being torn down is stale.
            break;
        }
    }

    if (m_RecoveryThread != nullptr && m_RecoveryThread->isFinished()) {
        m_RecoveryThread->wait();
        delete m_RecoveryThread;
        m_RecoveryThread = nullptr;

        const bool connectionStarted =
                m_RecoveryPolicy.state() == RecoveryPolicy::State::Connecting &&
                m_AsyncConnectionSuccess.load() &&
                !m_AttemptTerminated.load();
        if (!connectionStarted) {
            stopConnectionForRecovery();
            if (!applyRecoveryAction(
                        m_RecoveryPolicy.connectionFailed(SDL_GetTicks(),
                                                          m_LastConnectionError.load()))) {
                return false;
            }
        }
        else {
            m_FramePresentedQueued.store(false);
            if (!applyRecoveryAction(
                        m_RecoveryPolicy.connectionStarted(SDL_GetTicks()))) {
                return false;
            }

            if (!recreateVideoDecoder(false, Session::drFramePresented, this)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to recreate the decoder during recovery");
                stopConnectionForRecovery();
                reportConnectionTermination(ML_ERROR_FRAME_CONVERSION);
                return false;
            }

            // Decoder destruction detaches the old overlay renderer. Replay
            // the text after recreation so the new renderer receives it.
            m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                               "Reconnecting to PC...");
        }
    }

    if (m_FramePresentedQueued.exchange(false)) {
        if (!applyRecoveryAction(m_RecoveryPolicy.framePresented())) {
            return false;
        }
    }

    if (m_RecoveryMode.load()) {
        return applyRecoveryAction(m_RecoveryPolicy.tick(SDL_GetTicks()));
    }

    return true;
}
