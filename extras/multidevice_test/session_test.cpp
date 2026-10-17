// SPDX-License-Identifier: GPL-2.0-or-later
// End-to-end tests of Z2S_MultiDeviceSession against an in-memory Supla
// server speaking the real SRPC protocol.

#include <gtest/gtest.h>
#include <supla-common/proto.h>

#include <cstring>

#include "Z2S_MultiDeviceSession.h"
#include "fake_supla_server.h"

using namespace z2s_md_test;

namespace {

Z2S_MultiDeviceServerConfig serverConfig() {
  Z2S_MultiDeviceServerConfig c;
  c.server = "svr1.supla.org";
  c.email = "user@example.com";
  c.activity_timeout_s = 120;
  c.soft_ver = "Z2S_Library 1.6.87";
  return c;
}

TDS_SuplaDeviceChannel_E relayChannel() {
  TDS_SuplaDeviceChannel_E ch = {};
  ch.Number = 7;  // the session renumbers channels
  ch.Type = SUPLA_CHANNELTYPE_RELAY;
  ch.FuncList = SUPLA_BIT_FUNC_LIGHTSWITCH | SUPLA_BIT_FUNC_POWERSWITCH;
  ch.Default = SUPLA_CHANNELFNC_LIGHTSWITCH;
  ch.Offline = SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE;
  ch.value[0] = 1;
  ch.SubDeviceId = 3;  // gateway subdevice id, not used by the session
  return ch;
}

TDS_SuplaDeviceChannel_E thermometerChannel() {
  TDS_SuplaDeviceChannel_E ch = {};
  ch.Number = 12;
  ch.Type = SUPLA_CHANNELTYPE_THERMOMETER;
  ch.Default = SUPLA_CHANNELFNC_THERMOMETER;
  ch.Offline = SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE;
  double t = -275;
  memcpy(ch.value, &t, sizeof(t));
  return ch;
}

void temperatureValue(double t, int8_t out[8]) { memcpy(out, &t, sizeof(t)); }

class RecordingListener : public Z2S_MultiDeviceSessionListener {
 public:
  void onSessionRegistered(Z2S_MultiDeviceSession *) override { registered++; }
  void onSessionDisconnected(Z2S_MultiDeviceSession *) override {
    disconnected++;
  }
  int onNewValue(Z2S_MultiDeviceSession *, TSD_SuplaChannelNewValue *value,
                 bool from_group) override {
    new_values.push_back(*value);
    from_groups.push_back(from_group);
    return new_value_result;
  }
  void onGetChannelState(Z2S_MultiDeviceSession *,
                         TDSC_ChannelState *state) override {
    state->Fields |= SUPLA_CHANNELSTATE_FIELD_BATTERYLEVEL;
    state->BatteryLevel = 77;
  }
  int onCalCfgRequest(Z2S_MultiDeviceSession *,
                      TSD_DeviceCalCfgRequest *request,
                      TDS_DeviceCalCfgResult *) override {
    calcfg_commands.push_back(request->Command);
    return calcfg_result;
  }
  int onSetChannelConfig(Z2S_MultiDeviceSession *,
                         TSDS_SetChannelConfig *config) override {
    set_configs.push_back(*config);
    return set_config_result;
  }
  void onChannelConfig(Z2S_MultiDeviceSession *,
                       TSD_ChannelConfig *config) override {
    configs.push_back(*config);
  }
  void onSetChannelConfigResult(Z2S_MultiDeviceSession *,
                                TSDS_SetChannelConfigResult *result) override {
    config_results.push_back(*result);
  }
  void onChannelConfigFinished(Z2S_MultiDeviceSession *,
                               int channel_number) override {
    config_finished.push_back(channel_number);
  }

