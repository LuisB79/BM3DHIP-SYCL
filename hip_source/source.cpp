/*
 * VapourSynth wrapper for BM3DCUDA
 * Copyright (c) 2021 WolframRhodium
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hip/hip_runtime_api.h>

#include <VapourSynth.h>
#include <VSHelper.h>

#if not __cpp_lib_atomic_wait
#include <chrono>
#include <thread>
using namespace std::chrono_literals;
#endif

using namespace std::string_literals;

extern const char * launch(
    float * d_res, float * d_src, float * h_res,
    int width, int height, int stride,
    float sigma, int block_step, int bm_range,
    int radius, int ps_num, int ps_range,
    bool chroma, float sigma_u, float sigma_v,
    bool final_, float extractor,
    hipStream_t stream, int warp_size
) noexcept;

#define checkError(expr) do {                                            \
    if (hipError_t result = expr; result != hipSuccess) [[unlikely]] { \
        const char * error_str = hipGetErrorString(result);             \
        return set_error("'"s + # expr + "' failed: " + error_str);      \
    }                                                                    \
} while(0)

constexpr int kFast = 4;

static VSPlugin * myself = nullptr;

struct ticket_semaphore {
    std::atomic<intptr_t> ticket {};
    std::atomic<intptr_t> current {};

    void acquire() noexcept {
        intptr_t tk { ticket.fetch_add(1, std::memory_order_acquire) };
        while (true) {
            intptr_t curr { current.load(std::memory_order_acquire) };
            if (tk <= curr) {
                return;
            }
#if __cpp_lib_atomic_wait
            current.wait(curr, std::memory_order::relaxed);
#else // __cpp_lib_atomic_wait
            std::this_thread::sleep_for(1ms);
#endif // __cpp_lib_atomic_wait
        }
    }

    void release() noexcept {
        current.fetch_add(1, std::memory_order_release);
#if __cpp_lib_atomic_wait
        current.notify_all();
#endif // __cpp_lib_atomic_wait
    }
};

template <typename T, auto deleter>
struct Resource {
    T data;

    [[nodiscard]] constexpr Resource() noexcept = default;

    [[nodiscard]] constexpr Resource(T x) noexcept : data(x) {}

    [[nodiscard]] constexpr Resource(Resource&& other) noexcept
        : data(std::exchange(other.data, T{}))
    { }

    constexpr Resource& operator=(Resource&& other) noexcept {
        if (this == &other) return *this;
        deleter_(data);
        data = std::exchange(other.data, T{});
        return *this;
    }

    Resource operator=(Resource other) = delete;

    Resource(const Resource& other) = delete;

    constexpr operator T() const noexcept {
        return data;
    }

    constexpr auto deleter_(T x) noexcept {
        if (x) {
            (void) deleter(x);
        }
    }

    constexpr Resource& operator=(T x) noexcept {
        deleter_(data);
        data = x;
        return *this;
    }

    ~Resource() noexcept {
        deleter_(data);
    }
};

struct CUDA_Resource {
    Resource<float *, hipFree> d_src;
    Resource<float *, hipFree> d_res;
    Resource<float *, hipHostFree> h_res;
    Resource<hipStream_t, hipStreamDestroy> stream;
};

struct BM3DData {
    VSNodeRef * node;
    VSNodeRef * ref_node;
    const VSVideoInfo * vi;

    // stored in graphexec:
    float sigma[3];
    int block_step[3];
    int bm_range[3];
    int ps_num[3];
    int ps_range[3];
    float extractor;

    int radius;
    int num_copy_engines; // fast
    bool chroma;
    bool process[3]; // sigma != 0
    bool final_;
    bool zero_init;

    int d_pitch;
    int device_id;
    int warp_size;

    ticket_semaphore semaphore;
    std::vector<CUDA_Resource> resources;
    std::mutex resources_lock;
};

static inline void Aggregation(
    float * VS_RESTRICT dstp, int dst_stride,
    const float * VS_RESTRICT srcp, int src_stride,
    int width, int height
) noexcept {

    const float * wdst = srcp;
    const float * weight = &srcp[height * src_stride];

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            dstp[x] = wdst[x] / weight[x];
        }

        dstp += dst_stride;
        wdst += src_stride;
        weight += src_stride;
    }
}

static void VS_CC BM3DInit(
    VSMap *in, VSMap *out, void **instanceData, VSNode *node,
    VSCore *core, const VSAPI *vsapi
) noexcept {

    BM3DData * d = static_cast<BM3DData *>(*instanceData);

    if (d->radius) {
        VSVideoInfo vi = *d->vi;
        vi.height *= 2 * (2 * d->radius + 1);
        vsapi->setVideoInfo(&vi, 1, node);
    } else {
        vsapi->setVideoInfo(d->vi, 1, node);
    }
}

static const VSFrameRef *VS_CC BM3DGetFrame(
    int n, int activationReason, void **instanceData, void **frameData,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d = static_cast<BM3DData *>(*instanceData);

    if (activationReason == arInitial) {
        int start_frame = std::max(n - d->radius, 0);
        int end_frame = std::min(n + d->radius, d->vi->numFrames - 1);

        for (int i = start_frame; i <= end_frame; ++i) {
            vsapi->requestFrameFilter(i, d->node, frameCtx);
        }
        if (d->final_) {
            for (int i = start_frame; i <= end_frame; ++i) {
                vsapi->requestFrameFilter(i, d->ref_node, frameCtx);
            }
        }
    } else if (activationReason == arAllFramesReady) {
        if (auto error = hipSetDevice(d->device_id); error != hipSuccess) {
            vsapi->setFilterError(
                ("BM3D: "s + hipGetErrorString(error)).c_str(),
                frameCtx
            );
            return nullptr;
        }

        int radius = d->radius;
        int temporal_width = 2 * radius + 1;
        bool final_ = d->final_;
        int num_input_frames = temporal_width * (final_ ? 2 : 1); // including ref

        using freeFrame_t = decltype(vsapi->freeFrame);
        const std::vector srcs = [&](){
            std::vector<std::unique_ptr<const VSFrameRef, const freeFrame_t &>> temp;

            temp.reserve(num_input_frames);

            if (final_) {
                for (int i = -radius; i <= radius; ++i) {
                    int clamped_n = std::clamp(n + i, 0, d->vi->numFrames - 1);
                    temp.emplace_back(
                        vsapi->getFrameFilter(clamped_n, d->ref_node, frameCtx),
                        vsapi->freeFrame
                    );
                }
            }

            for (int i = -radius; i <= radius; ++i) {
                int clamped_n = std::clamp(n + i, 0, d->vi->numFrames - 1);
                temp.emplace_back(
                    vsapi->getFrameFilter(clamped_n, d->node, frameCtx),
                    vsapi->freeFrame
                );
            }

            return temp;
        }();

        const VSFrameRef * src = srcs[radius + (final_ ? temporal_width : 0)].get();

        std::unique_ptr<VSFrameRef, const freeFrame_t &> dst { nullptr, vsapi->freeFrame };
        if (radius) {
            dst.reset(
                vsapi->newVideoFrame(
                    d->vi->format, d->vi->width,
                    d->vi->height * 2 * temporal_width,
                    src, core)
            );
            for (int i = 0; i < d->vi->format->numPlanes; ++i) {
                if (d->zero_init && !d->process[i]) {
                    auto ptr = vsapi->getWritePtr(dst.get(), i);
                    auto height = vsapi->getFrameHeight(dst.get(), i);
                    auto pitch = vsapi->getStride(dst.get(), i);
                    memset(ptr, 0, height * pitch);
                }
            }
        } else {
            const VSFrameRef * fr[] = {
                d->process[0] ? nullptr : src,
                d->process[1] ? nullptr : src,
                d->process[2] ? nullptr : src
            };
            const int pl[] = { 0, 1, 2 };

            dst.reset(
                vsapi->newVideoFrame2(
                    d->vi->format, d->vi->width,
                    d->vi->height, fr, pl, src, core)
            );
        }

        d->semaphore.acquire();
        d->resources_lock.lock();
        auto resource = std::move(d->resources.back());
        d->resources.pop_back();
        d->resources_lock.unlock();

        const auto set_error = [&](const std::string & error_message) {
            d->resources_lock.lock();
            d->resources.push_back(std::move(resource));
            d->resources_lock.unlock();
            d->semaphore.release();

            vsapi->setFilterError(("BM3D: " + error_message).c_str(), frameCtx);

            return nullptr;
        };

        float * const h_res = resource.h_res;
        hipStream_t stream = resource.stream;
        int d_pitch = d->d_pitch;
        int d_stride = d_pitch / sizeof(float);

        if (d->chroma) {
            int width = vsapi->getFrameWidth(src, 0);
            int height = vsapi->getFrameHeight(src, 0);
            int s_pitch = vsapi->getStride(src, 0);
            int s_stride = s_pitch / sizeof(float);
            int width_bytes = width * sizeof(float);

            float * h_src = h_res;
            for (int outer = 0; outer < (final_ ? 2 : 1); ++outer) {
                for (int i = 0; i < (int) std::size(d->process); ++i) {
                    for (int j = 0; j < temporal_width; ++j) {
                        if (i == 0 || d->process[i]) {
                            auto current_src = srcs[j + outer * temporal_width].get();

                            vs_bitblt(
                                h_src, d_pitch,
                                vsapi->getReadPtr(current_src, i), s_pitch,
                                width_bytes, height
                            );
                        }
                        h_src += d_stride * height;
                    }
                }
            }

            auto err = launch(resource.d_res, resource.d_src, resource.h_res, width, height, d_stride, d->sigma[0], d->block_step[0], d->bm_range[0], d->radius, d->ps_num[0], d->ps_range[0], d->chroma, d->sigma[1], d->sigma[2], d->final_, d->extractor, stream, d->warp_size);

            if (err) {
                return set_error(err);
            }

            checkError(hipStreamSynchronize(stream));

            float * h_dst = h_res;
            for (int plane = 0; plane < (int) std::size(d->process); ++plane) {
                if (!d->process[plane]) {
                    h_dst += d_stride * height * 2 * temporal_width;
                    continue;
                }

                float * dstp = reinterpret_cast<float *>(
                    vsapi->getWritePtr(dst.get(), plane));

                if (radius) {
                    vs_bitblt(
                        dstp, s_pitch, h_dst, d_pitch,
                        width_bytes, height * 2 * temporal_width
                    );
                } else {
                    Aggregation(
                        dstp, s_stride,
                        h_dst, d_stride,
                        width, height
                    );
                }

                h_dst += d_stride * height * 2 * temporal_width;
            }
        } else { // !d->chroma
            for (int plane = 0; plane < d->vi->format->numPlanes; plane++) {
                if (!d->process[plane]) {
                    continue;
                }

                int width = vsapi->getFrameWidth(src, plane);
                int height = vsapi->getFrameHeight(src, plane);
                int s_pitch = vsapi->getStride(src, plane);
                int s_stride = s_pitch / sizeof(float);
                int width_bytes = width * sizeof(float);

                float * h_src = h_res;
                for (int i = 0; i < num_input_frames; ++i) {
                    vs_bitblt(
                        h_src, d_pitch,
                        vsapi->getReadPtr(srcs[i].get(), plane), s_pitch,
                        width_bytes, height
                    );
                    h_src += d_stride * height;
                }

                auto err = launch(resource.d_res, resource.d_src, resource.h_res, width, height, d_stride, d->sigma[plane], d->block_step[plane], d->bm_range[plane], d->radius, d->ps_num[plane], d->ps_range[plane], d->chroma, 0.0f, 0.0f, d->final_, d->extractor, stream, d->warp_size);

                if (err) {
                    return set_error(err);
                }

                checkError(hipStreamSynchronize(stream));

                float * dstp = reinterpret_cast<float *>(
                    vsapi->getWritePtr(dst.get(), plane));

                if (radius) {
                    vs_bitblt(
                        dstp, s_pitch, h_res, d_pitch,
                        width_bytes, height * 2 * temporal_width
                    );
                } else {
                    Aggregation(
                        dstp, s_stride,
                        h_res, d_stride,
                        width, height
                    );
                }
            }
        }

        d->resources_lock.lock();
        d->resources.push_back(std::move(resource));
        d->resources_lock.unlock();
        d->semaphore.release();

        if (radius) {
            VSMap * dst_prop { vsapi->getFramePropsRW(dst.get()) };

            vsapi->propSetInt(dst_prop, "BM3D_V_radius", d->radius, paReplace);

            int64_t process[3] { d->process[0], d->process[1], d->process[2] };
            vsapi->propSetIntArray(dst_prop, "BM3D_V_process", process, 3);
        }

        return dst.release();
    }

    return nullptr;
}

static void VS_CC BM3DFree(
    void *instanceData, VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d = static_cast<BM3DData *>(instanceData);

    vsapi->freeNode(d->node);
    vsapi->freeNode(d->ref_node);

    (void) hipSetDevice(d->device_id);

    delete d;
}

static void VS_CC BM3DCreate(
    const VSMap *in, VSMap *out, void *userData,
    VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d { std::make_unique<BM3DData>() };

    const auto set_error = [&](const std::string & error_message) {
        vsapi->setError(out, ("BM3D: " + error_message).c_str());
        vsapi->freeNode(d->node);
        vsapi->freeNode(d->ref_node);
    };

    d->node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d->vi = vsapi->getVideoInfo(d->node);
    const int width = d->vi->width;
    const int height = d->vi->height;
    const int bits_per_sample = d->vi->format->bitsPerSample;

    if (
        !isConstantFormat(d->vi) || d->vi->format->sampleType == stInteger ||
        (d->vi->format->sampleType == stFloat && bits_per_sample != 32)) {
        return set_error("only constant format 32bit float input supported");
    }

    int error;

    d->ref_node = vsapi->propGetNode(in, "ref", 0, &error);
    bool final_;
    if (error) {
        d->ref_node = nullptr;
        final_ = false;
    } else {
        auto ref_vi = vsapi->getVideoInfo(d->ref_node);
        if (ref_vi->format->id != d->vi->format->id) {
            return set_error("\"ref\" must be of the same format as \"clip\"");
        } else if (ref_vi->width != width || ref_vi->height != height ) {
            return set_error("\"ref\" must be of the same dimensions as \"clip\"");
        } else if (ref_vi->numFrames != d->vi->numFrames) {
            return set_error("\"ref\" must be of the same number of frames as \"clip\"");
        }

        final_ = true;
    }
    d->final_ = final_;

    for (int i = 0; i < (int) std::size(d->sigma); ++i) {
        d->sigma[i] = static_cast<float>(
            vsapi->propGetFloat(in, "sigma", i, &error));

        if (error) {
            d->sigma[i] = (i == 0) ? 3.0f : d->sigma[i - 1];
        } else if (d->sigma[i] < 0.0f) {
            return set_error("\"sigma\" must be non-negative");
        }

        if (d->sigma[i] < std::numeric_limits<float>::epsilon()) {
            d->process[i] = false;
        } else {
            d->process[i] = true;
        }
    }
    for (int i = 0; i < (int) std::size(d->sigma); ++i) {
        // assumes grayscale input, hard_thr = 2.7
        d->sigma[i] *= (3.0f / 4.0f) / 255.0f * 64.0f * (final_ ? 1.0f : 2.7f);
    }

    for (int i = 0; i < (int) std::size(d->block_step); ++i) {
        d->block_step[i] = int64ToIntS(
            vsapi->propGetInt(in, "block_step", i, &error));

        if (error) {
            d->block_step[i] = (i == 0) ? 8 : d->block_step[i - 1];
        } else if (d->block_step[i] <= 0 || d->block_step[i] > 8) {
            return set_error("\"block_step\" must be in range [1, 8]");
        }
    }

    for (int i = 0; i < (int) std::size(d->bm_range); ++i) {
        d->bm_range[i] = int64ToIntS(
            vsapi->propGetInt(in, "bm_range", i, &error));

        if (error) {
            d->bm_range[i] = (i == 0) ? 9 : d->bm_range[i - 1];
        } else if (d->bm_range[i] <= 0) {
            return set_error("\"bm_range\" must be positive");
        }
    }

    const int radius = [&](){
        int temp = int64ToIntS(vsapi->propGetInt(in, "radius", 0, &error));
        if (error) {
            return 0;
        }
        return temp;
    }();
    if (radius < 0) {
        return set_error("\"radius\" must be non-negative");
    }
    d->radius = radius;

    for (int i = 0; i < (int) std::size(d->ps_num); ++i) {
        d->ps_num[i] = int64ToIntS(
            vsapi->propGetInt(in, "ps_num", i, &error));

        if (error) {
            d->ps_num[i] = (i == 0) ? 2 : d->ps_num[i - 1];
        } else if (d->ps_num[i] <= 0 || d->ps_num[i] > 8) {
            return set_error("\"ps_num\" must be in range [1, 8]");
        }
    }

    for (int i = 0; i < (int) std::size(d->ps_range); ++i) {
        d->ps_range[i] = int64ToIntS(
            vsapi->propGetInt(in, "ps_range", i, &error));

        if (error) {
            d->ps_range[i] = (i == 0) ? 4 : d->ps_range[i - 1];
        } else if (d->ps_range[i] <= 0) {
            return set_error("\"ps_range\" must be positive");
        }
    }

    const bool chroma = [&](){
        bool temp = !!vsapi->propGetInt(in, "chroma", 0, &error);
        if (error) {
            return false;
        }
        return temp;
    }();
    if (chroma && d->vi->format->id != pfYUV444PS) {
        return set_error("clip format must be YUV444 when \"chroma\" is true");
    }
    d->chroma = chroma;

    const int device_id = [&](){
        int temp = int64ToIntS(vsapi->propGetInt(in, "device_id", 0, &error));
        if (error) {
            return 0;
        }
        return temp;
    }();
    int device_count;
    checkError(hipGetDeviceCount(&device_count));
    if (0 <= device_id && device_id < device_count) {
        checkError(hipSetDevice(device_id));
    } else {
        return set_error("invalid device ID (" + std::to_string(device_id) + ")");
    }
    d->device_id = device_id;

    checkError(hipDeviceGetAttribute(&d->warp_size, hipDeviceAttributeWarpSize, d->device_id));

    const bool fast = [&](){
        bool temp = !!vsapi->propGetInt(in, "fast", 0, &error);
        if (error) {
            return true;
        }
        return temp;
    }();
    const int num_copy_engines { fast ? kFast : 1 };
    d->num_copy_engines = num_copy_engines;

    const float extractor = [&](){
        int temp = int64ToIntS(vsapi->propGetInt(in, "extractor_exp", 0, &error));
        if (error) {
            return 0.0f;
        }
        return (temp ? std::ldexp(1.0f, temp) : 0.0f);
    }();

    d->zero_init = !!vsapi->propGetInt(in, "zero_init", 0, &error);
    if (error) {
        d->zero_init = true;
    }

    // GPU resource allocation
    {
        d->semaphore.current.store(num_copy_engines - 1, std::memory_order_relaxed);

        d->resources.reserve(num_copy_engines);

        int max_width { d->process[0] ? width : width >> d->vi->format->subSamplingW };
        int max_height { d->process[0] ? height : height >> d->vi->format->subSamplingH };

        int num_planes { chroma ? 3 : 1 };
        int temporal_width = 2 * radius + 1;
        size_t d_pitch;
        for (int i = 0; i < num_copy_engines; ++i) {
            Resource<float *, hipFree> d_src {};
            if (i == 0) {
                checkError(hipMallocPitch(
                    (void **) &d_src.data, &d_pitch, max_width * sizeof(float),
                    (final_ ? 2 : 1) * num_planes * temporal_width * max_height));
                d->d_pitch = static_cast<int>(d_pitch);
            } else {
                checkError(hipMalloc(&d_src.data,
                    (final_ ? 2 : 1) * num_planes * temporal_width * max_height * d_pitch));
            }

            Resource<float *, hipFree> d_res {};
            checkError(hipMalloc(&d_res.data,
                num_planes * temporal_width * 2 * max_height * d_pitch));

            Resource<float *, hipHostFree> h_res {};
            checkError(hipHostMalloc(&h_res.data,
                num_planes * temporal_width * 2 * max_height * d_pitch));

            Resource<hipStream_t, hipStreamDestroy> stream {};
            checkError(hipStreamCreateWithFlags(&stream.data,
                hipStreamNonBlocking));

            d->resources.push_back(CUDA_Resource{
                .d_src = std::move(d_src),
                .d_res = std::move(d_res),
                .h_res = std::move(h_res),
                .stream = std::move(stream)
            });
        }
    }

    vsapi->createFilter(
        in, out, "BM3D",
        BM3DInit, BM3DGetFrame, BM3DFree,
        fmParallel, 0, d.release(), core
    );
}

struct VAggregateData {
    VSNodeRef * node;

    VSNodeRef * src_node;
    const VSVideoInfo * src_vi;

    std::array<bool, 3> process; // sigma != 0

    int radius;

    std::unordered_map<std::thread::id, float *> buffer;
    std::shared_mutex buffer_lock;
};

static void VS_CC VAggregateInit(
    VSMap *in, VSMap *out, void **instanceData, VSNode *node,
    VSCore *core, const VSAPI *vsapi
) {

    VAggregateData * d = static_cast<VAggregateData *>(*instanceData);

    vsapi->setVideoInfo(d->src_vi, 1, node);
}

static const VSFrameRef *VS_CC VAggregateGetFrame(
    int n, int activationReason, void **instanceData, void **frameData,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi
) {

    auto * d = static_cast<VAggregateData *>(*instanceData);

    if (activationReason == arInitial) {
        int start_frame = std::max(n - d->radius, 0);
        int end_frame = std::min(n + d->radius, d->src_vi->numFrames - 1);

        for (int i = start_frame; i <= end_frame; ++i) {
            vsapi->requestFrameFilter(i, d->node, frameCtx);
        }
        vsapi->requestFrameFilter(n, d->src_node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef * src_frame = vsapi->getFrameFilter(n, d->src_node, frameCtx);

        std::vector<const VSFrameRef *> vbm3d_frames;
        vbm3d_frames.reserve(2 * d->radius + 1);
        for (int i = n - d->radius; i <= n + d->radius; ++i) {
            auto frame_id = std::clamp(i, 0, d->src_vi->numFrames - 1);
            vbm3d_frames.emplace_back(vsapi->getFrameFilter(frame_id, d->node, frameCtx));
        }

        float * buffer {};
        {
            const auto thread_id = std::this_thread::get_id();
            bool init = true;

            d->buffer_lock.lock_shared();

            try {
                const auto & const_buffer = d->buffer;
                buffer = const_buffer.at(thread_id);
            } catch (const std::out_of_range &) {
                init = false;
            }

            d->buffer_lock.unlock_shared();

            if (!init) {
                assert(d->process[0] || d->src_vi->format->numPlanes > 1);

                const int max_height {
                    d->process[0] ?
                    vsapi->getFrameHeight(src_frame, 0) :
                    vsapi->getFrameHeight(src_frame, 1)
                };
                const int max_pitch {
                    d->process[0] ?
                    vsapi->getStride(src_frame, 0) :
                    vsapi->getStride(src_frame, 1)
                };

                buffer = reinterpret_cast<float *>(std::malloc(2 * max_height * max_pitch));

                std::lock_guard _ { d->buffer_lock };
                d->buffer.emplace(thread_id, buffer);
            }
        }

        const VSFrameRef * fr[] {
            d->process[0] ? nullptr : src_frame,
            d->process[1] ? nullptr : src_frame,
            d->process[2] ? nullptr : src_frame
        };
        constexpr int pl[] { 0, 1, 2 };
        auto dst_frame = vsapi->newVideoFrame2(
            d->src_vi->format,
            d->src_vi->width, d->src_vi->height,
            fr, pl, src_frame, core);

        for (int plane = 0; plane < d->src_vi->format->numPlanes; ++plane) {
            if (d->process[plane]) {
                int plane_width = vsapi->getFrameWidth(src_frame, plane);
                int plane_height = vsapi->getFrameHeight(src_frame, plane);
                int plane_stride = vsapi->getStride(src_frame, plane) / sizeof(float);

                memset(buffer, 0, 2 * plane_height * plane_stride * sizeof(float));

                for (int i = 0; i < 2 * d->radius + 1; ++i) {
                    auto agg_src = reinterpret_cast<const float *>(vsapi->getReadPtr(vbm3d_frames[i], plane));
                    agg_src += (2 * d->radius - i) * 2 * plane_height * plane_stride;

                    float * agg_dst = buffer;

                    for (int y = 0; y < 2 * plane_height; ++y) {
                        for (int x = 0; x < plane_width; ++x) {
                            agg_dst[x] += agg_src[x];
                        }
                        agg_src += plane_stride;
                        agg_dst += plane_stride;
                    }
                }

                auto dstp = reinterpret_cast<float *>(vsapi->getWritePtr(dst_frame, plane));
                Aggregation(dstp, plane_stride, buffer, plane_stride, plane_width, plane_height);
            }
        }

        for (const auto & frame : vbm3d_frames) {
            vsapi->freeFrame(frame);
        }
        vsapi->freeFrame(src_frame);

        return dst_frame;
    }

    return nullptr;
}

static void VS_CC VAggregateFree(
    void *instanceData, VSCore *core, const VSAPI *vsapi
) noexcept {

    VAggregateData * d = static_cast<VAggregateData *>(instanceData);

    for (const auto & [_, ptr] : d->buffer) {
        std::free(ptr);
    }

    vsapi->freeNode(d->src_node);
    vsapi->freeNode(d->node);

    delete d;
}

static void VS_CC VAggregateCreate(
    const VSMap *in, VSMap *out, void *userData,
    VSCore *core, const VSAPI *vsapi
) {

    auto d { std::make_unique<VAggregateData>() };

    d->node = vsapi->propGetNode(in, "clip", 0, nullptr);
    auto vi = vsapi->getVideoInfo(d->node);
    d->src_node = vsapi->propGetNode(in, "src", 0, nullptr);
    d->src_vi = vsapi->getVideoInfo(d->src_node);

    d->radius = (vi->height / d->src_vi->height - 2) / 4;

    d->process.fill(false);
    int num_planes_args = vsapi->propNumElements(in, "planes");
    for (int i = 0; i < num_planes_args; ++i) {
        int plane = vsapi->propGetInt(in, "planes", i, nullptr);
        d->process[plane] = true;
    }

    VSCoreInfo core_info;
    vsapi->getCoreInfo2(core, &core_info);
    d->buffer.reserve(core_info.numThreads);

    vsapi->createFilter(
        in, out, "VAggregate",
        VAggregateInit, VAggregateGetFrame, VAggregateFree,
        fmParallel, 0, d.release(), core);
}

static void VS_CC BM3Dv2Create(
    const VSMap *in, VSMap *out, void *userData,
    VSCore *core, const VSAPI *vsapi
) {

    std::array<bool, 3> process;
    process.fill(true);

    int num_sigma_args = vsapi->propNumElements(in, "sigma");
    for (int i = 0; i < std::min(3, num_sigma_args); ++i) {
        auto sigma = vsapi->propGetFloat(in, "sigma", i, nullptr);
        if (sigma < std::numeric_limits<float>::epsilon()) {
            process[i] = false;
        }
    }

    bool skip = true;
    auto src = vsapi->propGetNode(in, "clip", 0, nullptr);
    auto src_vi = vsapi->getVideoInfo(src);
    for (int i = 0; i < src_vi->format->numPlanes; ++i) {
        skip &= !process[i];
    }
    if (skip) {
        vsapi->propSetNode(out, "clip", src, paReplace);
        vsapi->freeNode(src);
        return ;
    }

    auto map = vsapi->invoke(myself, "BM3D", in);
    if (auto error = vsapi->getError(map); error) {
        vsapi->setError(out, error);
        vsapi->freeMap(map);
        vsapi->freeNode(src);
        return ;
    }

    int error;
    int radius = vsapi->propGetInt(in, "radius", 0, &error);
    if (error) {
        radius = 0;
    }
    if (radius == 0) {
        // spatial BM3D should handle everything itself
        auto node = vsapi->propGetNode(map, "clip", 0, nullptr);
        vsapi->freeMap(map);
        vsapi->propSetNode(out, "clip", node, paReplace);
        vsapi->freeNode(node);
        vsapi->freeNode(src);
        return ;
    }

    vsapi->propSetNode(map, "src", src, paReplace);
    vsapi->freeNode(src);

    for (int i = 0; i < 3; ++i) {
        if (process[i]) {
            vsapi->propSetInt(map, "planes", i, paAppend);
        }
    }

    auto map2 = vsapi->invoke(myself, "VAggregate", map);
    vsapi->freeMap(map);
    if (auto error = vsapi->getError(map2); error) {
        vsapi->setError(out, error);
        vsapi->freeMap(map2);
        return ;
    }

    auto node = vsapi->propGetNode(map2, "clip", 0, nullptr);
    vsapi->freeMap(map2);
    vsapi->propSetNode(out, "clip", node, paReplace);
    vsapi->freeNode(node);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(
    VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin
) {

    myself = plugin;

    configFunc(
        "com.wolframrhodium.bm3dhip", "bm3dhip",
        "BM3D algorithm implemented in HIP",
        VAPOURSYNTH_API_VERSION, 1, plugin
    );

    constexpr auto bm3d_args {
        "clip:clip;"
        "ref:clip:opt;"
        "sigma:float[]:opt;"
        "block_step:int[]:opt;"
        "bm_range:int[]:opt;"
        "radius:int:opt;"
        "ps_num:int[]:opt;"
        "ps_range:int[]:opt;"
        "chroma:int:opt;"
        "device_id:int:opt;"
        "fast:int:opt;"
        "extractor_exp:int:opt;"
        "zero_init:int:opt;"
    };

    registerFunc("BM3D", bm3d_args, BM3DCreate, nullptr, plugin);

    registerFunc(
        "VAggregate",
        "clip:clip;"
        "src:clip;"
        "planes:int[];",
        VAggregateCreate, nullptr, plugin);

    registerFunc("BM3Dv2", bm3d_args, BM3Dv2Create, nullptr, plugin);
}
