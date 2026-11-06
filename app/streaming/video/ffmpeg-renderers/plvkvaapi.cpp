#include "plvkvaapi.h"

#include <SDL.h>

extern "C" {
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
}

// Implementation in plvk_c.c
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <va/va.h>
#include <va/va_drmcommon.h>

#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

// From <linux/dma-buf.h> (Linux 6.0). Defined here so older kernel headers build.
struct PlVkDmaBufExportSyncFile {
    uint32_t flags;
    int32_t fd;
};
#define PLVK_DMA_BUF_SYNC_READ (1 << 0)
#define PLVK_DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct PlVkDmaBufExportSyncFile)

namespace {

// A frame younger than this may still be on its way to the kernel (radeonsi's
// winsys thread), so a signalled fence does not prove the decode finished.
constexpr uint64_t kSubmitRaceNs = 1500000;
constexpr uint64_t kCpuSyncAgeNs = 4000000;
constexpr uint64_t kSpinStepNs = 50000;

bool fenceSignalled(int fd)
{
    pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

void sleepNs(uint64_t ns)
{
    timespec ts = {static_cast<time_t>(ns / 1000000000ull), static_cast<long>(ns % 1000000000ull)};
    nanosleep(&ts, nullptr);
}

} // namespace

uint64_t PlVkVaapiMapper::nowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

PlVkVaapiMapper::PlVkVaapiMapper(pl_vulkan vulkan, PFN_vkGetDeviceProcAddr getDeviceProcAddr, bool trackFences)
    : m_Vulkan(vulkan)
{
    if (vulkan == nullptr || getDeviceProcAddr == nullptr ||
        !(vulkan->gpu->import_caps.tex & PL_HANDLE_DMA_BUF)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "VAAPI direct map: unavailable (no dma-buf import)");
        return;
    }

    bool semaphoreFd = false;
    for (int i = 0; i < vulkan->num_extensions; i++) {
        if (strcmp(vulkan->extensions[i], VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0) {
            semaphoreFd = true;
        }
    }
    fn_vkGetSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
        getDeviceProcAddr(vulkan->device, "vkGetSemaphoreCounterValue"));
    fn_vkWaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(
        getDeviceProcAddr(vulkan->device, "vkWaitSemaphores"));
    if (semaphoreFd) {
        fn_vkImportSemaphoreFdKHR = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            getDeviceProcAddr(vulkan->device, "vkImportSemaphoreFdKHR"));
    }
    if (fn_vkGetSemaphoreCounterValue == nullptr || fn_vkWaitSemaphores == nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "VAAPI direct map: unavailable (no timeline semaphores)");
        return;
    }

    pl_vulkan_sem_params semParams = {};
    semParams.type = VK_SEMAPHORE_TYPE_TIMELINE;
    semParams.debug_tag = PL_DEBUG_TAG;
    semParams.initial_value = 0;
    m_HoldSem = pl_vulkan_sem_create(vulkan->gpu, &semParams);
    if (m_HoldSem == VK_NULL_HANDLE) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "VAAPI direct map: unavailable (semaphore)");
        return;
    }

    m_Usable = true;
    m_GpuWait = fn_vkImportSemaphoreFdKHR != nullptr;
    m_TrackFences = trackFences && m_GpuWait;
    m_Entries.reserve(64);
    if (m_TrackFences) {
        m_Poller = std::thread([this] { pollerMain(); });
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "VAAPI direct map: cached surface import, decode wait on the %s%s",
                m_GpuWait ? "GPU (dma-buf sync_file)" : "CPU (no VK_KHR_external_semaphore_fd)",
                m_TrackFences ? ", decode completion timed" : "");
}

PlVkVaapiMapper::~PlVkVaapiMapper()
{
    if (m_Poller.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_PollLock);
            m_PollStop = true;
        }
        m_PollWake.notify_one();
        m_Poller.join();
    }
    for (auto& item : m_PollQueue) {
        close(item.second);
    }
    flush();
    if (m_HoldSem != VK_NULL_HANDLE) {
        pl_vulkan_sem_destroy(m_Vulkan->gpu, &m_HoldSem);
    }
}

