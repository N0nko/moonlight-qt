#pragma once

// Direct VAAPI -> libplacebo mapping for the Vulkan renderer (Linux).
//
// libplacebo maps a VAAPI frame by deriving a DRM-PRIME frame through FFmpeg,
// which calls vaSyncSurface() (a CPU wait for the decode to finish), exports
// fresh dma-buf fds and imports new Vulkan images on every frame. For a frame
// rendered as soon as it is decoded, that CPU wait is most of the frame's
// on-arrival work (2-3 ms for AV1 on the Steam Deck).
//
// This mapper instead
//  - exports and imports each VA surface once and keeps the textures;
//  - takes the decode's fence from the dma-buf (DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
//    and hands it to libplacebo as a semaphore, so the GPU waits for the decode
//    and the CPU records and submits the render meanwhile;
//  - falls back to vaSyncSurface() whenever that fence cannot be trusted
//    (radeonsi submits decode work from a winsys thread, so right after
//    vaEndPicture() the fence may not be attached to the buffer yet).
//
// MOONLIGHT_PLVK_VAAPI_DIRECT=0 restores libplacebo's own mapping.

#include <libplacebo/colorspace.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct AVFrame;

class PlVkVaapiMapper {
public:
    enum Path : uint8_t {
        PathLibplacebo = 0, // not mapped here (libplacebo's derived mapping)
        PathGpuWait = 1,    // decode fence waited on by the GPU
        PathCpuWait = 2,    // vaSyncSurface() on the CPU (fence not usable)
    };

    struct MapInfo {
        Path path = PathLibplacebo;
        uint64_t cpuWaitNs = 0; // time spent in vaSyncSurface()/spinning for the fence
        uint32_t fenceSeq = 0;  // non-zero: decode completion is being timed
        bool imported = false;  // this frame imported a new surface
    };

    // trackFences: time each decode fence's completion on a helper thread.
    PlVkVaapiMapper(pl_vulkan vulkan, PFN_vkGetDeviceProcAddr getDeviceProcAddr, bool trackFences);
    ~PlVkVaapiMapper();

    bool usable() const { return m_Usable; }
    bool gpuWaitAvailable() const { return m_GpuWait; }

    // Maps a VAAPI frame. frameAgeNs: time since the decoder returned the
    // frame (0 if unknown). Returns false if the caller must fall back.
    bool map(const AVFrame* frame, uint64_t frameAgeNs, pl_frame* out, MapInfo* info);
    // Must follow every successful map() once the frame has been rendered.
    void unmap();

    // CLOCK_MONOTONIC time the fence for fenceSeq signalled, 0 if not (yet) known.
    uint64_t fenceSignalNs(uint32_t fenceSeq);

    static uint64_t nowNs();

private:
    struct Entry {
        const void* framesCtx = nullptr;
        uintptr_t surface = 0;
        int width = 0;
        int height = 0;
        int numObjects = 0;
        std::array<int, 4> fds = {-1, -1, -1, -1};
        int numPlanes = 0;
        std::array<pl_tex, 4> tex = {};
        std::array<int, 4> planeObject = {};
        std::array<VkSemaphore, 4> wait = {};
        pl_bit_encoding bits = {};
        bool held = false;      // the textures are held by us (not by libplacebo)
        bool waitUsed = false;  // the wait semaphores were imported since the last check
        uint64_t holdValue = 0; // m_HoldSem value signalled by the latest hold
        bool broken = false;
    };

    Entry* findOrImport(const AVFrame* frame, MapInfo* info);
    bool importSurface(const AVFrame* frame, Entry& entry);
    void destroyEntry(Entry& entry);
    void flush();
    bool hold(Entry& entry);
    bool waitForPreviousUse(Entry& entry);
    int exportReadFence(int dmabufFd);
    void pollerMain();

    pl_vulkan m_Vulkan;
    bool m_Usable = false;
    bool m_GpuWait = false;
    bool m_SyncFileBroken = false;
    uint32_t m_ImportFailures = 0;

    PFN_vkImportSemaphoreFdKHR fn_vkImportSemaphoreFdKHR = nullptr;
    PFN_vkGetSemaphoreCounterValue fn_vkGetSemaphoreCounterValue = nullptr;
    PFN_vkWaitSemaphores fn_vkWaitSemaphores = nullptr;

    VkSemaphore m_HoldSem = VK_NULL_HANDLE; // timeline
    uint64_t m_HoldValue = 0;

    const void* m_FramesCtx = nullptr;
    std::vector<Entry> m_Entries;
    Entry* m_Current = nullptr;

    // Decode-completion timing
    bool m_TrackFences = false;
    uint32_t m_NextFenceSeq = 0;
    std::mutex m_PollLock;
    std::condition_variable m_PollWake;
    std::deque<std::pair<uint32_t, int>> m_PollQueue;
    struct FenceTime {
        uint32_t seq = 0;
        uint64_t ns = 0;
    };
    std::array<FenceTime, 512> m_FenceTimes = {};
    bool m_PollStop = false;
    std::thread m_Poller;
};
