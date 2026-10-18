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
#include "ZigbeeCore.h"
#include "ZigbeeGateway.h"

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

// Availability like in zigbee2mqtt: an active (mains powered) device is
// pinged after 10 min of silence and is unreachable when it does not answer;
// a passive (battery powered) device is unreachable after 25 h of silence.
// A timeout set for its channels in Z2S takes precedence (without ping).
static const uint32_t Z2S_MD_ACTIVE_TIMEOUT_MS  = 10 * 60 * 1000;
static const uint32_t Z2S_MD_PASSIVE_TIMEOUT_MS = 25 * 3600 * 1000;
static const uint32_t Z2S_MD_PING_ANSWER_MS     = 10 * 1000;

// When the last ping was sent to each device.
static uint32_t multiDevicePingMs[Z2S_ZB_DEVICES_MAX_NUMBER] = {};

/*****************************************************************************/

static bool Z2S_isMultiDeviceActive(uint8_t device_number_slot) {

  // Bit 7: secondary battery.
  uint8_t power_source = 
    z2s_zb_devices_table[device_number_slot].power_source & 0x7F;

  return (power_source != ZB_POWER_SOURCE_UNKNOWN) && 
         (power_source != ZB_POWER_SOURCE_BATTERY);
}

/*****************************************************************************/

// The longest timeout of the device's channels, 0 = none.
static uint32_t Z2S_getMultiDeviceChannelsTimeoutMs(
  uint8_t device_number_slot) {

  uint32_t timeout_ms = 0;

  for (uint8_t channel_number :
         Z2S_getMultiDeviceChannels(device_number_slot)) {

    auto z2s_core = Z2S_Core::getZ2SCoreByChannelNumber(channel_number);

    if (z2s_core && (z2s_core->getTimeoutMs() > timeout_ms))
      timeout_ms = z2s_core->getTimeoutMs();
  }
  return timeout_ms;
}

/*****************************************************************************/

// Time since anything was received from the device; 0 = nothing since start,
// counted from the start.
static uint32_t Z2S_getMultiDeviceSilenceMs(uint8_t device_number_slot) {

  return millis() - z2s_zb_devices_table[device_number_slot].last_seen_ms;
}

/*****************************************************************************/

static uint32_t Z2S_getMultiDeviceTimeoutMs(uint8_t device_number_slot) {

  uint32_t timeout_ms = Z2S_getMultiDeviceChannelsTimeoutMs(
    device_number_slot);

  if (timeout_ms > 0)
    return timeout_ms;

  return Z2S_isMultiDeviceActive(device_number_slot) ? 
    Z2S_MD_ACTIVE_TIMEOUT_MS : Z2S_MD_PASSIVE_TIMEOUT_MS;
}

/*****************************************************************************/

static void Z2S_pingMultiDevice(uint8_t device_number_slot) {

  auto channels = Z2S_getMultiDeviceChannels(device_number_slot);

  if (channels.empty())
    return;

  auto z2s_core = Z2S_Core::getZ2SCoreByChannelNumber(channels[0]);

  if (z2s_core == nullptr)
    return;

  log_i("Multidevice: pinging device #%u", device_number_slot);

  zbGateway.sendAttributeRead(
    z2s_zb_devices_table[device_number_slot].short_addr, 
    z2s_core->getZ2SChannel()->endpoint, ESP_ZB_ZCL_CLUSTER_ID_BASIC, 
    ESP_ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID);

  multiDevicePingMs[device_number_slot] = millis();
}

/*****************************************************************************/

// Also pings active devices when needed; while waiting for the answer the
// device keeps its current state (`unreachable`).
static bool Z2S_isMultiDeviceUnreachable(
  uint8_t device_number_slot, bool unreachable) {

  uint32_t timeout_ms = Z2S_getMultiDeviceTimeoutMs(device_number_slot);
  uint32_t silence_ms = Z2S_getMultiDeviceSilenceMs(device_number_slot);

  if ((Z2S_getMultiDeviceChannelsTimeoutMs(device_number_slot) > 0) || 
      (!Z2S_isMultiDeviceActive(device_number_slot)))
    return silence_ms > timeout_ms;

  if (silence_ms <= timeout_ms)
    return false;

  uint32_t since_ping_ms = millis() - multiDevicePingMs[device_number_slot];

  // Silent for too long: ping, and again every timeout while unreachable.
  if ((multiDevicePingMs[device_number_slot] == 0) || 
      (since_ping_ms >= timeout_ms)) {

    Z2S_pingMultiDevice(device_number_slot);
    return unreachable;
  }

  if (since_ping_ms <= Z2S_MD_PING_ANSWER_MS)
    return unreachable;

  // The ping was sent during this silence and got no answer in time.
  return silence_ms > since_ping_ms;
}

