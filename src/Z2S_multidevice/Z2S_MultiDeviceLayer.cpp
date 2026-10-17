// SPDX-License-Identifier: GPL-2.0-or-later

#include "Z2S_MultiDeviceLayer.h"

#include <SuplaDevice.h>
#include <supla/channels/channel.h>
#include <supla/device/register_device.h>
#include <supla/element.h>
#include <supla/network/network.h>
#include <supla/protocol/supla_srpc.h>
#include <supla/storage/config.h>
#include <supla/storage/storage.h>
#include <string.h>

#include "Z2S_MultiDeviceLog.h"

namespace {

// Minimal delay between starting two connections.
constexpr uint32_t kConnectIntervalMs = 250;
constexpr uint8_t kNoDevice = 0xFF;

}  // namespace

Z2S_MultiDeviceLayer::Z2S_MultiDeviceLayer(SuplaDeviceClass *sdc,
                                           const char *supla_ca_cert,
                                           const char *supla_3rd_party_ca_cert)
    : ProtocolLayer(sdc),
      supla_ca_cert_(supla_ca_cert),
      supla_3rd_party_ca_cert_(supla_3rd_party_ca_cert) {
  memset(device_of_channel_, kNoDevice, sizeof(device_of_channel_));
  memset(local_of_channel_, 0, sizeof(local_of_channel_));
}

Z2S_MultiDeviceLayer::~Z2S_MultiDeviceLayer() {
  devices_.clear();
  delete[] custom_ca_cert_;
}

bool Z2S_MultiDeviceLayer::addDevice(
    const uint8_t guid[SUPLA_GUID_SIZE],
    const uint8_t auth_key[SUPLA_AUTHKEY_SIZE], const char *name,
    const uint8_t *channels, int count) {
  if (count <= 0 || devices_.size() >= kNoDevice) return false;
  std::vector<TDS_SuplaDeviceChannel_E> reg(count);
  for (int i = 0; i < count; i++) {
    auto channel = Supla::Channel::GetByChannelNumber(channels[i]);
    if (channel == nullptr || channels[i] >= SUPLA_CHANNELMAXCOUNT ||
        device_of_channel_[channels[i]] != kNoDevice) {
      Z2S_MD_LOG_E("Multidevice: invalid channel %u for %s", channels[i],
                   name);
      return false;
    }
    channel->fillDeviceChannelStruct(&reg[i]);
  }

  Device device;
  device.session.reset(new Z2S_MultiDeviceSession(&config_, &factory_, this));
  device.session->setIdentity(guid, auth_key);
  device.session->setName(name);
  device.session->setChannels(reg.data(), count);
  device.channels.assign(channels, channels + count);
  for (int i = 0; i < count; i++) {
    auto channel = Supla::Channel::GetByChannelNumber(channels[i]);
    channel->setExternallyRegistered(true);
    if (channel->isInitialCaptionSet()) {
      device.session->setChannelCaption(i, channel->getInitialCaption());
    }
    device_of_channel_[channels[i]] = static_cast<uint8_t>(devices_.size());
    local_of_channel_[channels[i]] = static_cast<uint8_t>(i);
  }
  devices_.push_back(std::move(device));
  Z2S_MD_LOG_I("Multidevice: %s added (%d channel(s))", name, count);
  return true;
}

