#!/usr/bin/env python3
"""Check the real endFrame composition at the xrEndFrame API boundary."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "openxr_src/Headset.cpp").read_text()
start = source.index("void Headset::endFrame(int poseIdx) const")
opening = source.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
harness = r'''
#define XR_NO_PROTOTYPES
#include <openxr.h>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
extern "C" int openxr_get_mono_fov(float *angles) {
  float fov[] = {-0.7f, 0.7f, 0.5f, -0.5f};
  memcpy(angles, fov, sizeof(fov)); return 1;
}
bool captured = false, correct = false;
XrResult xrEndFrame(XrSession, const XrFrameEndInfo *info) {
  auto *layer = reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0]);
  const auto &l = layer->views[0]; const auto &r = layer->views[1];
  captured = true;
  correct = l.pose.position.x == r.pose.position.x && l.pose.orientation.w == r.pose.orientation.w &&
    l.fov.angleLeft == -0.7f && r.fov.angleLeft == -0.7f &&
    l.fov.angleRight == 0.7f && r.fov.angleRight == 0.7f &&
    l.subImage.imageArrayIndex == 0 && r.subImage.imageArrayIndex == 1;
  return XR_SUCCESS;
}
struct PoseData {
  std::vector<XrCompositionLayerProjectionView> eyeRenderInfos;
  XrViewState viewState{}; XrFrameState frameState{};
};
struct Headset {
  PoseData storedPoses[1]; XrSpace space{}; XrSession session{};
  void endFrame(int) const;
};
'''
harness += source[start:end] + r'''
int main() {
  Headset h;
  auto &p = h.storedPoses[0];
  p.eyeRenderInfos.resize(2);
  p.eyeRenderInfos[0].pose = {{0,0,0,1},{-0.032f,0,0}};
  p.eyeRenderInfos[1].pose = {{0,0,0,1},{0.032f,0,0}};
  p.eyeRenderInfos[0].fov = {-0.9f,0.6f,0.8f,-0.8f};
  p.eyeRenderInfos[1].fov = {-0.6f,0.9f,0.8f,-0.8f};
  p.eyeRenderInfos[1].subImage.imageArrayIndex = 1;
  p.viewState.viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  p.frameState.shouldRender = XR_TRUE;
  h.endFrame(0);
  if (!captured || !correct) { fputs("FAIL: mono image submitted with mismatched eye camera/FOV\n", stderr); return 1; }
  if (p.eyeRenderInfos[1].pose.position.x != 0.032f) return 1;
  puts("PASS: mono composition uses matching camera/FOV, preserves eye layers and original tracked poses.");
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "mono-projection.cpp"
    executable = Path(directory) / "mono-projection"
    cpp.write_text(harness)
    subprocess.run(["xcrun", "clang++", "-std=c++20", "-I", str(root / "openxr_headers"), str(cpp), "-o", str(executable)], check=True)
    raise SystemExit(subprocess.run([str(executable)], timeout=5).returncode)
