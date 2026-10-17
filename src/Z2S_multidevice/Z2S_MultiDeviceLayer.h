// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Z2S_MULTIDEVICE_LAYER_H_
#define Z2S_MULTIDEVICE_LAYER_H_

#include <supla/protocol/protocol_layer.h>

#include <memory>
#include <vector>

#include "Z2S_MultiDeviceClientTransport.h"
#include "Z2S_MultiDeviceSession.h"

// Protocol layer which registers selected ZigBee devices in Supla as
// separate devices, one session (connection, GUID, AuthKey) per device.
// Their channels stay normal supla-device channels of the gateway, but they
// are excluded from the gateway's own registration
// (Channel::setExternallyRegistered). Channel numbers are translated between
// the gateway (global) and the session (local, 0..n-1).
class Z2S_MultiDeviceLayer : public Supla::Protocol::ProtocolLayer,
                             public Z2S_MultiDeviceSessionListener {
 public:
  Z2S_MultiDeviceLayer(SuplaDeviceClass *sdc, const char *supla_ca_cert,
                       const char *supla_3rd_party_ca_cert);
  ~Z2S_MultiDeviceLayer() override;

  // Adds a device with its channels (gateway channel numbers, in the order of
  // the Supla device's channels). Call before SuplaDevice.begin().
  bool addDevice(const uint8_t guid[SUPLA_GUID_SIZE],
                 const uint8_t auth_key[SUPLA_AUTHKEY_SIZE], const char *name,
                 const uint8_t *channels, int count);
  int deviceCount() const { return static_cast<int>(devices_.size()); }
  Z2S_MultiDeviceSession *session(int index) {
    return devices_[index].session.get();
  }
  const char *serverName() const { return config_.server.c_str(); }

  // ProtocolLayer
  void onInit() override;
  bool onLoadConfig() override { return true; }
  bool verifyConfig() override { return true; }
  bool isEnabled() override { return !devices_.empty(); }
  void disconnect() override;
  bool iterate(uint32_t _millis) override;
  bool isNetworkRestartRequested() override { return false; }
  uint32_t getConnectionFailTime() override { return 0; }
  bool isRegisteredAndReady() override;
  void sendActionTrigger(uint8_t channelNumber, uint32_t actionId) override;
  void sendRemainingTimeValue(uint8_t channelNumber, uint32_t timeMs,
                              uint8_t state, int32_t senderId) override;
  void sendRemainingTimeValue(uint8_t channelNumber, uint32_t remainingTime,
                              uint8_t *state, int32_t senderId,
                              bool useSecondsInsteadOfMs) override;
  void sendChannelValueChanged(uint8_t channelNumber, int8_t *value,
                               uint8_t offline,
                               uint32_t validityTimeSec) override;
  void sendExtendedChannelValueChanged(
      uint8_t channelNumber, TSuplaChannelExtendedValue *value) override;
  void getChannelConfig(uint8_t channelNumber, uint8_t configType) override;
  bool setChannelConfig(uint8_t channelNumber, _supla_int_t channelFunction,
                        void *channelConfig, int size,
                        uint8_t configType) override;
  bool setInitialCaption(uint8_t channelNumber, const char *caption) override;
  void sendChannelStateResult(int32_t receiverId, uint8_t channelNo) override;

  // Z2S_MultiDeviceSessionListener
  void onSessionRegistered(Z2S_MultiDeviceSession *session) override;
  int onNewValue(Z2S_MultiDeviceSession *session,
                 TSD_SuplaChannelNewValue *new_value,
                 bool from_group) override;
  void onGetChannelState(Z2S_MultiDeviceSession *session,
                         TDSC_ChannelState *state) override;
  int onCalCfgRequest(Z2S_MultiDeviceSession *session,
                      TSD_DeviceCalCfgRequest *request,
                      TDS_DeviceCalCfgResult *result) override;
  int onSetChannelConfig(Z2S_MultiDeviceSession *session,
                         TSDS_SetChannelConfig *config) override;
  void onChannelConfig(Z2S_MultiDeviceSession *session,
                       TSD_ChannelConfig *config) override;
  void onSetChannelConfigResult(Z2S_MultiDeviceSession *session,
                                TSDS_SetChannelConfigResult *result) override;
  void onChannelConfigFinished(Z2S_MultiDeviceSession *session,
                               int channel_number) override;

 private:
  struct Device {
    std::unique_ptr<Z2S_MultiDeviceSession> session;
    // local channel number -> gateway channel number
    std::vector<uint8_t> channels;
  };

  // Session and local number of a gateway channel; nullptr when the channel
  // is not registered by this layer.
  Z2S_MultiDeviceSession *findChannel(int channel_number, int *local);
  // Gateway channel number of a session channel, -1 when not known.
  int globalChannel(Z2S_MultiDeviceSession *session, int local);
  void fillStateData(int channel_number, TDSC_ChannelState *state);
  uint8_t dispatchChannelConfig(TSD_ChannelConfig *config, int channel_number);

  const char *supla_ca_cert_;
  const char *supla_3rd_party_ca_cert_;
  char *custom_ca_cert_ = nullptr;
  Z2S_MultiDeviceServerConfig config_;
  Z2S_MultiDeviceClientTransportFactory factory_;
  std::vector<Device> devices_;
  uint8_t device_of_channel_[SUPLA_CHANNELMAXCOUNT];
  uint8_t local_of_channel_[SUPLA_CHANNELMAXCOUNT];
  uint32_t last_connect_ms_ = 0;
};

#endif  // Z2S_MULTIDEVICE_LAYER_H_
