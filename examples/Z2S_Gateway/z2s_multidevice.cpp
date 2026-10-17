#include "z2s_multidevice.h"

#include <SuplaDevice.h>
#include <supla/element.h>
#include <supla/channels/channel.h>

#include <Z2S_multidevice/Z2S_MultiDeviceIdentity.h>
#include <Z2S_multidevice/Z2S_MultiDeviceLayer.h>

#include <algorithm>
#include <vector>

#include "z2s_devices_table.h"
#include "z2s_telnet_server.h"

static Z2S_MultiDeviceLayer *multiDeviceLayer = nullptr;

// Zigbee device slot of each session of multiDeviceLayer.
static std::vector<uint8_t> multiDeviceSlots;

/*****************************************************************************/

static void Z2S_getMultiDeviceName(
  uint8_t device_number_slot, char *name, size_t name_size) {

  snprintf(
    name, name_size, "%s (%s::%s)",
    Z2S_getZbDeviceLocalName(device_number_slot),
    Z2S_getZbDeviceManufacturerName(device_number_slot),
    Z2S_getZbDeviceModelName(device_number_slot));
}

/*****************************************************************************/

// Channels of a Zigbee device: elements whose channel belongs to the device's
// subdevice, with their secondary channels.
static std::vector<uint8_t> Z2S_getMultiDeviceChannels(
  uint8_t device_number_slot) {

  std::vector<uint8_t> channels;

  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {

    auto channel = element->getChannel();

    if ((channel == nullptr) ||
        (channel->getSubDeviceId() != device_number_slot + 1))
      continue;

    channels.push_back(channel->getChannelNumber());

    auto secondary_channel = element->getSecondaryChannel();

    if (secondary_channel)
      channels.push_back(secondary_channel->getChannelNumber());
  }
  std::sort(channels.begin(), channels.end());

  return channels;
}

/*****************************************************************************/

void Z2S_initMultiDevice(
  const char *supla_ca_cert, const char *supla_3rd_party_ca_cert) {

  for (uint8_t devices_counter = 0;
       devices_counter < Z2S_ZB_DEVICES_MAX_NUMBER; devices_counter++) {

    if (z2s_zb_devices_table[devices_counter].record_id == 0)
      continue;

    z2s_md_identity_t identity;

    if ((Z2S_loadMultiDeviceIdentity(
           z2s_zb_devices_table[devices_counter].ieee_addr, &identity) !=
         Z2S_MultiDeviceIdentityResult::Loaded) || (!identity.enabled))
      continue;

    auto channels = Z2S_getMultiDeviceChannels(devices_counter);

    if (channels.empty()) {

      log_i("Multidevice: device #%u has no channels", devices_counter);
      continue;
    }

    if (multiDeviceLayer == nullptr)
      multiDeviceLayer = new Z2S_MultiDeviceLayer(
        &SuplaDevice, supla_ca_cert, supla_3rd_party_ca_cert);

    char name[SUPLA_DEVICE_NAME_MAXSIZE];
    Z2S_getMultiDeviceName(devices_counter, name, sizeof(name));

    if (multiDeviceLayer->addDevice(
          identity.guid, identity.auth_key, name, channels.data(),
          channels.size()))
      multiDeviceSlots.push_back(devices_counter);
  }
}

/*****************************************************************************/

static const char *Z2S_getMultiDeviceStateName(
  Z2S_MultiDeviceSession *session) {

  switch (session->state()) {

    case Z2S_MultiDeviceSession::State::Idle:
      return session->isSuspended() ? "suspended" : "disconnected";
    case Z2S_MultiDeviceSession::State::Connecting: return "connecting";
    case Z2S_MultiDeviceSession::State::Registering: return "registering";
    case Z2S_MultiDeviceSession::State::Registered: return "registered";
    case Z2S_MultiDeviceSession::State::Disabled: return "disabled";
  }
  return "?";
}

