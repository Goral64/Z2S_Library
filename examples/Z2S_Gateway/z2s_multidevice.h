#ifndef Z2S_MULTIDEVICE_H_
#define Z2S_MULTIDEVICE_H_

#include <stdint.h>

/*****************************************************************************/
// Registers Zigbee devices switched to multidevice mode as separate Supla
// devices. Call after Z2S_initSuplaChannels() and before SuplaDevice.begin().
void Z2S_initMultiDevice(
  const char *supla_ca_cert, const char *supla_3rd_party_ca_cert);
/*****************************************************************************/
// Call after Supla channels of a Zigbee device were created (pairing), before
// the gateway sends anything about them. Channels of a new device go where
// "MULTIDEVICE NEW" says.
void Z2S_onMultiDeviceChannelsBuilt(uint8_t device_number_slot);
/*****************************************************************************/
// Call before a Zigbee device is removed from the devices table. The identity
// of a device using gateway channels (or waiting for choice) is deleted, so
// that it is a new device when paired again; a separate device keeps it and
// comes back as the same Supla device.
void Z2S_onMultiDeviceRemoved(uint8_t device_number_slot);
/*****************************************************************************/
// MULTIDEVICE [SEPARATE|GATEWAY|FORGET <device slot>]
//             [NEW GATEWAY|SEPARATE|ASK]
void Z2S_onMultiDeviceTelnetCmd(uint8_t params_number, char **param);
/*****************************************************************************/
// "Multidevice" tab of the web GUI.
void Z2S_buildMultiDeviceTabGUI();
/*****************************************************************************/

#endif //Z2S_MULTIDEVICE_H_
