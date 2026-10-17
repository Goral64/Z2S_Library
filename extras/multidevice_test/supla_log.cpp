// SPDX-License-Identifier: GPL-2.0-or-later
// supla-common built with SUPLA_DEVICE expects the platform to provide
// supla_vlog().

#include <stdio.h>

extern "C" void supla_vlog(int pri, const char *message) {
  fprintf(stderr, "[srpc %d] %s\n", pri, message);
}
