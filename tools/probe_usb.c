// Enumerate USB without creating an OpenXR system or starting Quest Link.
#include <libusb.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--serial"))) {
    fprintf(stderr, "Usage: xr-usb-probe BUNDLE_DIRECTORY|--native [--serial]\n");
    return 2;
  }
  if (strcmp(argv[1], "--native")) {
    const char *diagnostic = getenv("LIBUSB_QUEST3_DIAGNOSTIC");
    if (!diagnostic || strcmp(diagnostic, "1")) {
      fprintf(stderr, "Simulator USB diagnostics require LIBUSB_QUEST3_DIAGNOSTIC=1.\n");
      return 1;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/IOUSBLib_ios_hax.dylib", argv[1]);
    setenv("XRHAX_ROOTDIR", argv[1], 1);
    if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) {
      fprintf(stderr, "USB shim load failed: %s\n", dlerror());
      return 1;
    }
  }
  libusb_context *context = NULL;
  int result = libusb_init(&context);
  printf("libusb_init=%d (%s)\n", result, libusb_error_name(result));
  if (result) return 1;
  libusb_device **devices = NULL;
  ssize_t count = libusb_get_device_list(context, &devices);
  printf("device_count=%zd\n", count);
  int found = 0;
  int failed = 0;
  for (ssize_t i = 0; i < count; i++) {
    struct libusb_device_descriptor descriptor;
    if (!libusb_get_device_descriptor(devices[i], &descriptor) && descriptor.idVendor == 0x2833) {
      printf("Meta USB %04x:%04x\n", descriptor.idVendor, descriptor.idProduct);
      found = 1;
      if (argc == 3) {
        libusb_device_handle *handle = NULL;
        int opened = libusb_open(devices[i], &handle);
        if (opened) {
          fprintf(stderr, "USB open failed: %s\n", libusb_error_name(opened));
          failed = 1;
          continue;
        }
        unsigned char serial[256];
        int length = libusb_get_string_descriptor_ascii(handle, descriptor.iSerialNumber,
                                                        serial, sizeof(serial));
        // Report completion and length without disclosing the headset serial.
        printf("Serial descriptor completion: %d%s\n", length,
               length < 0 ? libusb_error_name(length) : " bytes");
        failed |= length <= 0;
        libusb_close(handle);
        puts("USB handle closed after descriptor completion.");
      }
    }
  }
  if (count >= 0) libusb_free_device_list(devices, 1);
  libusb_exit(context);
  return found && !failed ? 0 : 1;
}
