// SPDX-License-Identifier: GPL-2.0-or-later

#include "Z2S_MultiDeviceSession.h"

#include <supla-common/srpc.h>
#include <string.h>

#include <algorithm>

#include "Z2S_MultiDeviceLog.h"

namespace {

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kRegisterTimeoutMs = 15000;
constexpr uint32_t kMinReconnectDelayMs = 5000;
constexpr uint32_t kMaxReconnectDelayMs = 5 * 60 * 1000;
// srpc_iterate() handles at most one packet per call.
constexpr int kMaxPacketsPerIterate = 16;

// srpc_ds_async_registerdevice_in_chunks_g() takes a callback without a user
// parameter. Registration is synchronous and single threaded, so the channels
// of the session being registered are kept here for the time of the call.
std::vector<TDS_SuplaDeviceChannel_E> *register_channels = nullptr;

TDS_SuplaDeviceChannel_E *getRegisterChannel(int index) {
  if (register_channels == nullptr || index < 0 ||
      static_cast<size_t>(index) >= register_channels->size()) {
    return nullptr;
  }
  return &(*register_channels)[index];
}

_supla_int_t srpcDataRead(void *buf, _supla_int_t count, void *user_params) {
  return static_cast<Z2S_MultiDeviceSession *>(user_params)
      ->onDataRead(buf, count);
}

_supla_int_t srpcDataWrite(void *buf, _supla_int_t count, void *user_params) {
  return static_cast<Z2S_MultiDeviceSession *>(user_params)
      ->onDataWrite(buf, count);
}

void srpcRemoteCall(void *, unsigned _supla_int_t, unsigned _supla_int_t,
                    void *user_params, unsigned char) {
  static_cast<Z2S_MultiDeviceSession *>(user_params)->onRemoteCall();
}

void copyString(char *dst, size_t dst_size, const std::string &src) {
  size_t len = std::min(src.size(), dst_size - 1);
  memcpy(dst, src.data(), len);
  dst[len] = '\0';
}

// millis() based times wrap around after 49 days.
bool timeReached(uint32_t now_ms, uint32_t time_ms) {
  return static_cast<int32_t>(now_ms - time_ms) >= 0;
}

bool sameLayout(const TDS_SuplaDeviceChannel_E &a,
                const TDS_SuplaDeviceChannel_E &b) {
  return a.Type == b.Type && a.FuncList == b.FuncList &&
         a.Default == b.Default && a.Flags == b.Flags &&
         a.DefaultIcon == b.DefaultIcon;
}

}  // namespace

const char *Z2S_MultiDeviceResultCodeToString(int result_code) {
  switch (result_code) {
    case SUPLA_RESULTCODE_TRUE:
      return "OK";
    case SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE:
      return "server temporarily unavailable";
    case SUPLA_RESULTCODE_BAD_CREDENTIALS:
      return "bad credentials";
    case SUPLA_RESULTCODE_LOCATION_CONFLICT:
      return "location conflict";
    case SUPLA_RESULTCODE_CHANNEL_CONFLICT:
      return "channel conflict";
    case SUPLA_RESULTCODE_DEVICE_DISABLED:
      return "device disabled";
    case SUPLA_RESULTCODE_LOCATION_DISABLED:
      return "location disabled";
    case SUPLA_RESULTCODE_DEVICE_LIMITEXCEEDED:
      return "device limit exceeded";
    case SUPLA_RESULTCODE_GUID_ERROR:
      return "invalid GUID";
    case SUPLA_RESULTCODE_DEVICE_LOCKED:
      return "device locked";
    case SUPLA_RESULTCODE_REGISTRATION_DISABLED:
      return "registration disabled";
    case SUPLA_RESULTCODE_AUTHKEY_ERROR:
      return "invalid AuthKey";
    case SUPLA_RESULTCODE_NO_LOCATION_AVAILABLE:
      return "no location available";
    case SUPLA_RESULTCODE_UNAUTHORIZED:
      return "unauthorized";
    case SUPLA_RESULTCODE_RESTART_REQUESTED:
      return "OK (restart requested)";
    case SUPLA_RESULTCODE_IDENTIFY_REQUESTED:
      return "OK (identify requested)";
  }
  return "unknown result code";
}

