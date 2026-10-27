#pragma once
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef XR_SHARED_MEMORY_NAME
#define XR_SHARED_MEMORY_NAME "/tmp/Sim2OpenXR_shmem"
#endif
#define XR_SHARED_MEMORY_SIZE 1048576

// Darwin POSIX shared memory cannot be truncated again after its initial size
// is set, even to the same size. Both processes may start first.
static inline int xr_open_shared_memory(void) {
    int fd = shm_open(XR_SHARED_MEMORY_NAME, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) return -1;
    struct stat info;
    if (fstat(fd, &info) == 0) {
        if (info.st_size == XR_SHARED_MEMORY_SIZE) return fd;
        if (info.st_size == 0) {
            if (ftruncate(fd, XR_SHARED_MEMORY_SIZE) == 0) return fd;
            // The peer may have initialized it between fstat and ftruncate.
            if (fstat(fd, &info) == 0 && info.st_size == XR_SHARED_MEMORY_SIZE) return fd;
        }
        errno = EINVAL;
    }
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}
