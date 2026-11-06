#pragma once

#include "renderer.h"
#include "pacer/lowlatencypolicy.h"
#include "pacer/fifodepthpolicy.h"

#ifdef Q_OS_WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <array>
#include <vector>

#ifdef Q_OS_DARWIN
class MetalVulkanTextureFactory {
public:
    MetalVulkanTextureFactory(pl_vulkan vulkan);
    ~MetalVulkanTextureFactory();

    bool mapVideoToolboxToPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    void unmapVideoToolboxFromPlacebo(pl_frame* mappedFrame);

private:
    pl_vulkan m_Vulkan;
    /* CVMetalTextureCacheRef */ void* m_TextureCache = nullptr;
};

// Work around direct-to-display mode sometimes (but not always!) blocking us
// from getting a new drawable while the current one is getting scanned out.
#define PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH 1

// MoltenVK will block for the next drawable when we render the next frame
// rather than inside pl_swapchain_start_frame(), so we will force it to wait
// by rendering some no-op work right after we get the new swapchain frame.
#define PLVK_USE_EARLY_RENDER_TO_WAIT 1

#endif

class FifoTraceWriter;
#if defined(Q_OS_LINUX) && defined(HAVE_LIBVA)
#define PLVK_HAVE_VAAPI_DIRECT_MAP 1
class PlVkVaapiMapper;
#endif

class PlVkRenderer : public IFFmpegRenderer {
    friend class FifoTraceWriter;
public:
    PlVkRenderer(AVHWDeviceType hwDeviceType = AV_HWDEVICE_TYPE_NONE, IFFmpegRenderer *backendRenderer = nullptr);
    virtual ~PlVkRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool testRenderFrame(AVFrame* frame) override;
    virtual void waitToRender() override;
    virtual bool getSourcePresentationSlot(PresentationSlot& slot) override;
    virtual bool isFifoDeadlineEnabled() override;
    virtual FifoPlan planFifoFrame(const uint64_t* readyUs, int count) override;
    virtual void setFramePacingInfo(const FramePacingInfo& info) override;
    virtual bool fifoDepthOnArrival() override;
    virtual bool fifoDepthGate(bool backlog, int queued, bool hostExtra) override;
    virtual void cleanupRenderContext() override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual int getDecoderCapabilities() override;
    virtual bool isPixelFormatSupported(int videoFormat, enum AVPixelFormat pixelFormat) override;
    virtual AVPixelFormat getPreferredPixelFormat(int videoFormat) override;

private:
    static void lockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    static void unlockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    static void overlayUploadComplete(void* opaque);

    void beginRenderTiming();
    void endRenderTiming();

    bool createSwapchain(int depth);
    bool createOverlay(pl_overlay* overlay, SDL_Surface* surface);
    bool mapAvFrameToPlacebo(const AVFrame *frame, pl_frame* mappedFrame, bool allowDirect = false);
    void unmapAvFrameFromPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    bool populateQueues(int videoFormat);
    bool chooseVulkanDevice(PDECODER_PARAMETERS params, bool hdrOutputRequired);
    bool tryInitializeDevice(VkPhysicalDevice device, VkPhysicalDeviceProperties* deviceProps,
                             PDECODER_PARAMETERS decoderParams, bool hdrOutputRequired);
    bool isExtensionSupportedByPhysicalDevice(VkPhysicalDevice device, const char* extensionName);
    bool isPresentModeSupportedByPhysicalDevice(VkPhysicalDevice device, VkPresentModeKHR presentMode);
    bool isColorSpaceSupportedByPhysicalDevice(VkPhysicalDevice device, VkColorSpaceKHR colorSpace);
    bool isSurfacePresentationSupportedByPhysicalDevice(VkPhysicalDevice device);

#ifdef Q_OS_LINUX
    static void presentHook(void* context, VkSwapchainKHR swapchain,
                            VkPresentInfoKHR* presentInfo);
    static uint64_t monotonicTimeNs();
    void preparePresent(VkSwapchainKHR swapchain, VkPresentInfoKHR* presentInfo);
    void collectPresentationFeedback(VkSwapchainKHR swapchain);
    void resetPresentationTiming(VkSwapchainKHR swapchain);
    void logPresentationTiming(uint64_t nowNs);
    uint64_t getPresentLeadTimeNs() const;
    void waitForPresentationBudget();
    const char* getPacingModeName() const;
    void injectPresentId(VkPresentInfoKHR* presentInfo);
    void observeFifoFeedback(const VkPastPresentationTimingGOOGLE& timing);
    void logFifoDeadline();
    static void renderInfoCallback(void* priv, const pl_render_info* info);

