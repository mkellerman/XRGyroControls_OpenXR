#!/usr/bin/env python3
"""Exercise the real pose producer with a fake headset and explicit completion."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "openxr_src/Main.cpp").read_text()
start = source.index('extern "C" int pose_fetch_loop()')
opening = source.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[start:end]
harness = r'''
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <semaphore>
#include <thread>
std::atomic<int> began{0};
std::atomic<bool> stop{false};
struct Stopped {};
struct Headset {
  enum class BeginFrameResult { RenderFully, SkipRender, SkipFully, Error };
  BeginFrameResult beginFrame(int *index) {
    if (stop) throw Stopped{};
    *index = ++began; return BeginFrameResult::RenderFully;
  }
  void endFrame(int) {}
} device;
Headset *headset = &device;
std::mutex endFrameMutex, poseLatchMutex;
std::binary_semaphore beginFrameSem(0), frameCompleteSem(0);
int xr_begin_frame_valid = 0, current_pose_idx = 0, pose_data_valid = 0;
'''
harness += function + r'''
int main() {
  std::thread worker([] { try { pose_fetch_loop(); } catch (const Stopped &) {} });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  bool passed = began == 1;
  if (!passed) fprintf(stderr, "FAIL: began %d frames before one completed\n", began.load());
  frameCompleteSem.release();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (began != 2) { fprintf(stderr, "FAIL: expected one next frame, got %d\n", began.load()); passed = false; }
  stop = true;
  while (frameCompleteSem.try_acquire()) {}
  frameCompleteSem.release();
  worker.join();
  if (!passed) return 1;
  puts("PASS: one OpenXR frame in flight; completion permits exactly one next frame.");
  return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "frame-order.cpp"
    executable = Path(directory) / "frame-order"
    cpp.write_text(harness)
    subprocess.run(["xcrun", "clang++", "-std=c++20", str(cpp), "-o", str(executable)], check=True)
    raise SystemExit(subprocess.run([str(executable)], timeout=5).returncode)
