// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Z2S_MULTIDEVICE_SESSION_H_
#define Z2S_MULTIDEVICE_SESSION_H_

#include <stdint.h>
#include <supla-common/proto.h>

#include <memory>
#include <string>
#include <vector>

#include "Z2S_MultiDeviceTransport.h"

struct Z2S_MultiDeviceServerConfig {
  std::string server;
  int port = 2016;
  std::string email;
  int proto_version = 28;
  // Activity timeout requested from the server [s].
  int activity_timeout_s = 120;
  std::string soft_ver;
};

class Z2S_MultiDeviceSession;

// Calls from the server to the device of a session. Channel numbers in the
// passed structures are the session (local) channel numbers.
class Z2S_MultiDeviceSessionListener {
 public:
  virtual ~Z2S_MultiDeviceSessionListener() = default;

  virtual void onSessionRegistered(Z2S_MultiDeviceSession *session) {}
  virtual void onSessionDisconnected(Z2S_MultiDeviceSession *session) {}
  // SUPLA_SD_CALL_CHANNEL_SET_VALUE (from_group == false, the result is sent
  // to the server unless it is -1) and SUPLA_SD_CALL_CHANNELGROUP_SET_VALUE
  // (from_group == true, SenderID == 0, the result is ignored). Returns 1
  // (success), 0 (failure) or -1 (no result), like
  // Supla::Element::handleNewValueFromServer().
  virtual int onNewValue(Z2S_MultiDeviceSession *session,
                         TSD_SuplaChannelNewValue *new_value,
                         bool from_group) {
    return -1;
  }
  // The user opened the channel information in the app. ReceiverID and
  // ChannelNumber are already set.
  virtual void onGetChannelState(Z2S_MultiDeviceSession *session,
                                 TDSC_ChannelState *state) {}
  // Returns SUPLA_CALCFG_RESULT_*; a negative value sends no result.
  virtual int onCalCfgRequest(Z2S_MultiDeviceSession *session,
                              TSD_DeviceCalCfgRequest *request,
                              TDS_DeviceCalCfgResult *result) {
    return SUPLA_CALCFG_RESULT_NOT_SUPPORTED;
  }
  // SUPLA_SD_CALL_SET_CHANNEL_CONFIG (any config type); returns
  // SUPLA_CONFIG_RESULT_*, sent back to the server.
  virtual int onSetChannelConfig(Z2S_MultiDeviceSession *session,
                                 TSDS_SetChannelConfig *config) {
    return SUPLA_CONFIG_RESULT_TYPE_NOT_SUPPORTED;
  }
  // SUPLA_SD_CALL_GET_CHANNEL_CONFIG_RESULT: reply to getChannelConfig().
  virtual void onChannelConfig(Z2S_MultiDeviceSession *session,
                               TSD_ChannelConfig *config) {}
  virtual void onSetChannelConfigResult(Z2S_MultiDeviceSession *session,
                                        TSDS_SetChannelConfigResult *result) {
  }
  virtual void onChannelConfigFinished(Z2S_MultiDeviceSession *session,
                                       int channel_number) {}
};

// One ZigBee device registered in Supla as a separate device: owns its own
// connection, GUID/AuthKey and SRPC instance. It knows nothing about channel
// types: the channel layout and values come ready from supla-device
// elements.
class Z2S_MultiDeviceSession {
 public:
  enum class State { Idle, Connecting, Registering, Registered, Disabled };

  Z2S_MultiDeviceSession(const Z2S_MultiDeviceServerConfig *config,
                         Z2S_MultiDeviceTransportFactory *factory,
                         Z2S_MultiDeviceSessionListener *listener);
  ~Z2S_MultiDeviceSession();
  Z2S_MultiDeviceSession(const Z2S_MultiDeviceSession &) = delete;
  Z2S_MultiDeviceSession &operator=(const Z2S_MultiDeviceSession &) = delete;

  void setIdentity(const uint8_t guid[SUPLA_GUID_SIZE],
                   const uint8_t auth_key[SUPLA_AUTHKEY_SIZE]);
  void setName(const char *name);
  // Channel layout; channel i gets local number i. Value, Offline and
  // ValueValidityTimeSec are the initial value, used until setChannelValue()
  // is called. A change of the layout (or name) on a registered session makes
  // the device register again.
  void setChannels(const TDS_SuplaDeviceChannel_E *channels, int count);
  int channelCount() const { return static_cast<int>(channels_.size()); }