  int registered = 0;
  int disconnected = 0;
  int new_value_result = 1;
  int calcfg_result = SUPLA_CALCFG_RESULT_NOT_SUPPORTED;
  int set_config_result = SUPLA_CONFIG_RESULT_TRUE;
  std::vector<TSD_SuplaChannelNewValue> new_values;
  std::vector<bool> from_groups;
  std::vector<int> calcfg_commands;
  std::vector<TSDS_SetChannelConfig> set_configs;
  std::vector<TSD_ChannelConfig> configs;
  std::vector<TSDS_SetChannelConfigResult> config_results;
  std::vector<int> config_finished;
};

class SessionTest : public ::testing::Test {
 protected:
  SessionTest()
      : config_(serverConfig()),
        factory_(&server_),
        session_(&config_, &factory_, &listener_) {
    for (int i = 0; i < 16; i++) {
      guid_[i] = static_cast<uint8_t>(1 + i);
      auth_key_[i] = static_cast<uint8_t>(0x80 + i);
    }
    session_.setIdentity(guid_, auth_key_);
    session_.setName("Lamp (IKEA::TRADFRI)");
    TDS_SuplaDeviceChannel_E channels[] = {relayChannel(),
                                           thermometerChannel()};
    session_.setChannels(channels, 2);
  }

  // Runs the session and the server for the given number of 10 ms steps.
  void run(int steps) {
    for (int i = 0; i < steps; i++) {
      now_ += 10;
      if (session_.wantsToConnect(now_)) session_.startConnect(now_);
      session_.iterate(now_);
      server_.iterate();
    }
  }

  // Runs until the given time has passed, in 1 s steps.
  void runFor(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 1000) {
      now_ += 1000;
      if (session_.wantsToConnect(now_)) session_.startConnect(now_);
      session_.iterate(now_);
      server_.iterate();
    }
    run(5);
  }

  FakeConnection &conn() { return *server_.connections.back(); }

  Z2S_MultiDeviceServerConfig config_;
  FakeSuplaServer server_;
  FakeTransportFactory factory_;
  RecordingListener listener_;
  Z2S_MultiDeviceSession session_;
  uint8_t guid_[16];
  uint8_t auth_key_[16];
  uint32_t now_ = 1000;
};

}  // namespace

TEST_F(SessionTest, RegistersAsSeparateDevice) {
  run(5);

  ASSERT_TRUE(session_.isRegistered());
  EXPECT_EQ(listener_.registered, 1);
  ASSERT_EQ(conn().registrations.size(), 1u);
  const Registration &reg = conn().registrations[0];
  EXPECT_EQ(reg.email, "user@example.com");
  EXPECT_EQ(reg.name, "Lamp (IKEA::TRADFRI)");
  EXPECT_EQ(reg.soft_ver, "Z2S_Library 1.6.87");
  EXPECT_EQ(reg.server_name, "svr1.supla.org");
  EXPECT_EQ(reg.flags, 0);
  EXPECT_EQ(0, memcmp(reg.guid.data(), guid_, 16));
  EXPECT_EQ(0, memcmp(reg.auth_key.data(), auth_key_, 16));
  ASSERT_EQ(reg.channels.size(), 2u);

  EXPECT_EQ(+reg.channels[0].Number, 0);
  EXPECT_EQ(+reg.channels[0].Type, SUPLA_CHANNELTYPE_RELAY);
  EXPECT_EQ(+reg.channels[0].FuncList,
            SUPLA_BIT_FUNC_LIGHTSWITCH | SUPLA_BIT_FUNC_POWERSWITCH);
  EXPECT_EQ(+reg.channels[0].Default, SUPLA_CHANNELFNC_LIGHTSWITCH);
  EXPECT_EQ(reg.channels[0].value[0], 1);
  EXPECT_EQ(+reg.channels[0].Offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);
  EXPECT_EQ(+reg.channels[0].SubDeviceId, 0);

  EXPECT_EQ(+reg.channels[1].Number, 1);
  EXPECT_EQ(+reg.channels[1].Type, SUPLA_CHANNELTYPE_THERMOMETER);
  EXPECT_EQ(+reg.channels[1].Offline, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE);

  // The server proposed the requested timeout, so no change is requested.
  EXPECT_EQ(conn().activity_timeout_requests, 0);
  // Values sent with the registration are not sent again.
  EXPECT_TRUE(conn().values.empty());
}

