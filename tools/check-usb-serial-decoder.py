#!/usr/bin/env python3
"""Exercise the actual pinned libusb ASCII decoder with mocked completed reads.

No USB library is loaded; no IOKit API is invoked. The only linked control
transfer implementation is the local deterministic stub below.
"""
from pathlib import Path
import subprocess
import argparse
from tempfile import TemporaryDirectory

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, required=True, help="Configured libusb checkout")
source = parser.parse_args().source.resolve()
text = (source / "libusb/descriptor.c").read_text()
start = text.index("int API_EXPORTED libusb_get_string_descriptor_ascii(")
opening = text.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (text[end] == "{") - (text[end] == "}")
    end += 1
decoder = text[start:end]
harness = r'''
#include "libusbi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DESC_HEADER_LENGTH 2
static int scenario, calls;
void usbi_log(struct libusb_context *ctx, enum libusb_log_level level,
              const char *function, const char *format, ...) { (void)ctx; }
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *handle,
  uint8_t type, uint8_t request, uint16_t value, uint16_t index,
  unsigned char *data, uint16_t length, unsigned int timeout) {
  calls++;
  assert(type == LIBUSB_ENDPOINT_IN && request == LIBUSB_REQUEST_GET_DESCRIPTOR);
  assert(timeout == 1000);
  assert((value >> 8) == LIBUSB_DT_STRING);
  if (scenario == 1) return 0;
  if (scenario == 2) return 2;
  if (scenario == 3) return LIBUSB_ERROR_TIMEOUT;
  if (scenario == 4) return LIBUSB_ERROR_PIPE;
  if ((value & 255) == 0) {
    assert(index == 0 && length == 4);
    unsigned char language[] = {4, 3, 9, 4};
    memcpy(data, language, sizeof(language));
    if (scenario == 5) data[1] = 255;
    return 4;
  }
  assert((value & 255) == 7 && index == 0x0409 && length >= 6);
  unsigned char serial[] = {6, 3, 'A', 0, 'B', 0};
  memcpy(data, serial, sizeof(serial));
  return scenario == 6 ? 4 : 6;
}
''' + decoder + r'''
int main(void) {
  int expected[] = {2, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO, LIBUSB_ERROR_TIMEOUT,
    LIBUSB_ERROR_PIPE, LIBUSB_ERROR_IO, LIBUSB_ERROR_IO};
  for (scenario = 0; scenario < 7; scenario++) {
    unsigned char output[32] = {0}; calls = 0;
    int result = libusb_get_string_descriptor_ascii(NULL, 7, output, sizeof(output));
    assert(result == expected[scenario]);
    if (scenario == 0) assert(!strcmp((char *)output, "AB") && calls == 2);
    printf("scenario %d: result=%d calls=%d\n", scenario, result, calls);
  }
}
'''
with TemporaryDirectory(prefix="quest3-usb-offline-") as temporary:
    root = Path(temporary)
    (root / "serial_decode_harness.c").write_text(harness)
    subprocess.run(["clang", "-I", str(source), "-I", str(source / "libusb"),
                    str(root / "serial_decode_harness.c"), "-o", str(root / "serial_decode_harness")], check=True)
    subprocess.run([str(root / "serial_decode_harness")], check=True)
