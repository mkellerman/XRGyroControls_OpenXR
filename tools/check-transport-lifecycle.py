#!/usr/bin/env python3
"""Exercise the real Monado destructor with instrumented USB/resource seams.

No headset or simulator access. This catches close/free while worker threads
still own the resources; it cannot reproduce or rule out a host kernel panic.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()
    source = (args.source / "src/xrt/drivers/quest_link/ql_xrsp.cpp").read_text()
    destructor = function(source, "void ql_xrsp_host_destroy(")
    harness = r'''
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#define QL_SWAPCHAIN_DEPTH 1
#define QL_NUM_SLICES 1
#define QL_IDX_SLICE(j,i) 0
struct os_thread_helper { bool initialized, running; };
struct ql_xrsp_host {
    bool initialized;
    os_thread_helper read_thread, write_thread;
    void *dev, *ctx;
    int if_num, pose_mutex, usb_mutex, stream_mutex[1];
    void *csd_stream[1], *idr_stream[1];
};
static ql_xrsp_host *current;
static int closes, exits;
static void assert_quiescent() {
    if (current->read_thread.running || current->write_thread.running)
        throw std::runtime_error("USB/resources released while worker threads still own them");
}
static void os_thread_helper_destroy(os_thread_helper *worker) {
    if (!worker->initialized) throw std::runtime_error("thread helper destroyed twice");
    worker->running = false;
    worker->initialized = false;
}
static void libusb_release_interface(void *, int) { assert_quiescent(); }
static void libusb_close(void *) { assert_quiescent(); ++closes; }
static void libusb_exit(void *) { assert_quiescent(); ++exits; }
static void os_mutex_destroy(int *) { assert_quiescent(); }
'''
    # Older sources have no stop function: the baseline destructor still runs
    # and fails the same resource-ownership assertion.
    if "void ql_xrsp_host_stop(" in source:
        harness += function(source, "void ql_xrsp_host_stop(")
    harness += destructor
    harness += r'''
int main() {
    ql_xrsp_host host{};
    host.initialized = true;
    host.read_thread.running = host.write_thread.running = true;
    host.read_thread.initialized = host.write_thread.initialized = true;
    host.dev = host.ctx = &host;
    current = &host;
    try {
        ql_xrsp_host_destroy(&host);
        if (closes != 1 || exits != 1)
            throw std::runtime_error("USB device/context not released exactly once");
        ql_xrsp_host_destroy(&host);
        if (closes != 1 || exits != 1)
            throw std::runtime_error("repeated destructor double-released USB resources");
    } catch (const std::exception &error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    puts("PASS: workers quiesce before USB/resource release; repeated cleanup is harmless.");
}
'''
    with tempfile.TemporaryDirectory() as directory:
        cpp = Path(directory) / "lifecycle.cpp"
        executable = Path(directory) / "lifecycle"
        cpp.write_text(harness)
        subprocess.run(["xcrun", "clang++", "-std=c++17", str(cpp), "-o", str(executable)], check=True)
        return subprocess.run([str(executable)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
