#include "z2s_multidevice.h"

#include <SuplaDevice.h>
#include <supla/element.h>
#include <supla/channels/channel.h>
#include <supla/protocol/protocol_layer.h>
#include <supla/storage/config.h>
#include <supla/storage/storage.h>

#include <Z2S_multidevice/Z2S_MultiDeviceIdentity.h>
#include <Z2S_multidevice/Z2S_MultiDeviceLayer.h>

#include <algorithm>
#include <vector>

#include "z2s_devices_table.h"
#include "z2s_telnet_server.h"

// Where channels of newly paired Zigbee devices go (z2s_md_mode_t, pending =
// ask the user).
const static char Z2S_MULTIDEVICE_NEW_MODE[] PROGMEM = "z2s_md_new";

static Z2S_MultiDeviceLayer *multiDeviceLayer = nullptr;

static const char *multiDeviceCACert = nullptr;
static const char *multiDevice3rdPartyCACert = nullptr;

// SuplaDevice.begin() was called: a layer created now has to be initialized
// by hand.
static bool multiDeviceStarted = false;

/*****************************************************************************/

static Z2S_MultiDeviceLayer *Z2S_getMultiDeviceLayer() {

  if (multiDeviceLayer == nullptr) {

    multiDeviceLayer = new Z2S_MultiDeviceLayer(
      &SuplaDevice, multiDeviceCACert, multiDevice3rdPartyCACert);

    if (multiDeviceStarted)
      multiDeviceLayer->onInit();
  }
  return multiDeviceLayer;
}

/*****************************************************************************/

// The gateway registers again with its current channel list.
static void Z2S_restartGatewaySession() {

  for (auto proto = Supla::Protocol::ProtocolLayer::first(); proto != nullptr;
       proto = proto->next()) {

    if (proto != multiDeviceLayer)
      proto->disconnect();
  }
}

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

static uint8_t Z2S_getMultiDeviceNewMode() {

  uint8_t new_mode = Z2S_MD_MODE_GATEWAY;

  if (Supla::Storage::ConfigInstance())
    Supla::Storage::ConfigInstance()->getUInt8(
      Z2S_MULTIDEVICE_NEW_MODE, &new_mode);

  return new_mode <= Z2S_MD_MODE_PENDING ? new_mode : Z2S_MD_MODE_GATEWAY;
}

/*****************************************************************************/

// Puts channels of a Zigbee device where its mode says. Returns true when the
// gateway's own channel list changed.
static bool Z2S_applyMultiDeviceMode(
  uint8_t device_number_slot, const z2s_md_identity_t &identity) {

  auto channels = Z2S_getMultiDeviceChannels(device_number_slot);
  bool gateway_changed = false;

  if ((identity.mode == Z2S_MD_MODE_SEPARATE) && (!channels.empty())) {

    auto layer = Z2S_getMultiDeviceLayer();

    for (uint8_t channel_number : channels) {

      auto channel = Supla::Channel::GetByChannelNumber(channel_number);
      if (!channel->isExternallyRegistered())
        gateway_changed = true;
    }

    if (layer->findDevice(device_number_slot) >= 0) {

      layer->setDeviceChannels(
        device_number_slot, channels.data(), channels.size());
    } else {

      char name[SUPLA_DEVICE_NAME_MAXSIZE];
      Z2S_getMultiDeviceName(device_number_slot, name, sizeof(name));

      layer->addDevice(
        device_number_slot, identity.guid, identity.auth_key, name,
        channels.data(), channels.size());
    }
    return gateway_changed;
  }

  // Removing the session gives the channels back to the gateway, so their
  // state is compared with the one before.
  std::vector<bool> was_external;

  for (uint8_t channel_number : channels)
    was_external.push_back(
      Supla::Channel::GetByChannelNumber(channel_number)->
        isExternallyRegistered());

  if (multiDeviceLayer)
    multiDeviceLayer->removeDevice(device_number_slot);

  // Pending: the channels are registered nowhere until the user decides.
  bool external = identity.mode == Z2S_MD_MODE_PENDING;

  for (size_t i = 0; i < channels.size(); i++) {

    Supla::Channel::GetByChannelNumber(channels[i])->setExternallyRegistered(
      external);
    
    if (was_external[i] != external)
      gateway_changed = true;
  }
  return gateway_changed;
}

/*****************************************************************************/

void Z2S_initMultiDevice(
  const char *supla_ca_cert, const char *supla_3rd_party_ca_cert) {

  multiDeviceCACert = supla_ca_cert;
  multiDevice3rdPartyCACert = supla_3rd_party_ca_cert;

  for (uint8_t devices_counter = 0;
       devices_counter < Z2S_ZB_DEVICES_MAX_NUMBER; devices_counter++) {

    if (z2s_zb_devices_table[devices_counter].record_id == 0)
      continue;

    auto ieee_addr = z2s_zb_devices_table[devices_counter].ieee_addr;

    z2s_md_identity_t identity;

    switch (Z2S_loadMultiDeviceIdentity(ieee_addr, &identity)) {

      case Z2S_MultiDeviceIdentityResult::Loaded:
        Z2S_applyMultiDeviceMode(devices_counter, identity);
        break;

      case Z2S_MultiDeviceIdentityResult::Missing:
        // A device paired before: its channels stay with the gateway. With
        // the file, a missing one later means a newly paired device.
        Z2S_newMultiDeviceIdentity(ieee_addr, &identity);
        identity.mode = Z2S_MD_MODE_GATEWAY;
        Z2S_saveMultiDeviceIdentity(&identity);
        break;

      default:
        break;
    }
  }
  multiDeviceStarted = true;
}

