#pragma once

#include "../../decoder.h"
#include "../renderer.h"
#include "sourcetimeline.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>
#include <vector>
#include <atomic>

// The maximum number of frames pacer will ever hold is:
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
#define PACER_MAX_OUTSTANDING_FRAMES (3 + 1 + 1)

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual void waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats,
          DecoderFramePresentedCallback framePresentedCallback,
          void* framePresentedContext);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing,
                    bool enableFrameReserve, bool pacingDiagnostics,
                    bool enableSourceTiming, StreamingPreferences::PacingMode pacingMode);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    int renderReserveFrames() const;

    int pacingReserveFrames() const;

    bool hasRenderableFrameLocked(int queueDepth, int reserveFrames);

    void recordReserveUseLocked(int queueDepth, int reserveFrames);

    void recordQueueDepthLocked();

    void logPacingDiagnostics();
    AVFrame* takeSourceFrameLocked();

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    std::atomic_bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    bool m_FrameReserveEnabled;
    bool m_FrameReservePrimed;
    bool m_PacingDiagnostics;
    bool m_LatestFrameEnabled = false;
    std::atomic_bool m_SourceTimingEnabled{false};
    bool m_SourceTimingActive = false;
    SourceTimeline m_SourceTimeline;
    uint32_t m_SourceHolds = 0;
    uint32_t m_SourceResets = 0;
    uint32_t m_SourceDeadlineMisses = 0;
    uint64_t m_SourceMaxQueueAgeUs = 0;
    uint64_t m_SourceReserveUs = 0;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;
    DecoderFramePresentedCallback m_FramePresentedCallback;
    void* m_FramePresentedContext;

    uint64_t m_DiagnosticWindowStartUs;
    uint32_t m_DiagnosticQueueWaits;
    uint32_t m_DiagnosticReserveUses;
    uint32_t m_DiagnosticDroppedFrames;
    int m_DiagnosticMinQueueDepth;
    int m_DiagnosticMaxQueueDepth;
    std::vector<uint64_t> m_DiagnosticQueueWaitUs;
};