    // Per-present FIFO record, completed by display-timing feedback.
    struct FifoRecord {
        uint32_t id = 0;
        uint32_t pts = 0;
        bool hasPts = false;
        bool gateFromFeedback = false;
        bool armed = false;
        bool cadenceOk = false;
        uint8_t action = 0;
        int8_t depth = 1;
        int16_t queueDepth = 0;
        uint16_t drainDrops = 0;
        uint16_t otherDrops = 0;
        uint64_t receiveNs = 0;
        uint64_t readyNs = 0;
        uint64_t renderStartNs = 0;
        uint64_t submitNs = 0;
        uint64_t gateNs = 0;
        uint64_t gateVblankNs = 0;
        uint64_t toVblankNs = 0;
        int64_t frameSlackNs = 0;
        int64_t slackLoNs = 0;
        uint64_t leadNs = 0;
        uint64_t renderNs = 0;
        uint64_t gpuNs = 0;
        uint64_t actualNs = 0;
        uint64_t earliestNs = 0;
        uint64_t desiredNs = 0;
        uint64_t marginNs = 0;
        int32_t displayRefreshes = 0;
        int32_t sourceRefreshes = 0;
        // Queue-depth controller
        uint8_t ctlState = 0;     // FifoDepthState at submit
        bool backlog = false;     // head was queued before the gate
        bool onArrival = false;   // rendered without waiting in the queue
        uint64_t workNs = 0;      // decode-ready to submit + GPU
        int64_t ctlSlackNs = 0;   // slack sample fed to the controller
        int64_t ctlSlackLoNs = 0; // controller's p99.9 slack after this frame
        // On-arrival work breakdown (CPU times on the render thread)
        uint8_t mapPath = 0;       // PlVkVaapiMapper::Path
        uint32_t fenceSeq = 0;     // decode fence timed by the mapper
        uint64_t mapNs = 0;        // map incl. any CPU decode wait
        uint64_t cpuWaitNs = 0;    // part of mapNs spent waiting for the decode
        uint64_t renderCpuNs = 0;  // pl_render_image()
        uint64_t submitCpuNs = 0;  // pl_swapchain_submit_frame() (queue submit + present)
        uint64_t acquireNs = 0;    // pl_swapchain_start_frame() in waitToRender()
        uint64_t submitStartNs = 0;
        uint64_t submitEndNs = 0;
        uint64_t decodeDoneNs = 0; // decode fence signalled (0: unknown)
        uint64_t gpuDoneNs = 0;    // estimated render completion
    };
    void emitFifoRecord(const FifoRecord& record);
#endif

    // The backend renderer if we're frontend-only
    IFFmpegRenderer* m_Backend;
    AVHWDeviceType m_HwDeviceType;

#ifdef Q_OS_DARWIN
    std::unique_ptr<MetalVulkanTextureFactory> m_MetalTextureFactory;
#endif

#ifdef PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH
    int m_DelayedPresents = 0;
    Uint32 m_RenderStartTime = 0;
#endif

    // SDL state
    SDL_Window* m_Window = nullptr;

    // Stream state
    int m_MaxVideoFps;

    // The libplacebo rendering state
    pl_log m_Log = nullptr;
    pl_vk_inst m_PlVkInstance = nullptr;
    VkSurfaceKHR m_VkSurface = VK_NULL_HANDLE;
    int m_SwapchainDepth = 0;
    VkPresentModeKHR m_VkPresentMode;
    pl_vulkan m_Vulkan = nullptr;
    pl_swapchain m_Swapchain = nullptr;
    pl_renderer m_Renderer = nullptr;
    pl_tex m_Textures[PL_MAX_PLANES] = {};
    pl_color_space m_LastColorspace = {};

#ifdef PLVK_USE_EARLY_RENDER_TO_WAIT
    pl_overlay m_EmptyOverlay = {};
    pl_overlay_part m_EmptyOverlayPart = {};
#endif

