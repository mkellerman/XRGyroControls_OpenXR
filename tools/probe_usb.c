// Enumerate USB without creating an OpenXR system or starting Quest Link.
#include <libusb.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  fprintf(stderr, "Disabled after a host USB kernel panic; live transport testing is quarantined.\n");
  return 1;
#if 0
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc != 2) {
    fprintf(stderr, "Usage: xr-usb-probe BUNDLE_DIRECTORY\n");
    return 2;
  }
  alarm(15);
  char path[4096];
  snprintf(path, sizeof(path), "%s/IOUSBLib_ios_hax.dylib", argv[1]);
  setenv("XRHAX_ROOTDIR", argv[1], 1);
  if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) {
    fprintf(stderr, "USB shim load failed: %s\n", dlerror());
    return 1;
  }
  libusb_context *context = NULL;
  int result = libusb_init(&context);
  printf("libusb_init=%d (%s)\n", result, libusb_error_name(result));
  if (result) return 1;
  libusb_device **devices = NULL;
  ssize_t count = libusb_get_device_list(context, &devices);
  printf("device_count=%zd\n", count);
  int found = 0;
  for (ssize_t i = 0; i < count; i++) {
    struct libusb_device_descriptor descriptor;
    if (!libusb_get_device_descriptor(devices[i], &descriptor) && descriptor.idVendor == 0x2833) {
      printf("Meta USB %04x:%04x\n", descriptor.idVendor, descriptor.idProduct);
      found = 1;
    }
  }
  if (count >= 0) libusb_free_device_list(devices, 1);
  libusb_exit(context);
  return found ? 0 : 1;
#endif
}