void Z2S_MultiDeviceLayer::onInit() {
  // The gateway's protocol layer has already loaded the config.
  config_.server = Supla::RegisterDevice::getServerName();
  config_.email = Supla::RegisterDevice::getEmail();
  char soft_ver[SUPLA_SOFTVER_MAXSIZE];
  snprintf(soft_ver, sizeof(soft_ver), "Z2S_Library %s",
           Supla::RegisterDevice::getSoftVer());
  config_.soft_ver = soft_ver;

  auto cfg = Supla::Storage::ConfigInstance();
  int32_t port = cfg != nullptr ? cfg->getSuplaServerPort() : -1;
  config_.port = port > 0 ? port : 2016;
  if (config_.port == 2015) {
    factory_.setSsl(false, nullptr);
    return;
  }

  // Certificate selection like in Supla::Protocol::SuplaSrpc.
  uint8_t security_level = 0;
  if (cfg != nullptr) cfg->getUInt8("security_level", &security_level);
  const char *cert = Supla::RegisterDevice::isSuplaPublicServerConfigured()
                         ? supla_ca_cert_
                         : supla_3rd_party_ca_cert_;
  if (security_level == 1 && cfg != nullptr) {
    int len = cfg->getCustomCASize();
    if (len > 0) {
      custom_ca_cert_ = new char[len + 1];
      memset(custom_ca_cert_, 0, len + 1);
      cfg->getCustomCA(custom_ca_cert_, len + 1);
      cert = custom_ca_cert_;
    }
  } else if (security_level == 2) {
    cert = nullptr;
  }
  factory_.setSsl(true, cert);
}

void Z2S_MultiDeviceLayer::disconnect() {
  // The network is down: disconnect now, connect again when it is back.
  for (auto &device : devices_) {
    device.session->disable();
    device.session->enable();
  }
}

bool Z2S_MultiDeviceLayer::iterate(uint32_t _millis) {
  bool connecting = false;
  for (auto &device : devices_) {
    if (device.session->isConnecting()) connecting = true;
  }
  bool registered = false;
  for (auto &device : devices_) {
    auto session = device.session.get();
    // One connection at a time: the TLS handshake needs a lot of memory.
    if (!connecting && session->wantsToConnect(_millis) &&
        _millis - last_connect_ms_ >= kConnectIntervalMs) {
      session->startConnect(_millis);
      last_connect_ms_ = _millis;
      connecting = session->isConnecting();
    }
    session->iterate(_millis);
    if (session->isRegistered()) registered = true;
  }
  return registered;
}

bool Z2S_MultiDeviceLayer::isRegisteredAndReady() {
  for (auto &device : devices_) {
    if (device.session->isRegistered()) return true;
  }
  return false;
}

Z2S_MultiDeviceSession *Z2S_MultiDeviceLayer::findChannel(int channel_number,
                                                          int *local) {
  if (channel_number < 0 || channel_number >= SUPLA_CHANNELMAXCOUNT ||
      device_of_channel_[channel_number] == kNoDevice) {
    return nullptr;
  }
  *local = local_of_channel_[channel_number];
  return devices_[device_of_channel_[channel_number]].session.get();
}

int Z2S_MultiDeviceLayer::globalChannel(Z2S_MultiDeviceSession *session,
                                        int local) {
  for (auto &device : devices_) {
    if (device.session.get() == session) {
      if (local < 0 || local >= static_cast<int>(device.channels.size())) {
        return -1;
      }
      return device.channels[local];
    }
  }
  return -1;
}

void Z2S_MultiDeviceLayer::sendActionTrigger(uint8_t channelNumber,
                                             uint32_t actionId) {
  int local;
  auto session = findChannel(channelNumber, &local);
  if (session) session->sendActionTrigger(local, actionId);
}

void Z2S_MultiDeviceLayer::sendRemainingTimeValue(uint8_t channelNumber,
                                                  uint32_t timeMs,
                                                  uint8_t state,
                                                  int32_t senderId) {
  int local;
  auto session = findChannel(channelNumber, &local);
  if (session == nullptr || !session->isRegistered()) return;
  TSuplaChannelExtendedValue value = {};
  value.type = EV_TYPE_TIMER_STATE_V1;
  value.size = sizeof(TTimerState_ExtendedValue);
  auto timer_state = reinterpret_cast<TTimerState_ExtendedValue *>(value.value);
  timer_state->SenderID = senderId;
  timer_state->TargetValue[0] = state;
  timer_state->RemainingTimeMs = timeMs;
  session->sendExtendedValue(local, &value);
}

