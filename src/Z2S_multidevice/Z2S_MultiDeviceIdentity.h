// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Z2S_MULTIDEVICE_IDENTITY_H_
#define Z2S_MULTIDEVICE_IDENTITY_H_

#include <stdint.h>
#include <supla-common/proto.h>

// Supla identity of a ZigBee device registered as a separate device, one
// file per device (named after its IEEE address). GUID and AuthKey are
// generated once and never changed: a new GUID is a new device in Supla
// Cloud.
typedef struct z2s_md_identity_s {
  uint32_t version;
  uint8_t ieee_addr[8];
  uint8_t enabled;
  uint8_t reserved[3];
  uint8_t guid[SUPLA_GUID_SIZE];
  uint8_t auth_key[SUPLA_AUTHKEY_SIZE];
} z2s_md_identity_t;

enum class Z2S_MultiDeviceIdentityResult { Loaded, Missing, Damaged };

Z2S_MultiDeviceIdentityResult Z2S_loadMultiDeviceIdentity(
    const uint8_t ieee_addr[8], z2s_md_identity_t *identity);
bool Z2S_saveMultiDeviceIdentity(const z2s_md_identity_t *identity);
// Fills a new identity with random GUID and AuthKey.
void Z2S_newMultiDeviceIdentity(const uint8_t ieee_addr[8],
                                z2s_md_identity_t *identity);

#endif  // Z2S_MULTIDEVICE_IDENTITY_H_