TEST_F(SessionTest, RegistersWithCurrentValue) {
  int8_t value[8];
  temperatureValue(21.5, value);
  session_.setChannelValue(1, value, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE, 0);
  run(5);

  ASSERT_TRUE(session_.isRegistered());
  const auto &ch = conn().registrations[0].channels[1];
  EXPECT_EQ(ch.Offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);
  double t;
  memcpy(&t, ch.value, sizeof(t));
  EXPECT_DOUBLE_EQ(t, 21.5);
}

TEST_F(SessionTest, SendsOnlyChangedValues) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());

  int8_t value[8];
  temperatureValue(19.0, value);
  session_.setChannelValue(1, value, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE, 0);
  run(3);
  ASSERT_EQ(conn().values.size(), 1u);
  EXPECT_EQ(conn().values[0].channel, 1);
  EXPECT_EQ(conn().values[0].offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);
  EXPECT_EQ(0, memcmp(conn().values[0].value, value, 8));

  // Same value again: nothing is sent.
  session_.setChannelValue(1, value, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE, 0);
  run(3);
  EXPECT_EQ(conn().values.size(), 1u);

  // Same value, but offline: sent.
  session_.setChannelValue(1, value, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE, 0);
  run(3);
  ASSERT_EQ(conn().values.size(), 2u);
  EXPECT_EQ(conn().values[1].offline, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE);

  // Validity time is part of the value.
  session_.setChannelValue(1, value, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE, 600);
  run(3);
  ASSERT_EQ(conn().values.size(), 3u);
  EXPECT_EQ(conn().values[2].validity_time_sec, 600u);
}

TEST_F(SessionTest, ValueChangedWhileDisconnectedGoesWithRegistration) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  server_.closeAll();
  run(5);
  EXPECT_FALSE(session_.isRegistered());
  EXPECT_EQ(listener_.disconnected, 1);

  int8_t value[8];
  temperatureValue(18.25, value);
  session_.setChannelValue(1, value, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE, 0);
  runFor(10000);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(server_.accepted_count, 2);
  double t;
  memcpy(&t, conn().registrations[0].channels[1].value, sizeof(t));
  EXPECT_DOUBLE_EQ(t, 18.25);
  EXPECT_TRUE(conn().values.empty());
}

TEST_F(SessionTest, SendsCaptionsAfterEachRegistration) {
  session_.setChannelCaption(0, "Lamp (IKEA::TRADFRI) – relay");
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(conn().captions.size(), 1u);
  EXPECT_EQ(conn().captions[0].channel, 0);
  EXPECT_EQ(conn().captions[0].caption, "Lamp (IKEA::TRADFRI) – relay");

  // A caption set while registered is sent at once.
  session_.setChannelCaption(1, "temperature");
  run(3);
  ASSERT_EQ(conn().captions.size(), 2u);
  EXPECT_EQ(conn().captions[1].channel, 1);

  server_.closeAll();
  runFor(10000);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_EQ(conn().captions.size(), 2u);
}

TEST_F(SessionTest, NewValueGoesToListenerAndResultToServer) {
  run(5);
  char value[8] = {0};
  server_.sendSetValue(&conn(), 0, 42, value, 5000);
  run(3);
  ASSERT_EQ(listener_.new_values.size(), 1u);
  EXPECT_EQ(+listener_.new_values[0].ChannelNumber, 0);
  EXPECT_EQ(+listener_.new_values[0].SenderID, 42);
  EXPECT_EQ(+listener_.new_values[0].DurationMS, 5000u);
  EXPECT_FALSE(listener_.from_groups[0]);
  ASSERT_EQ(conn().set_results.size(), 1u);
  EXPECT_EQ(conn().set_results[0].channel, 0);
  EXPECT_EQ(conn().set_results[0].sender_id, 42);
  EXPECT_EQ(conn().set_results[0].success, 1);

  // -1: the element sends no result.
  listener_.new_value_result = -1;
  server_.sendSetValue(&conn(), 0, 43, value);
  run(3);
  EXPECT_EQ(listener_.new_values.size(), 2u);
  EXPECT_EQ(conn().set_results.size(), 1u);
}