Z2S_MultiDeviceSession::Z2S_MultiDeviceSession(
    const Z2S_MultiDeviceServerConfig *config,
    Z2S_MultiDeviceTransportFactory *factory,
    Z2S_MultiDeviceSessionListener *listener)
    : config_(config), factory_(factory), listener_(listener) {}

Z2S_MultiDeviceSession::~Z2S_MultiDeviceSession() {
  // No listener calls from the destructor.
  listener_ = nullptr;
  disconnect();
}

void Z2S_MultiDeviceSession::setIdentity(
    const uint8_t guid[SUPLA_GUID_SIZE],
    const uint8_t auth_key[SUPLA_AUTHKEY_SIZE]) {
  memcpy(guid_, guid, SUPLA_GUID_SIZE);
  memcpy(auth_key_, auth_key, SUPLA_AUTHKEY_SIZE);
  // Seeds the reconnect jitter, so that sessions do not reconnect in sync.
  rng_state_ = 2166136261u;
  for (uint8_t b : guid_) rng_state_ = (rng_state_ ^ b) * 16777619u;
  if (rng_state_ == 0) rng_state_ = 1;
}

void Z2S_MultiDeviceSession::setName(const char *name) {
  if (name_ == name) return;
  name_ = name;
  if (state_ == State::Registered) {
    Z2S_MD_LOG_I("[%s] Name changed, registering again", name_.c_str());
    layout_changed_ = true;
  }
}

void Z2S_MultiDeviceSession::setChannels(
    const TDS_SuplaDeviceChannel_E *channels, int count) {
  bool changed = count != static_cast<int>(channels_.size());
  std::vector<Channel> updated(count);
  for (int i = 0; i < count; i++) {
    if (i < static_cast<int>(channels_.size())) {
      if (sameLayout(channels_[i].reg, channels[i])) {
        updated[i] = channels_[i];
        continue;
      }
      changed = true;
    }
    updated[i].reg = channels[i];
    updated[i].reg.Number = static_cast<unsigned char>(i);
    updated[i].reg.SubDeviceId = 0;
  }
  channels_ = std::move(updated);
  if (changed && state_ == State::Registered) {
    Z2S_MD_LOG_I("[%s] Channel layout changed, registering again",
                 name_.c_str());
    layout_changed_ = true;
  }
}

void Z2S_MultiDeviceSession::setChannelValue(int channel_number,
                                             const int8_t *value,
                                             uint8_t offline,
                                             uint32_t validity_time_sec) {
  if (!isValidChannel(channel_number)) return;
  auto &reg = channels_[channel_number].reg;
  memcpy(reg.value, value, SUPLA_CHANNELVALUE_SIZE);
  reg.Offline = offline;
  reg.ValueValidityTimeSec = validity_time_sec;
}

void Z2S_MultiDeviceSession::setChannelCaption(int channel_number,
                                               const char *caption) {
  if (!isValidChannel(channel_number)) return;
  auto &channel = channels_[channel_number];
  if (channel.caption == caption) return;
  channel.caption = caption;
  channel.caption_pending = state_ == State::Registered;
}

bool Z2S_MultiDeviceSession::sendExtendedValue(
    int channel_number, TSuplaChannelExtendedValue *value) {
  if (!isRegistered() || !isValidChannel(channel_number)) return false;
  srpc_ds_async_channel_extendedvalue_changed(
      srpc_, static_cast<unsigned char>(channel_number), value);
  return true;
}

bool Z2S_MultiDeviceSession::sendActionTrigger(int channel_number,
                                               uint32_t action) {
  if (!isRegistered() || !isValidChannel(channel_number)) return false;
  TDS_ActionTrigger at = {};
  at.ChannelNumber = static_cast<unsigned char>(channel_number);
  at.ActionTrigger = static_cast<_supla_int_t>(action);
  srpc_ds_async_action_trigger(srpc_, &at);
  return true;
}