/*****************************************************************************/

static void Z2S_printMultiDevices() {

  telnet.printf("\n\r>Multidevice server: %s\n\r",
                multiDeviceLayer ? multiDeviceLayer->serverName() : "-");

  for (uint8_t devices_counter = 0;
       devices_counter < Z2S_ZB_DEVICES_MAX_NUMBER; devices_counter++) {

    if (z2s_zb_devices_table[devices_counter].record_id == 0)
      continue;

    char ieee_addr_str[24];
    ieee_addr_to_str(
      ieee_addr_str, z2s_zb_devices_table[devices_counter].ieee_addr);

    char name[SUPLA_DEVICE_NAME_MAXSIZE];
    Z2S_getMultiDeviceName(devices_counter, name, sizeof(name));

    z2s_md_identity_t identity;
    const char *mode = "gateway channels";

    switch (Z2S_loadMultiDeviceIdentity(
              z2s_zb_devices_table[devices_counter].ieee_addr, &identity)) {

      case Z2S_MultiDeviceIdentityResult::Loaded:
        if (identity.enabled) mode = "separate device";
        break;
      case Z2S_MultiDeviceIdentityResult::Damaged:
        mode = "identity file damaged";
        break;
      default:
        break;
    }

    telnet.printf(
      ">#%02u %s %s: %s", devices_counter, ieee_addr_str, name, mode);

    for (size_t i = 0; i < multiDeviceSlots.size(); i++) {

      if (multiDeviceSlots[i] != devices_counter)
        continue;

      auto session = multiDeviceLayer->session(i);

      telnet.printf(
        ", %s, %u channel(s), last result %d",
        Z2S_getMultiDeviceStateName(session), session->channelCount(),
        session->lastResultCode());
    }
    telnet.printf("\n\r");
  }
  telnet.printf(">");
}

/*****************************************************************************/

void Z2S_onMultiDeviceTelnetCmd(uint8_t params_number, char **param) {

  if (params_number == 0) {

    Z2S_printMultiDevices();
    return;
  }

  bool enable = strcmp(param[0], "ENABLE") == 0;

  if ((!enable && (strcmp(param[0], "DISABLE") != 0)) ||
      (params_number < 2)) {

    telnet.printf(
      "\n\r>MULTIDEVICE [ENABLE|DISABLE <device slot>]\n\r>");
    return;
  }

  uint8_t device_number_slot = strtoul(param[1], nullptr, 0);

  if ((device_number_slot >= Z2S_ZB_DEVICES_MAX_NUMBER) ||
      (z2s_zb_devices_table[device_number_slot].record_id == 0)) {

    telnet.printf("\n\r>Invalid device slot %u\n\r>", device_number_slot);
    return;
  }

  auto ieee_addr = z2s_zb_devices_table[device_number_slot].ieee_addr;

  z2s_md_identity_t identity;

  switch (Z2S_loadMultiDeviceIdentity(ieee_addr, &identity)) {

    case Z2S_MultiDeviceIdentityResult::Damaged:
      telnet.printf(
        "\n\r>Identity file of device #%u is damaged - not changed\n\r>",
        device_number_slot);
      return;

    case Z2S_MultiDeviceIdentityResult::Missing:
      if (!enable) {

        telnet.printf("\n\r>Device #%u already uses gateway channels\n\r>",
                      device_number_slot);
        return;
      }
      Z2S_newMultiDeviceIdentity(ieee_addr, &identity);
      break;

    default:
      break;
  }

  identity.enabled = enable ? 1 : 0;

  if (!Z2S_saveMultiDeviceIdentity(&identity)) {

    telnet.printf("\n\r>Saving identity of device #%u failed\n\r>",
                  device_number_slot);
    return;
  }

  telnet.printf(
    "\n\r>Device #%u: %s after restart\n\r>", device_number_slot,
    enable ? "separate Supla device" : "gateway channels");
}