void Z2S_MultiDeviceLayer::sendRemainingTimeValue(uint8_t channelNumber,
                                                  uint32_t remainingTime,
                                                  uint8_t *state,
                                                  int32_t senderId,
                                                  bool useSecondsInsteadOfMs) {
  int local;
  auto session = findChannel(channelNumber, &local);
  if (session == nullptr || !session->isRegistered()) return;
  TSuplaChannelExtendedValue value = {};
  value.type = useSecondsInsteadOfMs ? EV_TYPE_TIMER_STATE_V1_SEC
                                     : EV_TYPE_TIMER_STATE_V1;
  value.size = sizeof(TTimerState_ExtendedValue);
  auto timer_state = reinterpret_cast<TTimerState_ExtendedValue *>(value.value);
  timer_state->SenderID = senderId;
  memcpy(timer_state->TargetValue, state, sizeof(timer_state->TargetValue));
  // RemainingTimeMs and RemainingTimeS share the same field.
  timer_state->RemainingTimeMs = remainingTime;
  session->sendExtendedValue(local, &value);
}

void Z2S_MultiDeviceLayer::sendChannelValueChanged(uint8_t channelNumber,
                                                   int8_t *value,
                                                   uint8_t offline,
                                                   uint32_t validityTimeSec) {
  int local;
  auto session = findChannel(channelNumber, &local);
  if (session) session->setChannelValue(local, value, offline, validityTimeSec);
}

void Z2S_MultiDeviceLayer::sendExtendedChannelValueChanged(
    uint8_t channelNumber, TSuplaChannelExtendedValue *value) {
  int local;
  auto session = findChannel(channelNumber, &local);
  if (session) session->sendExtendedValue(local, value);
}

void Z2S_MultiDeviceLayer::getChannelConfig(uint8_t channelNumber,
                                            uint8_t configType) {
  int local;
  auto session = findChannel(channelNumber, &local);
  if (session) session->getChannelConfig(local, configType);
}

bool Z2S_MultiDeviceLayer::setChannelConfig(uint8_t channelNumber,
                                            _supla_int_t channelFunction,
                                            void *channelConfig, int size,
                                            uint8_t configType) {
  int local;
  auto session = findChannel(channelNumber, &local);
  if (session == nullptr) return false;
  return session->setChannelConfig(local, channelFunction, channelConfig, size,
                                   configType);
}

bool Z2S_MultiDeviceLayer::setInitialCaption(uint8_t channelNumber,
                                             const char *caption) {
  int local;
  auto session = findChannel(channelNumber, &local);
  if (session == nullptr) return false;
  session->setChannelCaption(local, caption);
  return true;
}

void Z2S_MultiDeviceLayer::fillStateData(int channel_number,
                                         TDSC_ChannelState *state) {
  // Like Supla::Protocol::SuplaSrpc::sendChannelStateResult().
  if (Supla::Network::Instance() != nullptr) {
    Supla::Network::Instance()->fillStateData(state);
  }
  getSdc()->fillStateData(state);
  auto element = Supla::Element::getElementByChannelNumber(channel_number);
  if (element) element->handleGetChannelState(state);
}

void Z2S_MultiDeviceLayer::sendChannelStateResult(int32_t receiverId,
                                                  uint8_t channelNo) {
  // Requests from the server are answered in onGetChannelState().
  if (receiverId != 0) return;
  int local;
  auto session = findChannel(channelNo, &local);
  if (session == nullptr || !session->isRegistered()) return;
  TDSC_ChannelState state = {};
  state.ChannelNumber = channelNo;
  fillStateData(channelNo, &state);
  state.ChannelNumber = static_cast<unsigned char>(local);
  session->sendChannelState(&state);
}

void Z2S_MultiDeviceLayer::onSessionRegistered(
    Z2S_MultiDeviceSession *session) {
  // Like after the gateway's registration: values, captions, configs and
  // channel states are sent again by the elements (Element::iterateConnected).
  for (auto &device : devices_) {
    if (device.session.get() != session) continue;
    for (uint8_t channel_number : device.channels) {
      auto channel = Supla::Channel::GetByChannelNumber(channel_number);
      if (channel) channel->onRegistered();
    }
  }
}