bool Z2S_MultiDeviceSession::setChannelConfig(int channel_number,
                                              int32_t function,
                                              const void *config, int size,
                                              uint8_t config_type) {
  if (!isRegistered() || !isValidChannel(channel_number) || size < 0 ||
      size > SUPLA_CHANNEL_CONFIG_MAXSIZE) {
    return false;
  }
  TSDS_SetChannelConfig request = {};
  request.ChannelNumber = static_cast<unsigned char>(channel_number);
  request.Func = function;
  request.ConfigType = config_type;
  request.ConfigSize = static_cast<unsigned _supla_int16_t>(size);
  memcpy(request.Config, config, size);
  srpc_ds_async_set_channel_config_request(srpc_, &request);
  return true;
}

bool Z2S_MultiDeviceSession::getChannelConfig(int channel_number,
                                              uint8_t config_type) {
  if (!isRegistered() || !isValidChannel(channel_number)) return false;
  TDS_GetChannelConfigRequest request = {};
  request.ChannelNumber = static_cast<unsigned char>(channel_number);
  request.ConfigType = config_type;
  srpc_ds_async_get_channel_config_request(srpc_, &request);
  return true;
}

bool Z2S_MultiDeviceSession::sendChannelState(TDSC_ChannelState *state) {
  if (!isRegistered() || !isValidChannel(state->ChannelNumber)) return false;
  srpc_csd_async_channel_state_result(srpc_, state);
  return true;
}

bool Z2S_MultiDeviceSession::wantsToConnect(uint32_t now_ms) const {
  return state_ == State::Idle && !suspended_ && !channels_.empty() &&
         (!reconnect_scheduled_ || timeReached(now_ms, next_connect_ms_));
}

void Z2S_MultiDeviceSession::setSuspended(bool suspended) {
  if (suspended_ == suspended) return;
  suspended_ = suspended;
  if (suspended) {
    Z2S_MD_LOG_I("[%s] ZigBee device unreachable, disconnecting",
                 name_.c_str());
    disconnect();
  } else {
    Z2S_MD_LOG_I("[%s] ZigBee device reachable again", name_.c_str());
    reconnect_scheduled_ = false;
  }
}

void Z2S_MultiDeviceSession::disable() {
  disconnect();
  state_ = State::Disabled;
}

void Z2S_MultiDeviceSession::enable() {
  if (state_ == State::Disabled) {
    state_ = State::Idle;
    reconnect_scheduled_ = false;
  }
}

uint32_t Z2S_MultiDeviceSession::jitter(uint32_t max_ms) {
  if (max_ms == 0) return 0;
  // xorshift32
  rng_state_ ^= rng_state_ << 13;
  rng_state_ ^= rng_state_ >> 17;
  rng_state_ ^= rng_state_ << 5;
  return rng_state_ % max_ms;
}

void Z2S_MultiDeviceSession::startConnect(uint32_t now_ms) {
  now_ms_ = now_ms;
  disconnect();
  transport_ = factory_->create();
  if (!transport_ ||
      !transport_->connect(config_->server.c_str(), config_->port)) {
    Z2S_MD_LOG_W("[%s] Cannot connect to %s:%d", name_.c_str(),
                 config_->server.c_str(), config_->port);
    disconnect();
    scheduleReconnectAfterFailure(now_ms);
    return;
  }
  state_ = State::Connecting;
  state_since_ms_ = now_ms;
  Z2S_MD_LOG_D("[%s] Connecting to %s:%d", name_.c_str(),
               config_->server.c_str(), config_->port);
}

void Z2S_MultiDeviceSession::disconnect() {
  bool was_connected = srpc_ != nullptr;
  if (srpc_ != nullptr) {
    srpc_free(srpc_);
    srpc_ = nullptr;
  }
  if (transport_) {
    transport_->close();
    transport_.reset();
  }
  for (auto &channel : channels_) channel.sent = false;
  write_failed_ = false;
  close_requested_ = false;
  if (state_ != State::Disabled) state_ = State::Idle;
  if (was_connected && listener_ != nullptr) {
    listener_->onSessionDisconnected(this);
  }
}

