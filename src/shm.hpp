#pragma once
// Shared ARGB8888 SHM buffer creation (bar surface + menu popup).
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <cstddef>

struct ShmAlloc {
    void*  data = nullptr;
    size_t size = 0;
};

inline void shm_buffer_release(void* data, wl_buffer* buf) {
    auto* a = static_cast<ShmAlloc*>(data);
    wl_buffer_destroy(buf);
    munmap(a->data, a->size);
    delete a;
}

inline const wl_buffer_listener shm_buffer_listener = {
    .release = shm_buffer_release};

// Returns a buffer whose backing memory is written through *out_data.
inline wl_buffer* create_argb_buffer(wl_shm* shm, int w, int h,
                                     void** out_data) {
    const int    stride = w * 4;
    const size_t size   = static_cast<size_t>(stride) * h;
    int fd = memfd_create("mattbar-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
        if (fd >= 0) close(fd);
        return nullptr;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int>(size));
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                  WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(buffer, &shm_buffer_listener,
                           new ShmAlloc{data, size});
    *out_data = data;
    return buffer;
}