/*****************************************************************************/

static void Z2S_iterateMultiDeviceGUI();

/*****************************************************************************/

// A separate device whose Zigbee device does not answer is disconnected from
// Supla (in Supla Cloud it looks like a device switched off) and connects
// again when the Zigbee device is back. Nothing changes while Zigbee is not
// running, so that a problem of the radio does not disconnect all devices at
// once.
class Z2S_MultiDeviceMonitor : public Supla::Element {

public:
  void iterateAlways() override {

    if (millis() - _last_check_ms < 1000)
      return;

    _last_check_ms = millis();

    Z2S_iterateMultiDeviceGUI();

    if (multiDeviceLayer == nullptr)
      return;

    if (!Zigbee.started())
      return;

    for (uint8_t devices_counter = 0;
         devices_counter < Z2S_ZB_DEVICES_MAX_NUMBER; devices_counter++) {

      int device_index = multiDeviceLayer->findDevice(devices_counter);

      if (device_index < 0)
        continue;

      auto session = multiDeviceLayer->session(device_index);
      bool suspended = Z2S_isMultiDeviceUnreachable(
        devices_counter, session->isSuspended());

      if (session->isSuspended() != suspended)
        multiDeviceLayer->setDeviceSuspended(devices_counter, suspended);
    }
  }

private:
  uint32_t _last_check_ms = 0;
};

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
        // the file, a missing one later means a newly paired device. A device
        // without channels (joined, but not interviewed) is still new.
        if (Z2S_getMultiDeviceChannels(devices_counter).empty())
          break;

        Z2S_newMultiDeviceIdentity(ieee_addr, &identity);
        identity.mode = Z2S_MD_MODE_GATEWAY;
        Z2S_saveMultiDeviceIdentity(&identity);
        break;

      default:
        break;
    }
  }
  new Z2S_MultiDeviceMonitor();

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

