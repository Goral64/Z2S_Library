// SPDX-License-Identifier: GPL-2.0-or-later
// In-memory Supla server for tests. It parses and builds SRPC frames on its
// own (independently of srpc.c), so the wire format produced by the session
// is verified end to end.
#pragma once

#include <supla-common/proto.h>

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Z2S_MultiDeviceTransport.h"

namespace z2s_md_test {

struct Registration {
  std::string email;
  std::string name;
  std::string soft_ver;
  std::string server_name;
  int flags = 0;
  std::array<uint8_t, 16> guid{};
  std::array<uint8_t, 16> auth_key{};
  std::vector<TDS_SuplaDeviceChannel_E> channels;
};

struct ValueChange {
  int channel;
  char value[8];
  int offline;
  uint32_t validity_time_sec;
};

struct ExtendedValueChange {
  int channel;
  TSuplaChannelExtendedValue value;
};

struct CaptionRequest {
  int channel;
  std::string caption;
};

struct ChannelConfigRequest {
  int channel;
  int func;
  int config_type;
  std::vector<char> config;
};

struct GetChannelConfigRequest {
  int channel;
  int config_type;
};

struct ChannelConfigResult {
  int channel;
  int config_type;
  int result;
};

struct ChannelStateReport {
  int channel;
  int receiver_id;
  TDSC_ChannelState state;
};

struct ActionReport {
  int channel;
  int action;
};

struct SetResult {
  int channel;
  int sender_id;
  int success;
};

struct CalCfgResult {
  int channel;
  int receiver_id;
  int command;
  int result;
};

class FakeSuplaServer;

struct FakeConnection {
  FakeSuplaServer *server = nullptr;
  unsigned char version = 0;  // protocol version used by the device
  unsigned int next_rr_id = 1;
  std::deque<char> to_server;
  std::deque<char> to_device;
  bool closed = false;
  std::vector<Registration> registrations;
  std::vector<ValueChange> values;
  std::vector<ExtendedValueChange> extended_values;
  std::vector<SetResult> set_results;
  std::vector<CaptionRequest> captions;
  std::vector<ChannelConfigRequest> configs;
  std::vector<GetChannelConfigRequest> get_configs;
  std::vector<ChannelStateReport> channel_states;
  std::vector<ActionReport> actions;
  std::vector<CalCfgResult> calcfg_results;
  // Results of configs sent by the server.
  std::vector<ChannelConfigResult> config_results;
  int pings = 0;
  int activity_timeout_requests = 0;
};

class FakeSuplaServer {
 public:
  FakeSuplaServer();
  ~FakeSuplaServer();

  std::shared_ptr<FakeConnection> accept();
  // Processes all pending packets on all connections.
  void iterate();
  void closeAll();

  void sendGetChannelState(FakeConnection *conn, int channel, int sender_id);
  void sendSetValue(FakeConnection *conn, int channel, int sender_id,
                    const char value[8], uint32_t duration_ms = 0);
  void sendGroupSetValue(FakeConnection *conn, int channel,
                         const char value[8]);
  void sendCalCfg(FakeConnection *conn, int channel, int sender_id,
                  int command);
  void sendSetChannelConfig(FakeConnection *conn, int channel, int func,
                            int config_type, const void *config, int size);

  int register_result_code = SUPLA_RESULTCODE_TRUE;
  int activity_timeout = 120;
  int channel_config_result = SUPLA_CONFIG_RESULT_TRUE;
  // Raw channel configs stored by the "server": (channel, config type) ->
  // config. Like the real server, channels with
  // SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE get their default config
  // after registration (empty when none is stored), the weekly schedule when
  // they have SUPLA_CHANNEL_FLAG_WEEKLY_SCHEDULE and one is stored, and then
  // CHANNEL_CONFIG_FINISHED.
  std::map<std::pair<int, int>, std::vector<char>> stored_configs;
  bool refuse_connections = false;
  std::vector<std::shared_ptr<FakeConnection>> connections;
  int accepted_count = 0;
};

class FakeTransportFactory : public Z2S_MultiDeviceTransportFactory {
 public:
  explicit FakeTransportFactory(FakeSuplaServer *server) : server_(server) {}
  std::unique_ptr<Z2S_MultiDeviceTransport> create() override;

 private:
  FakeSuplaServer *server_;
};

}  // namespace z2s_md_test