int Z2S_MultiDeviceLayer::onNewValue(Z2S_MultiDeviceSession *session,
                                     TSD_SuplaChannelNewValue *new_value,
                                     bool from_group) {
  int local = new_value->ChannelNumber;
  int channel_number = globalChannel(session, local);
  auto element = Supla::Element::getElementByChannelNumber(channel_number);
  if (element == nullptr) return -1;
  new_value->ChannelNumber = static_cast<unsigned char>(channel_number);
  int result = element->handleNewValueFromServer(new_value);
  new_value->ChannelNumber = static_cast<unsigned char>(local);
  return from_group ? -1 : result;
}

void Z2S_MultiDeviceLayer::onGetChannelState(Z2S_MultiDeviceSession *session,
                                             TDSC_ChannelState *state) {
  int local = state->ChannelNumber;
  int channel_number = globalChannel(session, local);
  if (channel_number < 0) return;
  state->ChannelNumber = static_cast<unsigned char>(channel_number);
  fillStateData(channel_number, state);
  state->ChannelNumber = static_cast<unsigned char>(local);
}

int Z2S_MultiDeviceLayer::onCalCfgRequest(Z2S_MultiDeviceSession *session,
                                          TSD_DeviceCalCfgRequest *request,
                                          TDS_DeviceCalCfgResult *result) {
  int local = request->ChannelNumber;
  int channel_number = globalChannel(session, local);
  auto element = Supla::Element::getElementByChannelNumber(channel_number);
  if (element == nullptr) return SUPLA_CALCFG_RESULT_NOT_SUPPORTED;
  request->ChannelNumber = channel_number;
  int calcfg_result = element->handleCalcfgFromServer(request);
  request->ChannelNumber = local;
  // Results sent later by the element (pending) go only to the gateway's
  // session, so none is sent here.
  if (calcfg_result == SUPLA_SRPC_CALCFG_RESULT_PENDING) return -1;
  return calcfg_result;
}

uint8_t Z2S_MultiDeviceLayer::dispatchChannelConfig(TSD_ChannelConfig *config,
                                                    int channel_number) {
  auto element = Supla::Element::getElementByChannelNumber(channel_number);
  if (element == nullptr) return SUPLA_RESULTCODE_CHANNELNOTFOUND;
  int local = config->ChannelNumber;
  config->ChannelNumber = static_cast<unsigned char>(channel_number);
  uint8_t result;
  switch (config->ConfigType) {
    case SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE:
      result = element->handleWeeklySchedule(config);
      break;
    case SUPLA_CONFIG_TYPE_ALT_WEEKLY_SCHEDULE:
      result = element->handleWeeklySchedule(config, true);
      break;
    default:
      result = element->handleChannelConfig(config);
      break;
  }
  config->ChannelNumber = static_cast<unsigned char>(local);
  return result;
}

int Z2S_MultiDeviceLayer::onSetChannelConfig(Z2S_MultiDeviceSession *session,
                                             TSDS_SetChannelConfig *config) {
  return dispatchChannelConfig(
      config, globalChannel(session, config->ChannelNumber));
}

void Z2S_MultiDeviceLayer::onChannelConfig(Z2S_MultiDeviceSession *session,
                                           TSD_ChannelConfig *config) {
  dispatchChannelConfig(config, globalChannel(session, config->ChannelNumber));
}

void Z2S_MultiDeviceLayer::onSetChannelConfigResult(
    Z2S_MultiDeviceSession *session, TSDS_SetChannelConfigResult *result) {
  int local = result->ChannelNumber;
  int channel_number = globalChannel(session, local);
  auto element = Supla::Element::getElementByChannelNumber(channel_number);
  if (element == nullptr) return;
  result->ChannelNumber = static_cast<unsigned char>(channel_number);
  element->handleSetChannelConfigResult(result);
  result->ChannelNumber = static_cast<unsigned char>(local);
}

void Z2S_MultiDeviceLayer::onChannelConfigFinished(
    Z2S_MultiDeviceSession *session, int channel_number) {
  auto element = Supla::Element::getElementByChannelNumber(
      globalChannel(session, channel_number));
  if (element) element->handleChannelConfigFinished();
}