void Z2S_MultiDeviceSession::scheduleReconnect(uint32_t now_ms,
                                               uint32_t delay_ms) {
  next_connect_ms_ = now_ms + delay_ms + jitter(delay_ms / 4 + 1);
  reconnect_scheduled_ = true;
}

void Z2S_MultiDeviceSession::scheduleReconnectAfterFailure(uint32_t now_ms) {
  uint32_t delay = kMinReconnectDelayMs;
  for (int i = 0; i < failure_count_ && delay < kMaxReconnectDelayMs; i++) {
    delay *= 2;
  }
  delay = std::min(delay, kMaxReconnectDelayMs);
  if (failure_count_ < 16) failure_count_++;
  scheduleReconnect(now_ms, delay);
}

int Z2S_MultiDeviceSession::onDataRead(void *buf, int count) {
  if (!transport_) return 0;
  int r = transport_->read(buf, count);
  if (r > 0) last_response_ms_ = now_ms_;
  return r;
}

int Z2S_MultiDeviceSession::onDataWrite(void *buf, int count) {
  if (!transport_ || write_failed_) return -1;
  if (!transport_->write(buf, count)) {
    write_failed_ = true;
    return -1;
  }
  last_sent_ms_ = now_ms_;
  return count;
}

void Z2S_MultiDeviceSession::sendRegistration() {
  TDS_SuplaRegisterDeviceHeader header = {};
  copyString(header.Email, sizeof(header.Email), config_->email);
  memcpy(header.AuthKey, auth_key_, SUPLA_AUTHKEY_SIZE);
  memcpy(header.GUID, guid_, SUPLA_GUID_SIZE);
  copyString(header.Name, sizeof(header.Name), name_);
  copyString(header.SoftVer, sizeof(header.SoftVer), config_->soft_ver);
  copyString(header.ServerName, sizeof(header.ServerName), config_->server);
  // No device flags: they make the server send calls that need an answer
  // (device config, subdevice pairing...), which belong to the gateway.
  header.Flags = 0;
  header.channel_count = static_cast<unsigned char>(channels_.size());

  std::vector<TDS_SuplaDeviceChannel_E> reg_channels(channels_.size());
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    reg_channels[i] = channel.reg;
    memcpy(channel.sent_value, channel.reg.value, SUPLA_CHANNELVALUE_SIZE);
    channel.sent_offline = channel.reg.Offline;
    channel.sent_validity_time_sec = channel.reg.ValueValidityTimeSec;
    channel.sent = true;
  }

  Z2S_MD_LOG_I("[%s] Registering (%d channel(s))", name_.c_str(),
               static_cast<int>(channels_.size()));
  register_channels = &reg_channels;
  _supla_int_t result =
      srpc_ds_async_registerdevice_in_chunks_g(srpc_, &header,
                                               &getRegisterChannel);
  register_channels = nullptr;
  if (!result) {
    Z2S_MD_LOG_E("[%s] Failed to send registration", name_.c_str());
    close_requested_ = true;
  }
}

void Z2S_MultiDeviceSession::handleRegisterResult(int result_code,
                                                  int activity_timeout) {
  last_result_code_ = result_code;
  if (result_code == SUPLA_RESULTCODE_TRUE ||
      result_code == SUPLA_RESULTCODE_RESTART_REQUESTED ||
      result_code == SUPLA_RESULTCODE_IDENTIFY_REQUESTED) {
    Z2S_MD_LOG_I("[%s] Registered (activity timeout %d s)", name_.c_str(),
                 activity_timeout);
    state_ = State::Registered;
    state_since_ms_ = now_ms_;
    failure_count_ = 0;
    for (auto &channel : channels_) {
      channel.caption_pending = !channel.caption.empty();
    }
    activity_timeout_s_ = activity_timeout > 0 ? activity_timeout : 30;
    if (activity_timeout_s_ != config_->activity_timeout_s) {
      TDCS_SuplaSetActivityTimeout at = {};
      at.activity_timeout = static_cast<unsigned char>(
          std::min(std::max(config_->activity_timeout_s, 6), 255));
      srpc_dcs_async_set_activity_timeout(srpc_, &at);
    }
    if (listener_ != nullptr) listener_->onSessionRegistered(this);
    return;
  }

  Z2S_MD_LOG_E("[%s] Registration failed: %s (%d)", name_.c_str(),
               Z2S_MultiDeviceResultCodeToString(result_code), result_code);
  close_requested_ = true;
  uint32_t delay;
  switch (result_code) {
    case SUPLA_RESULTCODE_REGISTRATION_DISABLED:
      // The user has to enable registration in Supla Cloud; retry often
      // enough to catch the registration window.
      delay = 60 * 1000;
      break;
    case SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE:
      delay = 30 * 1000;
      break;
    default:
      delay = 10 * 60 * 1000;
      break;
  }
  scheduleReconnect(now_ms_, delay);
}