TEST_F(SessionTest, GroupValueGoesToListenerWithoutResult) {
  run(5);
  char value[8] = {1};
  server_.sendGroupSetValue(&conn(), 0, value);
  run(3);
  ASSERT_EQ(listener_.new_values.size(), 1u);
  EXPECT_TRUE(listener_.from_groups[0]);
  EXPECT_EQ(+listener_.new_values[0].SenderID, 0);
  EXPECT_EQ(listener_.new_values[0].value[0], 1);
  EXPECT_TRUE(conn().set_results.empty());
}

TEST_F(SessionTest, ValueForUnknownChannelIsIgnored) {
  run(5);
  char value[8] = {1};
  server_.sendSetValue(&conn(), 5, 42, value);
  run(3);
  EXPECT_TRUE(listener_.new_values.empty());
  EXPECT_TRUE(conn().set_results.empty());
  EXPECT_TRUE(session_.isRegistered());
}

TEST_F(SessionTest, ChannelStateIsFilledByListener) {
  run(5);
  server_.sendGetChannelState(&conn(), 1, 99);
  run(3);
  ASSERT_EQ(conn().channel_states.size(), 1u);
  EXPECT_EQ(conn().channel_states[0].channel, 1);
  EXPECT_EQ(conn().channel_states[0].receiver_id, 99);
  EXPECT_TRUE(conn().channel_states[0].state.Fields &
              SUPLA_CHANNELSTATE_FIELD_BATTERYLEVEL);
  EXPECT_EQ(+conn().channel_states[0].state.BatteryLevel, 77);
}

TEST_F(SessionTest, SendsChannelStateWithoutRequest) {
  TDSC_ChannelState state = {};
  state.ChannelNumber = 1;
  state.Fields = SUPLA_CHANNELSTATE_FIELD_BATTERYLEVEL;
  state.BatteryLevel = 40;
  EXPECT_FALSE(session_.sendChannelState(&state));
  run(5);
  EXPECT_TRUE(session_.sendChannelState(&state));
  state.ChannelNumber = 2;
  EXPECT_FALSE(session_.sendChannelState(&state));
  run(3);
  ASSERT_EQ(conn().channel_states.size(), 1u);
  EXPECT_EQ(conn().channel_states[0].channel, 1);
  EXPECT_EQ(conn().channel_states[0].receiver_id, 0);
  EXPECT_EQ(+conn().channel_states[0].state.BatteryLevel, 40);
}

TEST_F(SessionTest, CalCfgGoesToListener) {
  run(5);
  server_.sendCalCfg(&conn(), 0, 5, SUPLA_CALCFG_CMD_IDENTIFY_DEVICE);
  run(3);
  ASSERT_EQ(listener_.calcfg_commands.size(), 1u);
  ASSERT_EQ(conn().calcfg_results.size(), 1u);
  EXPECT_EQ(conn().calcfg_results[0].receiver_id, 5);
  EXPECT_EQ(conn().calcfg_results[0].command,
            SUPLA_CALCFG_CMD_IDENTIFY_DEVICE);
  EXPECT_EQ(conn().calcfg_results[0].result,
            SUPLA_CALCFG_RESULT_NOT_SUPPORTED);

  listener_.calcfg_result = SUPLA_CALCFG_RESULT_TRUE;
  server_.sendCalCfg(&conn(), 0, 6, SUPLA_CALCFG_CMD_IDENTIFY_DEVICE);
  run(3);
  ASSERT_EQ(conn().calcfg_results.size(), 2u);
  EXPECT_EQ(conn().calcfg_results[1].result, SUPLA_CALCFG_RESULT_TRUE);
}

TEST_F(SessionTest, DeviceCalCfgIsNotSupported) {
  run(5);
  server_.sendCalCfg(&conn(), -1, 5, SUPLA_CALCFG_CMD_ENTER_CFG_MODE);
  run(3);
  EXPECT_TRUE(listener_.calcfg_commands.empty());
  ASSERT_EQ(conn().calcfg_results.size(), 1u);
  EXPECT_EQ(conn().calcfg_results[0].result,
            SUPLA_CALCFG_RESULT_NOT_SUPPORTED);
}