void PlVkVaapiMapper::destroyEntry(Entry& entry)
{
    for (int n = 0; n < 4; n++) {
        if (entry.tex[n] != nullptr) {
            pl_tex_destroy(m_Vulkan->gpu, &entry.tex[n]);
        }
        if (entry.wait[n] != VK_NULL_HANDLE) {
            pl_vulkan_sem_destroy(m_Vulkan->gpu, &entry.wait[n]);
        }
        if (entry.fds[n] >= 0) {
            close(entry.fds[n]);
            entry.fds[n] = -1;
        }
    }
}

void PlVkVaapiMapper::flush()
{
    if (m_Entries.empty()) {
        return;
    }
    // Textures and semaphores may still be in use by queued GPU work.
    pl_gpu_finish(m_Vulkan->gpu);
    for (Entry& entry : m_Entries) {
        destroyEntry(entry);
    }
    m_Entries.clear();
    m_Current = nullptr;
}

bool PlVkVaapiMapper::importSurface(const AVFrame* frame, Entry& entry)
{
    auto hwfc = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    auto vaDevice = reinterpret_cast<const AVVAAPIDeviceContext*>(hwfc->device_ctx->hwctx);
    const VASurfaceID surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3]));
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(hwfc->sw_format);
    if (desc == nullptr) {
        return false;
    }

    VADRMPRIMESurfaceDescriptor vaDesc = {};
    VAStatus st = vaExportSurfaceHandle(vaDevice->display, surface,
                                        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                        &vaDesc);
    if (st != VA_STATUS_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "VAAPI direct map: vaExportSurfaceHandle() failed: %d", st);
        return false;
    }

    entry.framesCtx = hwfc;
    entry.surface = reinterpret_cast<uintptr_t>(frame->data[3]);
    entry.width = frame->width;
    entry.height = frame->height;
    entry.numObjects = std::min<int>(vaDesc.num_objects, 4);
    for (uint32_t i = 0; i < vaDesc.num_objects; i++) {
        if (i < 4) {
            entry.fds[i] = vaDesc.objects[i].fd;
        }
        else {
            close(vaDesc.objects[i].fd);
        }
    }

    const int planes = av_pix_fmt_count_planes(hwfc->sw_format);
    if (planes <= 0 || planes > 4 || static_cast<int>(vaDesc.num_layers) < planes) {
        return false;
    }

    // Bit encoding as libplacebo's pl_map_hwframe_bit_encoding() derives it.
    pl_bit_encoding bits = {};
    for (int c = 0; c < std::min<int>(desc->nb_components, 3); c++) {
        const AVComponentDescriptor& comp = desc->comp[c];
        pl_bit_encoding cbits = {};
        cbits.sample_depth = comp.depth + std::abs(comp.shift);
        cbits.color_depth = comp.depth;
        cbits.bit_shift = std::max(comp.shift, 0);
        if (bits.sample_depth != 0 && !pl_bit_encoding_equal(&bits, &cbits)) {
            bits = {};
            break;
        }
        bits = cbits;
    }

    entry.numPlanes = planes;
    for (int n = 0; n < planes; n++) {
        const auto& layer = vaDesc.layers[n];
        if (layer.num_planes != 1 || layer.object_index[0] >= vaDesc.num_objects || layer.object_index[0] >= 4) {
            return false;
        }
        const auto& object = vaDesc.objects[layer.object_index[0]];
        pl_fmt fmt = pl_find_fourcc(m_Vulkan->gpu, layer.drm_format);
        if (fmt == nullptr || !pl_fmt_has_modifier(fmt, object.drm_format_modifier)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "VAAPI direct map: unsupported plane format 0x%08x / modifier 0x%llx",
                        layer.drm_format, (unsigned long long)object.drm_format_modifier);
            return false;
        }
        const bool chroma = n == 1 || n == 2;
        pl_tex_params params = {};
        params.w = AV_CEIL_RSHIFT(frame->width, chroma ? desc->log2_chroma_w : 0);
        params.h = AV_CEIL_RSHIFT(frame->height, chroma ? desc->log2_chroma_h : 0);
        params.format = fmt;
        params.sampleable = true;
        params.blit_src = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
        params.import_handle = PL_HANDLE_DMA_BUF;
        params.debug_tag = PL_DEBUG_TAG;
        params.shared_mem.handle.fd = object.fd; // libplacebo dup()s it
        params.shared_mem.size = object.size;
        params.shared_mem.offset = layer.offset[0];
        params.shared_mem.drm_format_mod = object.drm_format_modifier;
        params.shared_mem.stride_w = layer.pitch[0];
        entry.tex[n] = pl_tex_create(m_Vulkan->gpu, &params);
        if (entry.tex[n] == nullptr) {
            return false;
        }
        entry.planeObject[n] = static_cast<int>(layer.object_index[0]);
        if (m_GpuWait) {
            pl_vulkan_sem_params semParams = {};
            semParams.type = VK_SEMAPHORE_TYPE_BINARY;
            semParams.debug_tag = PL_DEBUG_TAG;
            entry.wait[n] = pl_vulkan_sem_create(m_Vulkan->gpu, &semParams);
            if (entry.wait[n] == VK_NULL_HANDLE) {
                return false;
            }
        }
        if (n == 0) {
            bits.sample_depth = fmt->component_depth[0]; // pl_fix_hwframe_sample_depth()
        }
    }
    entry.bits = bits;
    // Take the fresh textures over so that every frame can hand them back
    // to libplacebo together with a decode semaphore.
    return hold(entry);
}