void Z2S_MultiDeviceSession::sendChannelState(int channel_number,
                                              int32_t receiver_id) {
  TDSC_ChannelState state = {};
  state.ReceiverID = receiver_id;
  state.ChannelNumber = static_cast<unsigned char>(channel_number);
  if (listener_ != nullptr) listener_->onGetChannelState(this, &state);
  srpc_csd_async_channel_state_result(srpc_, &state);
}

void Z2S_MultiDeviceSession::onRemoteCall() {
  TsrpcReceivedData rd;
  if (srpc_getdata(srpc_, &rd, 0) != SUPLA_RESULT_TRUE) return;
  switch (rd.call_id) {
    case SUPLA_SDC_CALL_VERSIONERROR:
      Z2S_MD_LOG_E("[%s] Protocol version error (server min %d, max %d)",
                   name_.c_str(), rd.data.sdc_version_error->server_version_min,
                   rd.data.sdc_version_error->server_version);
      close_requested_ = true;
      scheduleReconnect(now_ms_, 10 * 60 * 1000);
      break;
    case SUPLA_SD_CALL_REGISTER_DEVICE_RESULT:
      handleRegisterResult(rd.data.sd_register_device_result->result_code,
                           rd.data.sd_register_device_result->activity_timeout);
      break;
    case SUPLA_SD_CALL_REGISTER_DEVICE_RESULT_B:
      handleRegisterResult(
          rd.data.sd_register_device_result_b->result_code,
          rd.data.sd_register_device_result_b->activity_timeout);
      break;
    case SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT:
      activity_timeout_s_ =
          rd.data.sdc_set_activity_timeout_result->activity_timeout;
      Z2S_MD_LOG_D("[%s] Activity timeout set to %d s", name_.c_str(),
                   activity_timeout_s_);
      break;
    case SUPLA_SDC_CALL_PING_SERVER_RESULT:
      break;
    case SUPLA_SD_CALL_CHANNEL_SET_VALUE: {
      auto *new_value = rd.data.sd_channel_new_value;
      if (new_value == nullptr || !isValidChannel(new_value->ChannelNumber)) {
        break;
      }
      int result = listener_ != nullptr
                       ? listener_->onNewValue(this, new_value, false)
                       : -1;
      if (result != -1) {
        srpc_ds_async_set_channel_result(srpc_, new_value->ChannelNumber,
                                         new_value->SenderID, result);
      }
      break;
    }
    case SUPLA_SD_CALL_CHANNELGROUP_SET_VALUE: {
      auto *group_value = rd.data.sd_channelgroup_new_value;
      if (group_value == nullptr ||
          !isValidChannel(group_value->ChannelNumber) || listener_ == nullptr) {
        break;
      }
      TSD_SuplaChannelNewValue new_value = {};
      new_value.ChannelNumber = group_value->ChannelNumber;
      new_value.DurationMS = group_value->DurationMS;
      memcpy(new_value.value, group_value->value, SUPLA_CHANNELVALUE_SIZE);
      listener_->onNewValue(this, &new_value, true);
      break;
    }
    case SUPLA_CSD_CALL_GET_CHANNEL_STATE: {
      auto *request = rd.data.csd_channel_state_request;
      if (request != nullptr && isValidChannel(request->ChannelNumber)) {
        sendChannelState(request->ChannelNumber, request->SenderID);
      }
      break;
    }
    case SUPLA_SD_CALL_DEVICE_CALCFG_REQUEST: {
      auto *request = rd.data.sd_device_calcfg_request;
      if (request == nullptr) break;
      TDS_DeviceCalCfgResult result = {};
      result.ReceiverID = request->SenderID;
      result.ChannelNumber = request->ChannelNumber;
      result.Command = request->Command;
      result.Result = SUPLA_CALCFG_RESULT_NOT_SUPPORTED;
      // Calls for the whole device (ChannelNumber -1) are not supported.
      if (isValidChannel(request->ChannelNumber) && listener_ != nullptr) {
        result.Result = listener_->onCalCfgRequest(this, request, &result);
      }
      if (result.Result >= 0) {
        srpc_ds_async_device_calcfg_result(srpc_, &result);
      }
      break;
    }
    case SUPLA_SD_CALL_SET_CHANNEL_CONFIG: {
      auto *request = rd.data.sds_set_channel_config_request;
      if (request == nullptr) break;
      TSDS_SetChannelConfigResult result = {};
      result.ChannelNumber = request->ChannelNumber;
      result.ConfigType = request->ConfigType;
      result.Result = SUPLA_RESULTCODE_CHANNELNOTFOUND;
      if (isValidChannel(request->ChannelNumber) && listener_ != nullptr) {
        result.Result = static_cast<unsigned char>(
            listener_->onSetChannelConfig(this, request));
      }
      srpc_ds_async_set_channel_config_result(srpc_, &result);
      break;
    }
    case SUPLA_SD_CALL_GET_CHANNEL_CONFIG_RESULT: {
      auto *config = rd.data.sd_channel_config;
      if (config != nullptr && isValidChannel(config->ChannelNumber) &&
          listener_ != nullptr) {
        listener_->onChannelConfig(this, config);
      }
      break;
    }
    case SUPLA_SD_CALL_SET_CHANNEL_CONFIG_RESULT: {
      auto *result = rd.data.sds_set_channel_config_result;
      if (result != nullptr && isValidChannel(result->ChannelNumber) &&
          listener_ != nullptr) {
        listener_->onSetChannelConfigResult(this, result);
      }
      break;
    }
    case SUPLA_SD_CALL_CHANNEL_CONFIG_FINISHED: {
      auto *finished = rd.data.sd_channel_config_finished;
      if (finished != nullptr && isValidChannel(finished->ChannelNumber) &&
          listener_ != nullptr) {
        listener_->onChannelConfigFinished(this, finished->ChannelNumber);
      }
      break;
    }
    case SUPLA_SCD_CALL_SET_CHANNEL_CAPTION_RESULT:
      break;
    default:
      Z2S_MD_LOG_D("[%s] Ignoring call %d from server", name_.c_str(),
                   static_cast<int>(rd.call_id));
      break;
  }
  srpc_rd_free(&rd);
}