TEST_F(SessionTest, ConfigFromServerGoesToListener) {
  run(5);
  TChannelConfig_PowerSwitch ps = {};
  ps.OvercurrentMaxAllowed = 1600;
  server_.sendSetChannelConfig(&conn(), 0, SUPLA_CHANNELFNC_POWERSWITCH,
                               SUPLA_CONFIG_TYPE_DEFAULT, &ps, sizeof(ps));
  listener_.set_config_result = SUPLA_CONFIG_RESULT_DATA_ERROR;
  run(3);
  ASSERT_EQ(listener_.set_configs.size(), 1u);
  EXPECT_EQ(+listener_.set_configs[0].ChannelNumber, 0);
  EXPECT_EQ(+listener_.set_configs[0].Func, SUPLA_CHANNELFNC_POWERSWITCH);
  EXPECT_EQ(+listener_.set_configs[0].ConfigSize, sizeof(ps));
  ASSERT_EQ(conn().config_results.size(), 1u);
  EXPECT_EQ(conn().config_results[0].channel, 0);
  EXPECT_EQ(conn().config_results[0].result, SUPLA_CONFIG_RESULT_DATA_ERROR);
}

TEST_F(SessionTest, RuntimeConfigAfterRegistration) {
  TDS_SuplaDeviceChannel_E hvac = {};
  hvac.Type = SUPLA_CHANNELTYPE_HVAC;
  hvac.Default = SUPLA_CHANNELFNC_HVAC_THERMOSTAT;
  hvac.Flags = SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE |
               SUPLA_CHANNEL_FLAG_WEEKLY_SCHEDULE;
  TDS_SuplaDeviceChannel_E channels[] = {thermometerChannel(), hvac};
  session_.setChannels(channels, 2);
  std::vector<char> schedule(sizeof(TChannelConfig_WeeklySchedule), 1);
  server_.stored_configs[{1, SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE}] = schedule;
  run(5);

  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(listener_.set_configs.size(), 2u);
  EXPECT_EQ(+listener_.set_configs[0].ChannelNumber, 1);
  EXPECT_EQ(+listener_.set_configs[0].ConfigType, SUPLA_CONFIG_TYPE_DEFAULT);
  EXPECT_EQ(+listener_.set_configs[1].ConfigType,
            SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE);
  EXPECT_EQ(+listener_.set_configs[1].ConfigSize, schedule.size());
  EXPECT_EQ(listener_.config_finished, std::vector<int>{1});
  EXPECT_EQ(conn().config_results.size(), 2u);
}

TEST_F(SessionTest, DeviceSetsAndGetsChannelConfig) {
  TChannelConfig_PowerSwitch ps = {};
  EXPECT_FALSE(session_.setChannelConfig(0, SUPLA_CHANNELFNC_POWERSWITCH, &ps,
                                         sizeof(ps),
                                         SUPLA_CONFIG_TYPE_DEFAULT));
  run(5);
  ps.DefaultRelatedMeterChannelNo = 1;
  EXPECT_TRUE(session_.setChannelConfig(0, SUPLA_CHANNELFNC_POWERSWITCH, &ps,
                                        sizeof(ps),
                                        SUPLA_CONFIG_TYPE_DEFAULT));
  run(3);
  ASSERT_EQ(conn().configs.size(), 1u);
  EXPECT_EQ(conn().configs[0].channel, 0);
  EXPECT_EQ(conn().configs[0].func, SUPLA_CHANNELFNC_POWERSWITCH);
  EXPECT_EQ(conn().configs[0].config.size(), sizeof(ps));
  ASSERT_EQ(listener_.config_results.size(), 1u);
  EXPECT_EQ(+listener_.config_results[0].Result, SUPLA_CONFIG_RESULT_TRUE);

  EXPECT_TRUE(session_.getChannelConfig(0, SUPLA_CONFIG_TYPE_DEFAULT));
  run(3);
  ASSERT_EQ(conn().get_configs.size(), 1u);
  ASSERT_EQ(listener_.configs.size(), 1u);
  EXPECT_EQ(+listener_.configs[0].ChannelNumber, 0);
  EXPECT_EQ(+listener_.configs[0].ConfigSize, sizeof(ps));
}