    // Pending swapchain state shared between waitToRender(), renderFrame(), and cleanupRenderContext()
    pl_swapchain_frame m_SwapchainFrame = {};
    bool m_HasPendingSwapchainFrame = false;

    // Overlay state
    SDL_SpinLock m_OverlayLock = 0;
    struct {
        // The staging overlay state is copied here under the overlay lock in the render thread.
        //
        // These values can be safely read by the render thread outside of the overlay lock,
        // but the copy from stagingOverlay to overlay must only happen under the overlay
        // lock when hasStagingOverlay is true.
        bool hasOverlay;
        pl_overlay overlay;

        // This state is written by the overlay update thread
        //
        // NB: hasStagingOverlay may be false even if there is a staging overlay texture present,
        // because this is how the overlay update path indicates that the overlay is not currently
        // safe for the render thread to read.
        //
        // It is safe for the overlay update thread to write to stagingOverlay outside of the lock,
        // as long as hasStagingOverlay is false.
        bool hasStagingOverlay;
        pl_overlay stagingOverlay;
    } m_Overlays[Overlay::OverlayMax] = {};

    // Device context used for hwaccel decoders
    AVBufferRef* m_HwDeviceCtx = nullptr;

    // Vulkan functions we call directly
    PFN_vkDestroySurfaceKHR fn_vkDestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 fn_vkGetPhysicalDeviceQueueFamilyProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fn_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fn_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkEnumeratePhysicalDevices fn_vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties fn_vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fn_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties fn_vkEnumerateDeviceExtensionProperties = nullptr;

#ifdef Q_OS_LINUX
    StreamingPreferences::PacingMode m_PacingMode = StreamingPreferences::PM_FIFO;
    bool m_PresentationSyncRequested = false;
    bool m_PresentationFeedbackRequested = false;
    bool m_PacingDiagnostics = false;
    bool m_FifoPresentationRequested = false;
    bool m_SourceTimingEnabled = false;
    bool m_SourceRefreshKnown = false;
    int m_SourceDisplayHz = 0;
    uint64_t m_LastSourceSlotNs = 0;
    uint64_t m_SourcePresentNs = 0;
    uint32_t m_TargetLateSlots = 0;
    bool m_DisplayTimingAvailable = false;
    bool m_MissingFeedbackLogged = false;
    int m_PresentLeadOverrideUs = -1;
    bool m_AdaptivePresentLeadEnabled = false;
    AdaptivePresentLead m_AdaptivePresentLead;
    PresentationHistory m_PresentationHistory;
    uint64_t m_PendingDecodeReadyNs = 0;
    std::vector<uint64_t> m_ReadyToSubmitNs;
    std::vector<uint64_t> m_ReadyToPresentNs;
    std::vector<uint64_t> m_SubmitToPresentNs;
    VkSwapchainKHR m_TimingSwapchain = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr fn_vkGetDeviceProcAddr = nullptr;
    PFN_vkGetPastPresentationTimingGOOGLE fn_vkGetPastPresentationTimingGOOGLE = nullptr;
    PFN_vkGetRefreshCycleDurationGOOGLE fn_vkGetRefreshCycleDurationGOOGLE = nullptr;
    PFN_vkWaitForPresentKHR fn_vkWaitForPresentKHR = nullptr;
    int m_MaxPendingPresents = 0;
    uint64_t m_SubmittedPresentId = 0;
    bool m_HasPendingPresentation = false;
    VkPresentIdKHR m_PresentIdInfo = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkPresentTimeGOOGLE m_PresentTime = {};
    VkPresentTimesInfoGOOGLE m_PresentTimesInfo = {
        VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE
    };
    uint32_t m_NextPresentId = 1;
    uint32_t m_LastFeedbackPresentId = 0;
    uint32_t m_PresentsWithoutFeedback = 0;
    uint32_t m_FeedbackPollCountdown = 0;
    uint64_t m_RefreshDurationNs = 0;
    uint64_t m_LastActualPresentTimeNs = 0;
    std::array<uint64_t, 32> m_ClockSamples = {};
    size_t m_ClockSampleCount = 0;
    uint64_t m_TimingWindowStartNs = 0;
    uint32_t m_TimingMissedVblanks = 0;
    uint32_t m_GuardSkippedVblanks = 0;
    std::vector<uint64_t> m_PresentationIntervalsNs;
    std::vector<uint64_t> m_PresentationErrorsNs;
    std::vector<uint64_t> m_SubmissionMarginsNs;