void Z2S_MultiDeviceSession::pushChannelValues() {
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    const auto &reg = channel.reg;
    if (channel.sent && channel.sent_offline == reg.Offline &&
        channel.sent_validity_time_sec == reg.ValueValidityTimeSec &&
        memcmp(channel.sent_value, reg.value, SUPLA_CHANNELVALUE_SIZE) == 0) {
      continue;
    }
    srpc_ds_async_channel_value_changed_c(
        srpc_, static_cast<unsigned char>(i), const_cast<char *>(reg.value),
        reg.Offline, reg.ValueValidityTimeSec);
    memcpy(channel.sent_value, reg.value, SUPLA_CHANNELVALUE_SIZE);
    channel.sent_offline = reg.Offline;
    channel.sent_validity_time_sec = reg.ValueValidityTimeSec;
    channel.sent = true;
  }
}

void Z2S_MultiDeviceSession::pushCaptions() {
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    if (!channel.caption_pending) continue;
    channel.caption_pending = false;
    TDCS_SetCaption caption = {};
    caption.ChannelNumber = static_cast<_supla_int_t>(i);
    copyString(caption.Caption, sizeof(caption.Caption), channel.caption);
    caption.CaptionSize = static_cast<unsigned _supla_int_t>(
        strnlen(caption.Caption, sizeof(caption.Caption)) + 1);
    srpc_dcs_async_set_channel_caption(srpc_, &caption);
  }
}