TEST_F(SessionTest, ExtendedValueAndActionOnlyWhenRegistered) {
  TSuplaChannelExtendedValue ev = {};
  ev.type = EV_TYPE_TIMER_STATE_V1;
  ev.size = sizeof(TTimerState_ExtendedValue);
  EXPECT_FALSE(session_.sendExtendedValue(0, &ev));
  EXPECT_FALSE(session_.sendActionTrigger(0, SUPLA_ACTION_CAP_SHORT_PRESS_x1));
  run(5);
  EXPECT_TRUE(session_.sendExtendedValue(0, &ev));
  EXPECT_TRUE(session_.sendActionTrigger(1, SUPLA_ACTION_CAP_SHORT_PRESS_x1));
  EXPECT_FALSE(session_.sendActionTrigger(2, SUPLA_ACTION_CAP_SHORT_PRESS_x1));
  run(3);
  ASSERT_EQ(conn().extended_values.size(), 1u);
  EXPECT_EQ(conn().extended_values[0].channel, 0);
  EXPECT_EQ(conn().extended_values[0].value.type, EV_TYPE_TIMER_STATE_V1);
  ASSERT_EQ(conn().actions.size(), 1u);
  EXPECT_EQ(conn().actions[0].channel, 1);
  EXPECT_EQ(conn().actions[0].action, SUPLA_ACTION_CAP_SHORT_PRESS_x1);
}

TEST_F(SessionTest, LayoutChangeRegistersAgain) {
  run(5);
  ASSERT_EQ(server_.accepted_count, 1);

  // Same layout with other values: no new registration.
  TDS_SuplaDeviceChannel_E channels[] = {relayChannel(), thermometerChannel(),
                                         thermometerChannel()};
  channels[0].value[0] = 0;
  session_.setChannels(channels, 2);
  run(5);
  EXPECT_EQ(server_.accepted_count, 1);

  // A channel added at the end.
  session_.setChannels(channels, 3);
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_EQ(server_.accepted_count, 2);
  EXPECT_EQ(conn().registrations[0].channels.size(), 3u);

  // Other function list.
  channels[0].FuncList = SUPLA_BIT_FUNC_POWERSWITCH;
  session_.setChannels(channels, 3);
  run(5);
  EXPECT_EQ(server_.accepted_count, 3);
  EXPECT_EQ(+conn().registrations[0].channels[0].FuncList,
            SUPLA_BIT_FUNC_POWERSWITCH);
}

TEST_F(SessionTest, NameChangeRegistersAgain) {
  run(5);
  session_.setName("Lamp (IKEA::TRADFRI)");
  run(5);
  EXPECT_EQ(server_.accepted_count, 1);
  session_.setName("Kitchen lamp (IKEA::TRADFRI)");
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_EQ(server_.accepted_count, 2);
  EXPECT_EQ(conn().registrations[0].name, "Kitchen lamp (IKEA::TRADFRI)");
}

TEST_F(SessionTest, RegistrationDisabledIsRetriedAfterAMinute) {
  server_.register_result_code = SUPLA_RESULTCODE_REGISTRATION_DISABLED;
  run(5);
  EXPECT_FALSE(session_.isRegistered());
  EXPECT_EQ(session_.lastResultCode(), SUPLA_RESULTCODE_REGISTRATION_DISABLED);
  EXPECT_EQ(server_.accepted_count, 1);

  runFor(50000);
  EXPECT_EQ(server_.accepted_count, 1);

  server_.register_result_code = SUPLA_RESULTCODE_TRUE;
  runFor(30000);
  EXPECT_TRUE(session_.isRegistered());
  EXPECT_EQ(server_.accepted_count, 2);
}

TEST_F(SessionTest, ReconnectDelayGrows) {
  server_.refuse_connections = true;
  run(5);
  EXPECT_EQ(server_.accepted_count, 0);
  EXPECT_FALSE(session_.isConnecting());

  // Delays 5 s, 10 s, 20 s (+ up to 25 % jitter): 4 attempts in 60 s.
  int attempts = 0;
  for (int i = 0; i < 60; i++) {
    now_ += 1000;
    if (session_.wantsToConnect(now_)) {
      attempts++;
      session_.startConnect(now_);
    }
    session_.iterate(now_);
  }
  EXPECT_GE(attempts, 2);
  EXPECT_LE(attempts, 3);

  server_.refuse_connections = false;
  runFor(60000);
  EXPECT_TRUE(session_.isRegistered());
}