  // Current value of a channel, sent when it changes and with every
  // registration.
  void setChannelValue(int channel_number, const int8_t *value,
                       uint8_t offline, uint32_t validity_time_sec);
  // Initial caption, sent after every registration (the server keeps a
  // caption set by the user).
  void setChannelCaption(int channel_number, const char *caption);
  // The calls below are sent at once and dropped when the session is not
  // registered; they return false then.
  bool sendExtendedValue(int channel_number,
                         TSuplaChannelExtendedValue *value);
  bool sendActionTrigger(int channel_number, uint32_t action);
  bool setChannelConfig(int channel_number, int32_t function,
                        const void *config, int size, uint8_t config_type);
  bool getChannelConfig(int channel_number, uint8_t config_type);

  // Connection lifecycle, driven by the owner.
  bool wantsToConnect(uint32_t now_ms) const;
  void startConnect(uint32_t now_ms);
  void iterate(uint32_t now_ms);
  // Disconnects and stops reconnecting.
  void disable();
  void enable();
  // A suspended session is disconnected and does not reconnect: the ZigBee
  // device is unreachable, so in Supla it looks like a device that was
  // switched off.
  void setSuspended(bool suspended);
  bool isSuspended() const { return suspended_; }

  State state() const { return state_; }
  bool isRegistered() const { return state_ == State::Registered; }
  bool isConnecting() const {
    return state_ == State::Connecting || state_ == State::Registering;
  }
  const char *name() const { return name_.c_str(); }
  int lastResultCode() const { return last_result_code_; }

  // SRPC callbacks (public for the C linkage helpers).
  int onDataRead(void *buf, int count);
  int onDataWrite(void *buf, int count);
  void onRemoteCall();

 private:
  struct Channel {
    TDS_SuplaDeviceChannel_E reg;
    std::string caption;
    bool caption_pending = false;
    bool sent = false;
    char sent_value[SUPLA_CHANNELVALUE_SIZE] = {};
    uint8_t sent_offline = 0;
    uint32_t sent_validity_time_sec = 0;
  };

  void disconnect();
  void scheduleReconnect(uint32_t now_ms, uint32_t delay_ms);
  void scheduleReconnectAfterFailure(uint32_t now_ms);
  void sendRegistration();
  void handleRegisterResult(int result_code, int activity_timeout);
  void pushChannelValues();
  void pushCaptions();
  void sendChannelState(int channel_number, int32_t receiver_id);
  void ping(uint32_t now_ms);
  uint32_t jitter(uint32_t max_ms);
  bool isValidChannel(int channel_number) const {
    return channel_number >= 0 &&
           channel_number < static_cast<int>(channels_.size());
  }

  const Z2S_MultiDeviceServerConfig *config_;
  Z2S_MultiDeviceTransportFactory *factory_;
  Z2S_MultiDeviceSessionListener *listener_;
  std::unique_ptr<Z2S_MultiDeviceTransport> transport_;
  void *srpc_ = nullptr;

  std::string name_;
  uint8_t guid_[SUPLA_GUID_SIZE] = {};
  uint8_t auth_key_[SUPLA_AUTHKEY_SIZE] = {};
  std::vector<Channel> channels_;
  bool suspended_ = false;
  bool layout_changed_ = false;

  State state_ = State::Idle;
  uint32_t now_ms_ = 0;
  uint32_t state_since_ms_ = 0;
  // Without a scheduled reconnect the session connects at once.
  bool reconnect_scheduled_ = false;
  uint32_t next_connect_ms_ = 0;
  uint32_t last_response_ms_ = 0;
  uint32_t last_sent_ms_ = 0;
  uint32_t last_ping_ms_ = 0;
  int activity_timeout_s_ = 30;
  int failure_count_ = 0;
  int last_result_code_ = 0;
  bool write_failed_ = false;
  bool close_requested_ = false;
  uint32_t rng_state_ = 1;
};

const char *Z2S_MultiDeviceResultCodeToString(int result_code);

#endif  // Z2S_MULTIDEVICE_SESSION_H_
