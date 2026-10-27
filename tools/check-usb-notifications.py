#!/usr/bin/env python3
"""Compile actual darwin_close with instrumented CF calls and fake USB objects.

The CF source and run loop are real host CoreFoundation objects. No run loop is
run and no USB/IOKit library is linked or loaded. A live source in CommonModes
after close fails the test.
"""
from pathlib import Path
import subprocess
import argparse
from tempfile import TemporaryDirectory

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True, help="libusb checkout")
source = parser.parse_args().source.resolve() / "libusb/os/darwin_usb.c"
text = source.read_text()
start = text.index("static void darwin_close (struct libusb_device_handle *dev_handle) {")
opening = text.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (text[end] == "{") - (text[end] == "}")
    end += 1
function = text[start:end]
prefix = r'''
#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int IOReturn;
#define kIOReturnSuccess 0
#define USB_MAXINTERFACES 32
static unsigned closes, removes;
static CFStringRef removed_mode;
static CFRunLoopRef libusb_darwin_acfl;
typedef struct usb_device_type { int (*USBDeviceClose)(void *); } usb_device_t;
struct darwin_cached_device { unsigned open_count; usb_device_t **device; };
struct libusb_device { struct darwin_cached_device *cached; };
struct darwin_device_handle_priv { CFRunLoopSourceRef cfSource; bool is_open; };
struct libusb_device_handle { struct libusb_device *dev; unsigned claimed_interfaces;
  struct darwin_device_handle_priv priv; };
#define usbi_get_device_handle_priv(h) (&(h)->priv)
#define DARWIN_CACHED_DEVICE(d) ((d)->cached)
#define usbi_err(...) ((void)0)
#define usbi_warn(...) ((void)0)
static int libusb_release_interface(struct libusb_device_handle *h, int i) { return 0; }
static int fake_close(void *unused) { closes++; return 0; }
static void perform(void *info) {}
static void remove_source(CFRunLoopRef loop, CFRunLoopSourceRef source, CFRunLoopMode mode) {
  removes++; removed_mode = mode;
  CFRunLoopRemoveSource(loop, source, mode);
}
#define CFRunLoopRemoveSource remove_source
'''
suffix = r'''
#undef CFRunLoopRemoveSource
int main(void) {
  libusb_darwin_acfl = CFRunLoopGetCurrent();
  CFRetain(libusb_darwin_acfl);
  CFRunLoopSourceContext context = {0}; context.perform = perform;
  CFRunLoopSourceRef source = CFRunLoopSourceCreate(NULL, 0, &context);
  assert(source);
  CFRunLoopAddSource(libusb_darwin_acfl, source, kCFRunLoopCommonModes);
  assert(CFRunLoopContainsSource(libusb_darwin_acfl, source, kCFRunLoopCommonModes));
  CFRetain(source); /* The test retains a reference while darwin_close releases its own. */
  usb_device_t table = { .USBDeviceClose = fake_close }, *interface = &table;
  struct darwin_cached_device cached = { .open_count = 1, .device = &interface };
  struct libusb_device device = { .cached = &cached };
  struct libusb_device_handle handle = { .dev = &device,
    .priv = { .cfSource = source, .is_open = true } };
  darwin_close(&handle);
  bool leaked = CFRunLoopContainsSource(libusb_darwin_acfl, source, kCFRunLoopCommonModes);
  bool mode_ok = CFEqual(removed_mode, kCFRunLoopCommonModes);
  assert(cached.open_count == 0 && handle.priv.cfSource == NULL);
  assert(closes == 1 && removes == 1);
  printf("actual darwin_close: remove_mode=%s common_registration_remains=%d USB_closes=%u\n",
    mode_ok ? "CommonModes" : "DefaultMode", leaked, closes);
  CFRunLoopRemoveSource(libusb_darwin_acfl, source, kCFRunLoopCommonModes);
  CFRelease(source);
  return leaked || !mode_ok;
}
'''
with TemporaryDirectory(prefix="quest3-usb-offline-") as temporary:
    root = Path(temporary)
    path = root / "notification_cleanup_harness.c"
    path.write_text(prefix + function + suffix)
    binary = root / "notification_cleanup_harness"
    subprocess.run(["clang", "-framework", "CoreFoundation", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