void Z2S_onMultiDeviceRemoved(uint8_t device_number_slot) {

  if ((device_number_slot >= Z2S_ZB_DEVICES_MAX_NUMBER) ||
      (z2s_zb_devices_table[device_number_slot].record_id == 0))
    return;

  auto ieee_addr = z2s_zb_devices_table[device_number_slot].ieee_addr;

  z2s_md_identity_t identity;

  if ((Z2S_loadMultiDeviceIdentity(ieee_addr, &identity) ==
       Z2S_MultiDeviceIdentityResult::Loaded) &&
      (identity.mode != Z2S_MD_MODE_SEPARATE)) {

    log_i(
      "Multidevice: device #%u removed, identity %s", device_number_slot,
      Z2S_deleteMultiDeviceIdentity(ieee_addr) ? "deleted" : "not deleted");
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

static const char *Z2S_getMultiDeviceModeName(uint8_t mode) {

  switch (mode) {

    case Z2S_MD_MODE_SEPARATE: return "separate device";
    case Z2S_MD_MODE_PENDING: return "waiting for choice";
    default: return "gateway channels";
  }
}

/*****************************************************************************/

static const char *multiDeviceNewModeNames[] = { "GATEWAY", "SEPARATE", "ASK" };

/*****************************************************************************/

static bool Z2S_isMultiDeviceSlotValid(uint8_t device_number_slot) {

  return (device_number_slot < Z2S_ZB_DEVICES_MAX_NUMBER) &&
         (z2s_zb_devices_table[device_number_slot].record_id > 0);
}

/*****************************************************************************/

// One line about a Zigbee device: mode and, for a separate device, its
// session (a table row of the web GUI when table_row is set). Returns the
// mode (z2s_md_mode_t), 0xFF when the identity file is damaged.
static uint8_t Z2S_describeMultiDevice(
  uint8_t device_number_slot, char *buffer, size_t buffer_size,
  bool table_row = false) {

  char ieee_addr_str[24];
  ieee_addr_to_str(
    ieee_addr_str, z2s_zb_devices_table[device_number_slot].ieee_addr);

  char name[SUPLA_DEVICE_NAME_MAXSIZE];
  Z2S_getMultiDeviceName(device_number_slot, name, sizeof(name));

  z2s_md_identity_t identity;
  uint8_t mode = Z2S_MD_MODE_GATEWAY;
  const char *mode_name = "gateway channels";

  switch (Z2S_loadMultiDeviceIdentity(
            z2s_zb_devices_table[device_number_slot].ieee_addr, &identity)) {

    case Z2S_MultiDeviceIdentityResult::Loaded:
      mode = identity.mode;
      mode_name = Z2S_getMultiDeviceModeName(mode);
      break;
    case Z2S_MultiDeviceIdentityResult::Damaged:
      mode = 0xFF;
      mode_name = "identity file damaged";
      break;
    default:
      break;
  }
  // Joined, but not interviewed: nothing to register yet.
  if ((mode != 0xFF) && Z2S_getMultiDeviceChannels(device_number_slot).empty())
    mode_name = "no channels";

  int length = snprintf(
    buffer, buffer_size,
    table_row ? "<tr><td>#%02u</td><td>%s</td><td>%s</td><td>%s</td>" :
                "#%02u %s %s: %s",
    device_number_slot, ieee_addr_str, name, mode_name);

  int device_index = multiDeviceLayer ?
    multiDeviceLayer->findDevice(device_number_slot) : -1;

  if ((length <= 0) || (static_cast<size_t>(length) >= buffer_size))
    return mode;

  if (device_index >= 0) {

    auto session = multiDeviceLayer->session(device_index);

    snprintf(
      buffer + length, buffer_size - length,
      table_row ?
        "<td>%s</td><td>%u</td><td>%d</td><td>%s</td><td>%u s</td>"
        "<td>%u s</td></tr>" :
        ", %s, %u channel(s), last result %d, %s, seen %u s ago, "
        "timeout %u s",
      Z2S_getMultiDeviceStateName(session), session->channelCount(),
      session->lastResultCode(),
      Z2S_isMultiDeviceActive(device_number_slot) ? "active" : "passive",
      Z2S_getMultiDeviceSilenceMs(device_number_slot) / 1000,
      Z2S_getMultiDeviceTimeoutMs(device_number_slot) / 1000);
  } else if (table_row)
    snprintf(
      buffer + length, buffer_size - length, "<td colspan=6></td></tr>");

  return mode;
}

/*****************************************************************************/

static void Z2S_setMultiDeviceNewMode(
  uint8_t new_mode, char *message, size_t message_size) {

  if (Supla::Storage::ConfigInstance()->setUInt8(
        Z2S_MULTIDEVICE_NEW_MODE, new_mode))
    Supla::Storage::ConfigInstance()->commit();

  snprintf(
    message, message_size, "New devices: %s",
    multiDeviceNewModeNames[new_mode]);
}

/*****************************************************************************/

static void Z2S_setMultiDeviceMode(
  uint8_t device_number_slot, uint8_t mode, char *message,
  size_t message_size) {

  if (!Z2S_isMultiDeviceSlotValid(device_number_slot)) {

    snprintf(message, message_size, "Invalid device slot %u",
             device_number_slot);
    return;
  }

  auto ieee_addr = z2s_zb_devices_table[device_number_slot].ieee_addr;

  z2s_md_identity_t identity;

  switch (Z2S_loadMultiDeviceIdentity(ieee_addr, &identity)) {

    case Z2S_MultiDeviceIdentityResult::Damaged:
      snprintf(
        message, message_size,
        "Identity file of device #%u is damaged - not changed",
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

    snprintf(message, message_size, "Saving identity of device #%u failed",
             device_number_slot);
    return;
  }

  if (Z2S_applyMultiDeviceMode(device_number_slot, identity))
    Z2S_restartGatewaySession();

  // Channels registered by the gateway before stay in Supla Cloud until they
  // are deleted there; till then the gateway cannot register.
  snprintf(
    message, message_size, "Device #%u: %s%s", device_number_slot,
    Z2S_getMultiDeviceModeName(mode),
    ((old_mode == Z2S_MD_MODE_GATEWAY) && (mode == Z2S_MD_MODE_SEPARATE)) ?
      " - delete its channels of the gateway in Supla Cloud" : "");
}

/*****************************************************************************/

// Deletes the identity file of a device using gateway channels (or with a
// damaged file): when paired again, it is a new device for "MULTIDEVICE NEW".
static void Z2S_forgetMultiDevice(
  uint8_t device_number_slot, char *message, size_t message_size) {

  if (!Z2S_isMultiDeviceSlotValid(device_number_slot)) {

    snprintf(message, message_size, "Invalid device slot %u",
             device_number_slot);
    return;
  }

  auto ieee_addr = z2s_zb_devices_table[device_number_slot].ieee_addr;

  z2s_md_identity_t identity;

  switch (Z2S_loadMultiDeviceIdentity(ieee_addr, &identity)) {

    case Z2S_MultiDeviceIdentityResult::Missing:
      snprintf(message, message_size, "Device #%u has no identity file",
               device_number_slot);
      return;

    case Z2S_MultiDeviceIdentityResult::Loaded:
      // The GUID of a separate device would be lost: a new device in Supla
      // Cloud.
      if (identity.mode != Z2S_MD_MODE_GATEWAY) {

        snprintf(
          message, message_size,
          "Device #%u is not using gateway channels - not deleted",
          device_number_slot);
        return;
      }
      break;

    default:
      break;
  }

  snprintf(
    message, message_size, "Identity file of device #%u %s",
    device_number_slot,
    Z2S_deleteMultiDeviceIdentity(ieee_addr) ? "deleted" : "not deleted");
}

/*****************************************************************************/

void Z2S_onMultiDeviceTelnetCmd(uint8_t params_number, char **param) {

  char message[160];

  if (params_number == 0) {

    telnet.printf(
      "\n\r>Multidevice server: %s, new devices: %s\n\r",
      multiDeviceLayer ? multiDeviceLayer->serverName() : "-",
      multiDeviceNewModeNames[Z2S_getMultiDeviceNewMode()]);

    char line[256];

    for (uint8_t devices_counter = 0;
         devices_counter < Z2S_ZB_DEVICES_MAX_NUMBER; devices_counter++) {

      if (!Z2S_isMultiDeviceSlotValid(devices_counter))
        continue;

      Z2S_describeMultiDevice(devices_counter, line, sizeof(line));
      telnet.printf(">%s\n\r", line);
    }
    telnet.printf(">");
    return;
  }

  if ((strcasecmp(param[0], "NEW") == 0) && (params_number > 1)) {

    uint8_t new_mode = 0xFF;

    for (uint8_t i = 0; i <= Z2S_MD_MODE_PENDING; i++)
      if (strcasecmp(param[1], multiDeviceNewModeNames[i]) == 0)
        new_mode = i;

    if (new_mode == 0xFF) {

      telnet.printf("\n\r>MULTIDEVICE NEW GATEWAY|SEPARATE|ASK\n\r>");
      return;
    }

    Z2S_setMultiDeviceNewMode(new_mode, message, sizeof(message));
    telnet.printf("\n\r>%s\n\r>", message);
    return;
  }

  if (params_number < 2) {

    telnet.printf(
      "\n\r>MULTIDEVICE [SEPARATE|GATEWAY|FORGET <device slot>] "
      "[NEW GATEWAY|SEPARATE|ASK]\n\r>");
    return;
  }

  uint8_t device_number_slot = strtoul(param[1], nullptr, 0);

  if (strcasecmp(param[0], "FORGET") == 0)
    Z2S_forgetMultiDevice(device_number_slot, message, sizeof(message));
  else
  if ((strcasecmp(param[0], "SEPARATE") == 0) ||
      (strcasecmp(param[0], "ENABLE") == 0))
    Z2S_setMultiDeviceMode(
      device_number_slot, Z2S_MD_MODE_SEPARATE, message, sizeof(message));
  else
  if ((strcasecmp(param[0], "GATEWAY") == 0) ||
      (strcasecmp(param[0], "DISABLE") == 0))
    Z2S_setMultiDeviceMode(
      device_number_slot, Z2S_MD_MODE_GATEWAY, message, sizeof(message));
  else {

    telnet.printf(
      "\n\r>MULTIDEVICE [SEPARATE|GATEWAY|FORGET <device slot>] "
      "[NEW GATEWAY|SEPARATE|ASK]\n\r>");
    return;
  }
  telnet.printf("\n\r>%s\n\r>", message);
}

/*****************************************************************************/
// "Multidevice" tab of the web GUI. ESPUI callbacks run in the web server
// task, so the actions are queued and done in the main loop
// (Z2S_iterateMultiDeviceGUI).

enum multidevice_gui_actions_t : uint8_t {

  MD_GUI_SEPARATE,
  MD_GUI_GATEWAY,
  MD_GUI_FORGET,
  MD_GUI_NEW_MODE,
  MD_GUI_REFRESH
};

typedef struct multidevice_gui_command_s {

  uint8_t action;
  int16_t value;
} multidevice_gui_command_t;

static QueueHandle_t multiDeviceGUIQueue = nullptr;

static uint16_t multiDeviceStatusLabel = 0xFFFF;
static uint16_t multiDeviceMessageLabel = 0xFFFF;

static volatile int16_t multiDeviceSelectedSlot = -1;

// ESPUI keeps pointers to labels.
static String multiDeviceOptionLabels[Z2S_ZB_DEVICES_MAX_NUMBER];

static uint32_t multiDeviceGUIRefreshMs = 0;

/*****************************************************************************/

static void Z2S_queueMultiDeviceGUICommand(uint8_t action, int16_t value) {

  multidevice_gui_command_t command = { action, value };

  if (multiDeviceGUIQueue)
    xQueueSend(multiDeviceGUIQueue, &command, 0);
}

/*****************************************************************************/

static void Z2S_multiDeviceSelectorCallback(
  BasicControl *sender, int type, void *param) {

  multiDeviceSelectedSlot = sender->getValueInt();
  Z2S_queueMultiDeviceGUICommand(MD_GUI_REFRESH, 0);
}

/*****************************************************************************/

static void Z2S_multiDeviceNewModeCallback(
  BasicControl *sender, int type, void *param) {

  Z2S_queueMultiDeviceGUICommand(MD_GUI_NEW_MODE, sender->getValueInt());
}

/*****************************************************************************/

static void Z2S_multiDeviceButtonCallback(
  BasicControl *sender, int type, void *param) {

  if (type == B_UP)
    Z2S_queueMultiDeviceGUICommand(
      (uint32_t)param, multiDeviceSelectedSlot);
}

/*****************************************************************************/

static void Z2S_refreshMultiDeviceGUI(const char *message) {

  if (multiDeviceStatusLabel == 0xFFFF)
    return;

  String status;
  String pending;
  char line[384];

  status.reserve(4096);
  status =
    "<table style='width:100%;text-align:left;font-size:smaller'>"
    "<tr><th>Slot</th><th>IEEE address</th><th>Name</th><th>Mode</th>"
    "<th>Session</th><th>Channels</th><th>Last result</th><th>Power</th>"
    "<th>Seen</th><th>Timeout</th></tr>";

  for (uint8_t devices_counter = 0;
       devices_counter < Z2S_ZB_DEVICES_MAX_NUMBER; devices_counter++) {

    if (!Z2S_isMultiDeviceSlotValid(devices_counter))
      continue;

    if (Z2S_describeMultiDevice(devices_counter, line, sizeof(line), true) ==
        Z2S_MD_MODE_PENDING) {

      snprintf(line, sizeof(line), " #%02u", devices_counter);
      pending += line;
      Z2S_describeMultiDevice(devices_counter, line, sizeof(line), true);
    }
    status += line;
  }
  status += "</table>";

  if (pending.length() > 0)
    status = "<b>Waiting for choice (gateway channels or separate device):" +
             pending + "</b><br><br>" + status;

  ESPUI.updateLabel(multiDeviceStatusLabel, status);

  if (message)
    ESPUI.updateLabel(multiDeviceMessageLabel, message);
}

/*****************************************************************************/

void Z2S_buildMultiDeviceTabGUI() {

  multiDeviceGUIQueue = xQueueCreate(8, sizeof(multidevice_gui_command_t));

  auto multidevice_tab = ESPUI.addControl(
    Control::Type::Tab, "", "Multidevice", Control::Color::Dark,
    Control::noParent);

  // Labels updated later need String values (a const char * value is kept
  // as a pointer and cannot be changed).
  multiDeviceStatusLabel = ESPUI.addControl(
    Control::Type::Label, "Zigbee devices", String("..."),
    Control::Color::Dark, multidevice_tab);
  ESPUI.getControl(multiDeviceStatusLabel)->reserve(4096);
  ESPUI.setPanelWide(multiDeviceStatusLabel, true);

  auto device_selector = ESPUI.addControl(
    Control::Type::Select, "Zigbee device", (long int)-1,
    Control::Color::Dark, multidevice_tab, Z2S_multiDeviceSelectorCallback);

  ESPUI.addControl(
    Control::Type::Option, "Select device...", (long int)-1,
    Control::Color::None, device_selector);

  // Devices paired later come with the GUI rebuilt after the restart that
  // follows pairing.
  for (uint8_t devices_counter = 0;
       devices_counter < Z2S_ZB_DEVICES_MAX_NUMBER; devices_counter++) {

    if (!Z2S_isMultiDeviceSlotValid(devices_counter))
      continue;

    char name[SUPLA_DEVICE_NAME_MAXSIZE];
    Z2S_getMultiDeviceName(devices_counter, name, sizeof(name));

    multiDeviceOptionLabels[devices_counter] =
      String("#") + devices_counter + " " + name;

    ESPUI.addControl(
      Control::Type::Option, multiDeviceOptionLabels[devices_counter].c_str(),
      (long int)devices_counter, Control::Color::None, device_selector);
  }

  ESPUI.addControl(
    Control::Type::Button, "", "Separate Supla device", Control::Color::Dark,
    device_selector, Z2S_multiDeviceButtonCallback, (void *)MD_GUI_SEPARATE);

  ESPUI.addControl(
    Control::Type::Button, "", "Gateway channels", Control::Color::Dark,
    device_selector, Z2S_multiDeviceButtonCallback, (void *)MD_GUI_GATEWAY);

  ESPUI.addControl(
    Control::Type::Button, "", "Forget identity", Control::Color::Dark,
    device_selector, Z2S_multiDeviceButtonCallback, (void *)MD_GUI_FORGET);

  multiDeviceMessageLabel = ESPUI.addControl(
    Control::Type::Label, "", emptyString, Control::Color::Dark,
    device_selector);
  ESPUI.getControl(multiDeviceMessageLabel)->reserve(160);

  auto new_mode_selector = ESPUI.addControl(
    Control::Type::Select, "New Zigbee devices",
    (long int)Z2S_getMultiDeviceNewMode(), Control::Color::Dark,
    multidevice_tab, Z2S_multiDeviceNewModeCallback);

  ESPUI.addControl(
    Control::Type::Option, "Gateway channels", (long int)Z2S_MD_MODE_GATEWAY,
    Control::Color::None, new_mode_selector);

  ESPUI.addControl(
    Control::Type::Option, "Separate Supla device",
    (long int)Z2S_MD_MODE_SEPARATE, Control::Color::None, new_mode_selector);

  ESPUI.addControl(
    Control::Type::Option, "Ask after pairing", (long int)Z2S_MD_MODE_PENDING,
    Control::Color::None, new_mode_selector);
}

/*****************************************************************************/

static void Z2S_iterateMultiDeviceGUI() {

  if (multiDeviceGUIQueue == nullptr)
    return;

  multidevice_gui_command_t command;
  char message[160] = {};
  bool refresh = false;

  while (xQueueReceive(multiDeviceGUIQueue, &command, 0) == pdTRUE) {

    refresh = true;

    switch (command.action) {

      case MD_GUI_SEPARATE:
        Z2S_setMultiDeviceMode(
          command.value, Z2S_MD_MODE_SEPARATE, message, sizeof(message));
        break;

      case MD_GUI_GATEWAY:
        Z2S_setMultiDeviceMode(
          command.value, Z2S_MD_MODE_GATEWAY, message, sizeof(message));
        break;

      case MD_GUI_FORGET:
        Z2S_forgetMultiDevice(command.value, message, sizeof(message));
        break;

      case MD_GUI_NEW_MODE:
        if ((command.value >= Z2S_MD_MODE_GATEWAY) &&
            (command.value <= Z2S_MD_MODE_PENDING))
          Z2S_setMultiDeviceNewMode(command.value, message, sizeof(message));
        break;

      default:
        break;
    }
  }

  // Session states and times change by themselves.
  if (refresh || (millis() - multiDeviceGUIRefreshMs > 10000)) {

    multiDeviceGUIRefreshMs = millis();
    Z2S_refreshMultiDeviceGUI(message[0] ? message : nullptr);
  }
}