/*****************************************************************************/

void Z2S_onMultiDeviceChannelsBuilt(uint8_t device_number_slot) {

  if ((device_number_slot >= Z2S_ZB_DEVICES_MAX_NUMBER) ||
      (z2s_zb_devices_table[device_number_slot].record_id == 0))
    return;

  auto ieee_addr = z2s_zb_devices_table[device_number_slot].ieee_addr;

  z2s_md_identity_t identity;

  switch (Z2S_loadMultiDeviceIdentity(ieee_addr, &identity)) {

    case Z2S_MultiDeviceIdentityResult::Loaded:
      break;

    case Z2S_MultiDeviceIdentityResult::Missing:
      Z2S_newMultiDeviceIdentity(ieee_addr, &identity);
      identity.mode = Z2S_getMultiDeviceNewMode();

      if (!Z2S_saveMultiDeviceIdentity(&identity))
        return;

      log_i(
        "Multidevice: new device #%u, mode %u", device_number_slot,
        identity.mode);
      break;

    default:
      return;
  }
  // Also on devices already paired: new channels go where the old ones are.
  if (Z2S_applyMultiDeviceMode(device_number_slot, identity))
    Z2S_restartGatewaySession();
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

static const char *Z2S_getMultiDeviceModeName(uint8_t mode) {

  switch (mode) {

    case Z2S_MD_MODE_SEPARATE: return "separate device";
    case Z2S_MD_MODE_PENDING: return "waiting for choice";
    default: return "gateway channels";
  }
}

/*****************************************************************************/

static void Z2S_printMultiDevices() {

  static const char *new_mode_names[] = { "GATEWAY", "SEPARATE", "ASK" };

  telnet.printf(
    "\n\r>Multidevice server: %s, new devices: %s\n\r",
    multiDeviceLayer ? multiDeviceLayer->serverName() : "-",
    new_mode_names[Z2S_getMultiDeviceNewMode()]);

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
        mode = Z2S_getMultiDeviceModeName(identity.mode);
        break;
      case Z2S_MultiDeviceIdentityResult::Damaged:
        mode = "identity file damaged";
        break;
      default:
        break;
    }

    telnet.printf(
      ">#%02u %s %s: %s", devices_counter, ieee_addr_str, name, mode);

    int device_index = multiDeviceLayer ?
      multiDeviceLayer->findDevice(devices_counter) : -1;

    if (device_index >= 0) {

      auto session = multiDeviceLayer->session(device_index);

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

static void Z2S_setMultiDeviceNewMode(char *mode_name) {

  uint8_t new_mode;

  if (strcasecmp(mode_name, "GATEWAY") == 0)
    new_mode = Z2S_MD_MODE_GATEWAY;
  else
  if (strcasecmp(mode_name, "SEPARATE") == 0)
    new_mode = Z2S_MD_MODE_SEPARATE;
  else
  if (strcasecmp(mode_name, "ASK") == 0)
    new_mode = Z2S_MD_MODE_PENDING;
  else {

    telnet.printf("\n\r>MULTIDEVICE NEW GATEWAY|SEPARATE|ASK\n\r>");
    return;
  }

  if (Supla::Storage::ConfigInstance()->setUInt8(
        Z2S_MULTIDEVICE_NEW_MODE, new_mode))
    Supla::Storage::ConfigInstance()->commit();

  telnet.printf("\n\r>New devices: %s\n\r>", mode_name);
}

/*****************************************************************************/

void Z2S_onMultiDeviceTelnetCmd(uint8_t params_number, char **param) {

  if (params_number == 0) {

    Z2S_printMultiDevices();
    return;
  }

  if ((strcasecmp(param[0], "NEW") == 0) && (params_number > 1)) {

    Z2S_setMultiDeviceNewMode(param[1]);
    return;
  }

  uint8_t mode;

  if ((strcasecmp(param[0], "SEPARATE") == 0) ||
      (strcasecmp(param[0], "ENABLE") == 0))
    mode = Z2S_MD_MODE_SEPARATE;
  else
  if ((strcasecmp(param[0], "GATEWAY") == 0) ||
      (strcasecmp(param[0], "DISABLE") == 0))
    mode = Z2S_MD_MODE_GATEWAY;
  else
    params_number = 0;

  if (params_number < 2) {

    telnet.printf(
      "\n\r>MULTIDEVICE [SEPARATE|GATEWAY <device slot>] "
      "[NEW GATEWAY|SEPARATE|ASK]\n\r>");
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
      Z2S_newMultiDeviceIdentity(ieee_addr, &identity);
      break;

    default:
      break;
  }

  uint8_t old_mode = identity.mode;
  identity.mode = mode;

  if (!Z2S_saveMultiDeviceIdentity(&identity)) {

    telnet.printf("\n\r>Saving identity of device #%u failed\n\r>",
                  device_number_slot);
    return;
  }

  if (Z2S_applyMultiDeviceMode(device_number_slot, identity))
    Z2S_restartGatewaySession();

  telnet.printf(
    "\n\r>Device #%u: %s\n\r", device_number_slot,
    Z2S_getMultiDeviceModeName(mode));

  // Channels registered by the gateway before stay in Supla Cloud until they
  // are deleted there; till then the gateway cannot register.
  if ((old_mode == Z2S_MD_MODE_GATEWAY) && (mode == Z2S_MD_MODE_SEPARATE))
    telnet.printf(
      ">Delete its channels of the gateway in Supla Cloud\n\r");

  telnet.printf(">");
}