void Z2S_MultiDeviceSession::ping(uint32_t now_ms) {
  uint32_t timeout_ms = static_cast<uint32_t>(activity_timeout_s_) * 1000;
  if (now_ms - last_response_ms_ >= timeout_ms + 10000) {
    Z2S_MD_LOG_W("[%s] No response from server, reconnecting", name_.c_str());
    close_requested_ = true;
    scheduleReconnectAfterFailure(now_ms);
    return;
  }
  uint32_t idle_threshold_ms =
      timeout_ms > 5000 ? timeout_ms - 5000 : timeout_ms;
  if (now_ms - last_ping_ms_ >= 5000 &&
      (now_ms - last_response_ms_ >= idle_threshold_ms ||
       now_ms - last_sent_ms_ >= idle_threshold_ms)) {
    last_ping_ms_ = now_ms;
    srpc_dcs_async_ping_server(srpc_);
  }
}

void Z2S_MultiDeviceSession::iterate(uint32_t now_ms) {
  now_ms_ = now_ms;
  switch (state_) {
    case State::Idle:
    case State::Disabled:
      return;

    case State::Connecting: {
      auto status = transport_->status();
      if (status == Z2S_MultiDeviceTransportStatus::Connected) {
        TsrpcParams params;
        srpc_params_init(&params);
        params.data_read = &srpcDataRead;
        params.data_write = &srpcDataWrite;
        params.on_remote_call_received = &srpcRemoteCall;
        params.user_params = this;
        srpc_ = srpc_init(&params);
        srpc_set_proto_version(
            srpc_, static_cast<unsigned char>(config_->proto_version));
        state_ = State::Registering;
        state_since_ms_ = now_ms;
        last_response_ms_ = now_ms;
        last_sent_ms_ = now_ms;
        last_ping_ms_ = now_ms;
        layout_changed_ = false;
        sendRegistration();
      } else if (status == Z2S_MultiDeviceTransportStatus::Failed ||
                 now_ms - state_since_ms_ > kConnectTimeoutMs) {
        Z2S_MD_LOG_W("[%s] Connection to %s failed", name_.c_str(),
                     config_->server.c_str());
        disconnect();
        scheduleReconnectAfterFailure(now_ms);
        return;
      }
      break;
    }

    case State::Registering:
    case State::Registered: {
      for (int i = 0;
           i < kMaxPacketsPerIterate && srpc_ != nullptr && !close_requested_;
           i++) {
        if (srpc_iterate(srpc_) != SUPLA_RESULT_TRUE) {
          if (!close_requested_) {
            Z2S_MD_LOG_W("[%s] Connection closed", name_.c_str());
            close_requested_ = true;
            scheduleReconnectAfterFailure(now_ms);
          }
          break;
        }
        if (transport_ &&
            transport_->status() != Z2S_MultiDeviceTransportStatus::Connected) {
          break;
        }
      }
      if (!close_requested_ && state_ == State::Registering &&
          now_ms - state_since_ms_ > kRegisterTimeoutMs) {
        Z2S_MD_LOG_W("[%s] No reply to registration", name_.c_str());
        close_requested_ = true;
        scheduleReconnectAfterFailure(now_ms);
      }
      if (!close_requested_ && state_ == State::Registered) {
        if (layout_changed_) {
          close_requested_ = true;
          scheduleReconnect(now_ms, 0);
        } else {
          pushChannelValues();
          pushCaptions();
          ping(now_ms);
        }
      }
      break;
    }
  }

  if (transport_ && !close_requested_ && !transport_->flush()) {
    write_failed_ = true;
  }
  if (write_failed_ && !close_requested_) {
    Z2S_MD_LOG_W("[%s] Write failed, reconnecting", name_.c_str());
    close_requested_ = true;
    scheduleReconnectAfterFailure(now_ms);
  }
  if (close_requested_) {
    if (transport_) transport_->flush();
    disconnect();
  }
}
