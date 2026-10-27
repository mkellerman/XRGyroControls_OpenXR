// Physical-device discovery probe. Does not create a compositor session or
// modify backboardd; generic OpenXR success must not mask a simulated HMD.
#include <openxr/openxr.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  const char *diagnostic = getenv("LIBUSB_QUEST3_DIAGNOSTIC");
  if (!diagnostic || strcmp(diagnostic, "1")) {
    fprintf(stderr, "Streaming remains quarantined; this handshake diagnostic requires LIBUSB_QUEST3_DIAGNOSTIC=1.\n");
    return 1;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc != 2) {
    fprintf(stderr, "Usage: xr-probe BUNDLE_DIRECTORY\n");
    return 2;
  }
  // The patched driver times out its handshake and joins workers normally.
  // Do not kill the process with an alarm while USB operations are pending.
  char path[4096];
  snprintf(path, sizeof(path), "%s/openxr_monado-dev.json", argv[1]);
  setenv("XR_RUNTIME_JSON", path, 1);
  setenv("XRHAX_ROOTDIR", argv[1], 1);
  setenv("XRT_COMPOSITOR_COMPUTE", "1", 1);
  snprintf(path, sizeof(path), "%s/IOUSBLib_ios_hax.dylib", argv[1]);
  if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) {
    fprintf(stderr, "USB shim load failed: %s\n", dlerror());
    return 1;
  }
  XrInstanceCreateInfo info = {XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy(info.applicationInfo.applicationName, "Quest transport probe");
  info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
  XrInstance instance = XR_NULL_HANDLE;
  XrResult result = xrCreateInstance(&info, &instance);
  if (XR_FAILED(result)) {
    fprintf(stderr, "xrCreateInstance failed: %d\n", result);
    return 1;
  }
  XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  result = xrGetSystem(instance, &systemInfo, &system);
  XrSystemProperties properties = {XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(result)) result = xrGetSystemProperties(instance, system, &properties);
  int failed = XR_FAILED(result);
  if (!failed) {
    printf("OpenXR system: %s\n", properties.systemName);
    failed = strstr(properties.systemName, "Simulated") != NULL;
    if (failed) fprintf(stderr, "FAIL: simulated fallback is not a physical Quest connection.\n");
    else printf("Physical-device discovery passed. Video and input are not tested by this probe.\n");
  } else {
    fprintf(stderr, "OpenXR system discovery failed: %d\n", result);
  }
  xrDestroyInstance(instance);
  return failed;
}