PlVkVaapiMapper::Entry* PlVkVaapiMapper::findOrImport(const AVFrame* frame, MapInfo* info)
{
    const void* framesCtx = frame->hw_frames_ctx->data;
    if (framesCtx != m_FramesCtx) {
        flush();
        m_FramesCtx = framesCtx;
    }
    const uintptr_t surface = reinterpret_cast<uintptr_t>(frame->data[3]);
    for (Entry& entry : m_Entries) {
        if (entry.surface == surface && entry.framesCtx == framesCtx && !entry.broken) {
            if (entry.width == frame->width && entry.height == frame->height) {
                return &entry;
            }
            entry.broken = true; // re-import below
        }
    }

    // Drop broken entries (finishing the GPU first) before importing again.
    if (std::any_of(m_Entries.begin(), m_Entries.end(), [](const Entry& e) { return e.broken; })) {
        pl_gpu_finish(m_Vulkan->gpu);
        for (Entry& entry : m_Entries) {
            if (entry.broken) {
                destroyEntry(entry);
            }
        }
        m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                       [](const Entry& e) { return e.broken; }),
                        m_Entries.end());
    }
    if (m_Entries.size() >= 64) {
        flush(); // a decoder that keeps allocating surfaces: start over
    }

    m_Entries.emplace_back();
    Entry& entry = m_Entries.back();
    if (!importSurface(frame, entry)) {
        destroyEntry(entry);
        m_Entries.pop_back();
        if (++m_ImportFailures >= 3) {
            m_Usable = false;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "VAAPI direct map: disabled after import failures");
        }
        return nullptr;
    }
    info->imported = true;
    return &entry;
}

bool PlVkVaapiMapper::hold(Entry& entry)
{
    for (int n = 0; n < entry.numPlanes; n++) {
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        pl_vulkan_hold_params params = {};
        params.tex = entry.tex[n];
        params.out_layout = &layout;
        params.qf = VK_QUEUE_FAMILY_IGNORED;
        params.semaphore.sem = m_HoldSem;
        params.semaphore.value = ++m_HoldValue;
        if (!pl_vulkan_hold_ex(m_Vulkan->gpu, &params)) {
            entry.broken = true;
            entry.held = false;
            return false;
        }
    }
    entry.held = true;
    entry.holdValue = m_HoldValue;
    return true;
}

