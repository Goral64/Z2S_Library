// SPDX-License-Identifier: GPL-2.0-or-later

#include "Z2S_MultiDeviceIdentity.h"

#include <esp_random.h>
#include <stdio.h>
#include <string.h>

#include "../z2s_little_fs.h"
#include "Z2S_MultiDeviceLog.h"

namespace {

constexpr uint32_t kIdentityVersion = 1;

void identityFileName(const uint8_t ieee_addr[8], char *buf, size_t size) {
  // Most significant byte first, like IEEE addresses shown by Z2S.
  snprintf(buf, size, "md_%02x%02x%02x%02x%02x%02x%02x%02x.z2s", ieee_addr[7],
           ieee_addr[6], ieee_addr[5], ieee_addr[4], ieee_addr[3],
           ieee_addr[2], ieee_addr[1], ieee_addr[0]);
}

}  // namespace

Z2S_MultiDeviceIdentityResult Z2S_loadMultiDeviceIdentity(
    const uint8_t ieee_addr[8], z2s_md_identity_t *identity) {
  char file_name[32];
  identityFileName(ieee_addr, file_name, sizeof(file_name));
  if (Z2S_getFileSize(file_name) == 0) {
    return Z2S_MultiDeviceIdentityResult::Missing;
  }
  if (!Z2S_loadFileWithCRC(file_name, reinterpret_cast<uint8_t *>(identity),
                           sizeof(*identity)) ||
      identity->version != kIdentityVersion ||
      memcmp(identity->ieee_addr, ieee_addr, 8) != 0) {
    // Never replaced by a new identity: that would be a new device in Supla
    // Cloud.
    Z2S_MD_LOG_E("Multidevice identity file %s is damaged", file_name);
    return Z2S_MultiDeviceIdentityResult::Damaged;
  }
  return Z2S_MultiDeviceIdentityResult::Loaded;
}

bool Z2S_saveMultiDeviceIdentity(const z2s_md_identity_t *identity) {
  char file_name[32];
  identityFileName(identity->ieee_addr, file_name, sizeof(file_name));
  return Z2S_saveFileWithCRC(file_name,
                             reinterpret_cast<const uint8_t *>(identity),
                             sizeof(*identity));
}

void Z2S_newMultiDeviceIdentity(const uint8_t ieee_addr[8],
                                z2s_md_identity_t *identity) {
  memset(identity, 0, sizeof(*identity));
  identity->version = kIdentityVersion;
  memcpy(identity->ieee_addr, ieee_addr, 8);
  esp_fill_random(identity->guid, sizeof(identity->guid));
  esp_fill_random(identity->auth_key, sizeof(identity->auth_key));
}
