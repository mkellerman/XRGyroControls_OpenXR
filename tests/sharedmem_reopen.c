#define XR_SHARED_MEMORY_NAME "/Sim2OpenXR_reopen_test"
#include "../simui/simui_src/sharedmem_buffer.h"
#include <assert.h>
#include <string.h>
#include <sys/wait.h>

int main(void) {
    shm_unlink(XR_SHARED_MEMORY_NAME);
    int writer = xr_open_shared_memory();
    assert(writer >= 0);
    char *written = mmap(NULL, XR_SHARED_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, writer, 0);
    assert(written != MAP_FAILED);
    strcpy(written, "tracked pose");
    close(writer);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int reader = xr_open_shared_memory();
        assert(reader >= 0);
        char *read = mmap(NULL, XR_SHARED_MEMORY_SIZE, PROT_READ, MAP_SHARED, reader, 0);
        assert(read != MAP_FAILED);
        assert(strcmp(read, "tracked pose") == 0);
        // Reproduce the legacy failure with the real Darwin syscall.
        assert(ftruncate(reader, XR_SHARED_MEMORY_SIZE) == -1 && errno == EINVAL);
        close(reader);
        munmap(read, XR_SHARED_MEMORY_SIZE);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    munmap(written, XR_SHARED_MEMORY_SIZE);
    shm_unlink(XR_SHARED_MEMORY_NAME);
    int invalid = shm_open(XR_SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0600);
    assert(invalid >= 0 && ftruncate(invalid, 4096) == 0);
    close(invalid);
    assert(xr_open_shared_memory() == -1 && errno == EINVAL);
    shm_unlink(XR_SHARED_MEMORY_NAME);
    return 0;
}