bool PlVkVaapiMapper::waitForPreviousUse(Entry& entry)
{
    // The binary semaphores may only be re-imported once the render that
    // waited on them has executed; the hold after that render signals it.
    uint64_t value = 0;
    if (fn_vkGetSemaphoreCounterValue(m_Vulkan->device, m_HoldSem, &value) == VK_SUCCESS &&
        value >= entry.holdValue) {
        return true;
    }
    VkSemaphoreWaitInfo waitInfo = {};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &m_HoldSem;
    waitInfo.pValues = &entry.holdValue;
    return fn_vkWaitSemaphores(m_Vulkan->device, &waitInfo, 100000000ull) == VK_SUCCESS;
}

int PlVkVaapiMapper::exportReadFence(int dmabufFd)
{
    if (m_SyncFileBroken) {
        return -1;
    }
    PlVkDmaBufExportSyncFile arg = {PLVK_DMA_BUF_SYNC_READ, -1};
    if (ioctl(dmabufFd, PLVK_DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg) != 0) {
        if (errno == ENOTTY || errno == EINVAL) {
            m_SyncFileBroken = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "VAAPI direct map: DMA_BUF_IOCTL_EXPORT_SYNC_FILE unsupported (%d); decode wait on the CPU",
                        errno);
        }
        return -1;
    }
    return arg.fd;
}