    // FIFO deadline wake. Observation runs whenever the FIFO gate and display
    // timing are available (so a disabled run still logs what it would do);
    // m_FifoDeadlineActive additionally lets planFifoFrame() act.
    bool m_FifoObserve = false;
    bool m_FifoDeadlineActive = false;
    bool m_FifoPeriodLogged = false;
    int m_FifoDisplayHz = 0;
    uint64_t m_FifoPeriodNs = 0;
    FifoDepthPolicy m_FifoPolicy;
    pl_render_params m_RenderParams = {};
    uint64_t m_FrameGpuNs = 0;
    uint64_t m_RenderStartNs = 0;
    uint64_t m_PendingReceiveNs = 0;
    uint32_t m_PendingPts = 0;
    bool m_PendingHasPts = false;
    uint32_t m_LastRenderedPts = 0;
    bool m_HasLastRenderedPts = false;
    FramePacingInfo m_NextPacingInfo;
    uint64_t m_GateNs = 0;
    uint64_t m_GateVblankNs = 0;
    bool m_GateFromFeedback = false;
    uint32_t m_LastSubmittedTimingId = 0;
    std::array<FifoRecord, 256> m_FifoRecords = {};
    uint64_t m_PrevFeedbackActualNs = 0;
    uint32_t m_PrevFeedbackPts = 0;
    bool m_PrevFeedbackValid = false;
    bool m_LastLoggedCadence = false;
    uint32_t m_FifoDrains = 0;
    uint32_t m_FifoAborts = 0;
    uint32_t m_FifoWaits = 0;
    uint32_t m_FifoBacklogFrames = 0;
    uint32_t m_FifoRepeats = 0;
    uint32_t m_FifoSkips = 0;
    uint32_t m_FifoHostGaps = 0;
    uint32_t m_FifoRefreshes = 0;
    // Queue-depth controller (MOONLIGHT_FIFO_DEPTH=0 disables it).
    FifoDepthController m_FifoDepth;
    bool m_FifoDepthAllowed = false;
    uint64_t m_PrevReadyNs = 0;
    uint32_t m_PrevReadyPts = 0;
    uint32_t m_FifoDepthGates[3] = {};
    uint32_t m_FifoDepthBacklogGates = 0;
    uint64_t m_FifoWindowStartNs = 0;
    FifoTraceWriter* m_FifoTrace = nullptr;
    // Per-frame work breakdown
    uint64_t m_LastAcquireNs = 0;
    uint32_t m_RenderedFrames = 0;
    bool m_LogPasses = false;
    struct RenderStats {
        std::vector<uint32_t> map, wait, render, submit, acquire, decode, work, gpu;
        uint32_t paths[3] = {};
        void clear()
        {
            map.clear(); wait.clear(); render.clear(); submit.clear();
            acquire.clear(); decode.clear(); work.clear(); gpu.clear();
            paths[0] = paths[1] = paths[2] = 0;
        }
    } m_RenderStats;
    void logRenderStats();
#endif
#ifdef PLVK_HAVE_VAAPI_DIRECT_MAP
    PlVkVaapiMapper* m_VaapiMapper = nullptr;
    bool m_VaapiMapperTried = false;
    bool m_MappedDirect = false;
    uint8_t m_LastMapPath = 0;
    uint64_t m_LastCpuWaitNs = 0;
    uint32_t m_LastFenceSeq = 0;
#endif
};
