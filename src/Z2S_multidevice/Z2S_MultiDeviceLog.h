// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Z2S_MULTIDEVICE_LOG_H_
#define Z2S_MULTIDEVICE_LOG_H_

#ifdef ARDUINO
#include <esp32-hal-log.h>

#define Z2S_MD_LOG_E(...) log_e(__VA_ARGS__)
#define Z2S_MD_LOG_W(...) log_w(__VA_ARGS__)
#define Z2S_MD_LOG_I(...) log_i(__VA_ARGS__)
#define Z2S_MD_LOG_D(...) log_d(__VA_ARGS__)
#else
// Host builds (unit tests).
#include <stdio.h>

#define Z2S_MD_LOG(level, fmt, ...) \
  fprintf(stderr, "[" level "] " fmt "\n", ##__VA_ARGS__)
#define Z2S_MD_LOG_E(fmt, ...) Z2S_MD_LOG("E", fmt, ##__VA_ARGS__)
#define Z2S_MD_LOG_W(fmt, ...) Z2S_MD_LOG("W", fmt, ##__VA_ARGS__)
#define Z2S_MD_LOG_I(fmt, ...) Z2S_MD_LOG("I", fmt, ##__VA_ARGS__)
#define Z2S_MD_LOG_D(fmt, ...) Z2S_MD_LOG("D", fmt, ##__VA_ARGS__)
#endif

#endif  // Z2S_MULTIDEVICE_LOG_H_