bool PlVkVaapiMapper::map(const AVFrame* frame, uint64_t frameAgeNs, pl_frame* out, MapInfo* info)
{
    *info = MapInfo();
    if (!m_Usable || frame == nullptr || frame->hw_frames_ctx == nullptr) {
        return false;
    }
    if (m_Current != nullptr) {
        unmap(); // a previous map() was never unmapped
    }

    Entry* entry = findOrImport(frame, info);
    if (entry == nullptr || !entry->held) {
        return false;
    }
    if (entry->waitUsed) {
        if (!waitForPreviousUse(*entry)) {
            entry->broken = true;
            return false;
        }
        entry->waitUsed = false;
    }

    pl_frame_from_avframe(out, frame);
    if (out->num_planes != entry->numPlanes) {
        entry->broken = true;
        return false;
    }
    for (int n = 0; n < entry->numPlanes; n++) {
        out->planes[n].texture = entry->tex[n];
    }
    out->repr.bits = entry->bits;
    out->user_data = nullptr;

    // Find the decode's fence. It is only trustworthy while still pending:
    // a signalled fence on a young frame may mean the job is not submitted yet.
    const uint64_t startNs = nowNs();
    std::array<int, 4> fence = {-1, -1, -1, -1}; // per object
    bool pending = false;
    // An older frame's decode has finished (vaSyncSurface() returns at once),
    // and a pending fence on it may belong to a later decode reading it as a
    // reference, which the render must not wait for.
    if (m_GpuWait && frameAgeNs != 0 && frameAgeNs < kCpuSyncAgeNs) {
        uint64_t ageNs = frameAgeNs;
        for (;;) {
            bool any = false;
            pending = false;
            for (int n = 0; n < entry->numPlanes; n++) {
                const int obj = entry->planeObject[n];
                if (fence[obj] < 0) {
                    fence[obj] = exportReadFence(entry->fds[obj]);
                }
                if (fence[obj] >= 0) {
                    any = true;
                    pending |= !fenceSignalled(fence[obj]);
                }
            }
            if (!any || pending || ageNs >= kSubmitRaceNs) {
                break;
            }
            // Young frame, no pending fence: the decode may not be in the
            // kernel yet. Look again shortly.
            for (int& fd : fence) {
                if (fd >= 0) {
                    close(fd);
                    fd = -1;
                }
            }
            sleepNs(kSpinStepNs);
            ageNs = frameAgeNs + (nowNs() - startNs);
        }
    }

    bool gpuWait = false;
    if (pending) {
        gpuWait = true;
        entry->waitUsed = true;
        for (int n = 0; n < entry->numPlanes && gpuWait; n++) {
            const int obj = entry->planeObject[n];
            const int fd = dup(fence[obj]);
            if (fd < 0) {
                gpuWait = false;
                break;
            }
            VkImportSemaphoreFdInfoKHR importInfo = {};
            importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
            importInfo.semaphore = entry->wait[n];
            importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            importInfo.fd = fd;
            if (fn_vkImportSemaphoreFdKHR(m_Vulkan->device, &importInfo) != VK_SUCCESS) {
                close(fd);
                gpuWait = false;
                // Semaphores already imported for earlier planes still carry
                // a payload; this entry cannot be reused safely.
                entry->broken = true;
            }
        }
        if (!gpuWait && entry->broken) {
            for (int& fd : fence) {
                if (fd >= 0) {
                    close(fd);
                }
            }
            return false;
        }
    }

    if (!gpuWait) {
        auto hwfc = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
        auto vaDevice = reinterpret_cast<const AVVAAPIDeviceContext*>(hwfc->device_ctx->hwctx);
        const VAStatus st = vaSyncSurface(vaDevice->display,
                                          static_cast<VASurfaceID>(entry->surface));
        if (st != VA_STATUS_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "VAAPI direct map: vaSyncSurface() failed: %d", st);
        }
    }
    info->path = gpuWait ? PathGpuWait : PathCpuWait;
    info->cpuWaitNs = nowNs() - startNs;

    // Time the decode completion off the render thread.
    int trackFd = -1;
    for (int& fd : fence) {
        if (fd >= 0) {
            if (trackFd < 0 && gpuWait && m_TrackFences) {
                trackFd = fd;
            }
            else {
                close(fd);
            }
            fd = -1;
        }
    }
    if (trackFd >= 0) {
        if (++m_NextFenceSeq == 0) {
            m_NextFenceSeq = 1;
        }
        info->fenceSeq = m_NextFenceSeq;
        {
            std::lock_guard<std::mutex> lock(m_PollLock);
            m_PollQueue.emplace_back(m_NextFenceSeq, trackFd);
        }
        m_PollWake.notify_one();
    }

    for (int n = 0; n < entry->numPlanes; n++) {
        pl_vulkan_release_params params = {};
        params.tex = entry->tex[n];
        // The decoder rewrote the image: same transition as a fresh import.
        params.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        params.qf = VK_QUEUE_FAMILY_IGNORED;
        if (gpuWait) {
            params.semaphore.sem = entry->wait[n];
            params.semaphore.value = 0;
        }
        pl_vulkan_release_ex(m_Vulkan->gpu, &params);
    }
    entry->held = false;
    m_Current = entry;
    return true;
}

void PlVkVaapiMapper::unmap()
{
    if (m_Current == nullptr) {
        return;
    }
    Entry* entry = m_Current;
    m_Current = nullptr;
    hold(*entry);
}

uint64_t PlVkVaapiMapper::fenceSignalNs(uint32_t fenceSeq)
{
    if (fenceSeq == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_PollLock);
    const FenceTime& t = m_FenceTimes[fenceSeq % m_FenceTimes.size()];
    return t.seq == fenceSeq ? t.ns : 0;
}

void PlVkVaapiMapper::pollerMain()
{
    std::unique_lock<std::mutex> lock(m_PollLock);
    for (;;) {
        m_PollWake.wait(lock, [this] { return m_PollStop || !m_PollQueue.empty(); });
        if (m_PollStop) {
            return;
        }
        const std::pair<uint32_t, int> item = m_PollQueue.front();
        m_PollQueue.pop_front();
        lock.unlock();
        pollfd p = {item.second, POLLIN, 0};
        const int r = poll(&p, 1, 500);
        const uint64_t t = nowNs();
        close(item.second);
        lock.lock();
        FenceTime& slot = m_FenceTimes[item.first % m_FenceTimes.size()];
        slot.seq = item.first;
        slot.ns = r == 1 ? t : 0;
    }
}
