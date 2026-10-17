#ifndef Z2S_MULTIDEVICE_H_
#define Z2S_MULTIDEVICE_H_

#include <stdint.h>

/*****************************************************************************/
// Registers Zigbee devices switched to multidevice mode as separate Supla
// devices. Call after Z2S_initSuplaChannels() and before SuplaDevice.begin().
void Z2S_initMultiDevice(
  const char *supla_ca_cert, const char *supla_3rd_party_ca_cert);
/*****************************************************************************/
// MULTIDEVICE [ENABLE|DISABLE <device slot>]
void Z2S_onMultiDeviceTelnetCmd(uint8_t params_number, char **param);
/*****************************************************************************/

#endif //Z2S_MULTIDEVICE_H_