TEST_F(SessionTest, SuspendedSessionStaysDisconnected) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  session_.setSuspended(true);
  EXPECT_FALSE(session_.isRegistered());
  EXPECT_TRUE(conn().closed);
  EXPECT_EQ(listener_.disconnected, 1);
  runFor(30000);
  EXPECT_EQ(server_.accepted_count, 1);

  session_.setSuspended(false);
  run(5);
  EXPECT_TRUE(session_.isRegistered());
  EXPECT_EQ(server_.accepted_count, 2);
}

TEST_F(SessionTest, DisabledSessionDoesNotConnect) {
  session_.disable();
  runFor(10000);
  EXPECT_EQ(server_.accepted_count, 0);
  session_.enable();
  run(5);
  EXPECT_TRUE(session_.isRegistered());
}

TEST_F(SessionTest, PingsWhenIdle) {
  server_.activity_timeout = 30;
  config_.activity_timeout_s = 30;
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  runFor(60000);
  EXPECT_GE(conn().pings, 2);
  EXPECT_TRUE(session_.isRegistered());
  EXPECT_EQ(server_.accepted_count, 1);
}

TEST_F(SessionTest, RequestsActivityTimeout) {
  server_.activity_timeout = 30;
  run(5);
  EXPECT_EQ(conn().activity_timeout_requests, 1);
}

TEST_F(SessionTest, ReconnectsWhenServerIsSilent) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  // The server stops answering (and reading).
  auto silent = server_.connections.back();
  for (int i = 0; i < 140; i++) {
    now_ += 1000;
    session_.iterate(now_);
    silent->to_server.clear();
  }
  EXPECT_FALSE(session_.isRegistered());
  runFor(20000);
  EXPECT_TRUE(session_.isRegistered());
  EXPECT_EQ(server_.accepted_count, 2);
}

TEST_F(SessionTest, SurvivesMillisWrapAround) {
  now_ = 0xFFFFFFFFu - 2000;
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  server_.closeAll();
  // The reconnect time is past the wrap-around.
  runFor(15000);
  EXPECT_TRUE(session_.isRegistered());
  EXPECT_EQ(server_.accepted_count, 2);
  EXPECT_LT(now_, 0x10000000u);
}

TEST(SessionsTest, TwoSessionsRegisterSeparately) {
  Z2S_MultiDeviceServerConfig config = serverConfig();
  FakeSuplaServer server;
  FakeTransportFactory factory(&server);
  RecordingListener listener;
  Z2S_MultiDeviceSession a(&config, &factory, &listener);
  Z2S_MultiDeviceSession b(&config, &factory, &listener);
  uint8_t guid_a[16] = {1}, guid_b[16] = {2}, key[16] = {3};
  a.setIdentity(guid_a, key);
  b.setIdentity(guid_b, key);
  a.setName("A");
  b.setName("B");
  TDS_SuplaDeviceChannel_E ch_a[] = {relayChannel()};
  TDS_SuplaDeviceChannel_E ch_b[] = {thermometerChannel(), relayChannel()};
  a.setChannels(ch_a, 1);
  b.setChannels(ch_b, 2);
  uint32_t now = 1000;
  for (int i = 0; i < 5; i++) {
    now += 10;
    for (auto *s : {&a, &b}) {
      if (s->wantsToConnect(now)) s->startConnect(now);
      s->iterate(now);
    }
    server.iterate();
  }
  ASSERT_TRUE(a.isRegistered());
  ASSERT_TRUE(b.isRegistered());
  ASSERT_EQ(server.connections.size(), 2u);
  EXPECT_EQ(server.connections[0]->registrations[0].name, "A");
  EXPECT_EQ(server.connections[0]->registrations[0].channels.size(), 1u);
  EXPECT_EQ(server.connections[1]->registrations[0].name, "B");
  EXPECT_EQ(server.connections[1]->registrations[0].channels.size(), 2u);
  EXPECT_EQ(listener.registered, 2);
}
