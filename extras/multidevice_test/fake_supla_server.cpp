// SPDX-License-Identifier: GPL-2.0-or-later

#include "fake_supla_server.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace z2s_md_test {

namespace {

constexpr size_t kTagSize = SUPLA_TAG_SIZE;
// tag + version + rr_id + call_id + data_size
constexpr size_t kHeaderSize = kTagSize + 1 + 4 + 4 + 4;
const char kTag[kTagSize] = {'S', 'U', 'P', 'L', 'A'};

uint32_t readU32(const std::deque<char> &q, size_t pos) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; i--) {
    v = (v << 8) | static_cast<uint8_t>(q[pos + i]);
  }
  return v;
}

void appendU32(std::deque<char> *q, uint32_t v) {
  for (int i = 0; i < 4; i++)
    q->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void sendPacket(FakeConnection *conn, uint32_t call_id, const void *data,
                size_t size) {
  auto &q = conn->to_device;
  q.insert(q.end(), kTag, kTag + kTagSize);
  q.push_back(static_cast<char>(conn->version ? conn->version : 25));
  appendU32(&q, conn->next_rr_id++);
  appendU32(&q, call_id);
  appendU32(&q, static_cast<uint32_t>(size));
  const char *p = static_cast<const char *>(data);
  q.insert(q.end(), p, p + size);
  q.insert(q.end(), kTag, kTag + kTagSize);
}

template <typename T>
T copyPayload(const std::vector<char> &data) {
  T value;
  memset(&value, 0, sizeof(value));
  if (data.size() > sizeof(value)) {
    throw std::runtime_error("payload larger than struct");
  }
  memcpy(&value, data.data(), data.size());
  return value;
}

// Sends a stored config; with `always`, an empty one when none is stored.
void sendStoredConfig(FakeConnection *conn, int channel, int func,
                      int config_type, bool always) {
  auto it = conn->server->stored_configs.find({channel, config_type});
  bool found = it != conn->server->stored_configs.end();
  if (!found && !always) return;
  TSDS_SetChannelConfig config = {};
  config.ChannelNumber = static_cast<unsigned char>(channel);
  config.Func = func;
  config.ConfigType = static_cast<unsigned char>(config_type);
  if (found) {
    config.ConfigSize = static_cast<unsigned _supla_int16_t>(it->second.size());
    memcpy(config.Config, it->second.data(), it->second.size());
  }
  sendPacket(conn, SUPLA_SD_CALL_SET_CHANNEL_CONFIG, &config,
             offsetof(TSDS_SetChannelConfig, Config) + config.ConfigSize);
}

void handlePacket(FakeConnection *conn, uint32_t call_id,
                  const std::vector<char> &data) {
  switch (call_id) {
    case SUPLA_DS_CALL_REGISTER_DEVICE_G: {
      auto r = std::make_unique<TDS_SuplaRegisterDevice_G>();
      memset(r.get(), 0, sizeof(*r));
      memcpy(r.get(), data.data(), std::min(data.size(), sizeof(*r)));
      size_t expected = sizeof(TDS_SuplaRegisterDevice_G) -
                        sizeof(r->channels) +
                        r->channel_count * sizeof(TDS_SuplaDeviceChannel_E);
      if (data.size() != expected) {
        throw std::runtime_error("invalid REGISTER_DEVICE_G size");
      }
      Registration reg;
      reg.email = r->Email;
      reg.name = r->Name;
      reg.soft_ver = r->SoftVer;
      reg.server_name = r->ServerName;
      reg.flags = r->Flags;
      memcpy(reg.guid.data(), r->GUID, 16);
      memcpy(reg.auth_key.data(), r->AuthKey, 16);
      reg.channels.assign(r->channels, r->channels + r->channel_count);
      conn->registrations.push_back(reg);

      TSD_SuplaRegisterDeviceResult_B result = {};
      result.result_code = conn->server->register_result_code;
      result.activity_timeout =
          static_cast<unsigned char>(conn->server->activity_timeout);
      result.version = SUPLA_PROTO_VERSION;
      result.version_min = 1;
      result.channel_report_size = 0;
      sendPacket(conn, SUPLA_SD_CALL_REGISTER_DEVICE_RESULT_B, &result,
                 sizeof(result) - CHANNEL_REPORT_MAXSIZE);
      if (result.result_code != SUPLA_RESULTCODE_TRUE) break;
      for (const auto &ch : conn->registrations.back().channels) {
        if (!(ch.Flags & SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE)) {
          continue;
        }
        sendStoredConfig(conn, ch.Number, ch.Default,
                         SUPLA_CONFIG_TYPE_DEFAULT, true);
        if (ch.Flags & SUPLA_CHANNEL_FLAG_WEEKLY_SCHEDULE) {
          sendStoredConfig(conn, ch.Number, ch.Default,
                           SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE, false);
        }
        TSD_ChannelConfigFinished finished = {};
        finished.ChannelNumber = ch.Number;
        sendPacket(conn, SUPLA_SD_CALL_CHANNEL_CONFIG_FINISHED, &finished,
                   sizeof(finished));
      }
      break;
    }
    case SUPLA_DS_CALL_DEVICE_CHANNEL_VALUE_CHANGED_C: {
      auto v = copyPayload<TDS_SuplaDeviceChannelValue_C>(data);
      ValueChange change;
      change.channel = v.ChannelNumber;
      memcpy(change.value, v.value, 8);
      change.offline = v.Offline;
      change.validity_time_sec = v.ValidityTimeSec;
      conn->values.push_back(change);
      break;
    }
    case SUPLA_DS_CALL_DEVICE_CHANNEL_EXTENDEDVALUE_CHANGED: {
      auto v = copyPayload<TDS_SuplaDeviceChannelExtendedValue>(data);
      conn->extended_values.push_back({v.ChannelNumber, v.value});
      break;
    }
    case SUPLA_DS_CALL_ACTIONTRIGGER: {
      if (data.size() != sizeof(TDS_ActionTrigger)) {
        throw std::runtime_error("invalid ACTIONTRIGGER size");
      }
      auto a = copyPayload<TDS_ActionTrigger>(data);
      conn->actions.push_back({a.ChannelNumber, a.ActionTrigger});
      break;
    }
    case SUPLA_DSC_CALL_CHANNEL_STATE_RESULT: {
      if (data.size() != sizeof(TDSC_ChannelState)) {
        throw std::runtime_error("invalid CHANNEL_STATE_RESULT size");
      }
      auto s = copyPayload<TDSC_ChannelState>(data);
      conn->channel_states.push_back({s.ChannelNumber, s.ReceiverID, s});
      break;
    }
    case SUPLA_DS_CALL_DEVICE_CALCFG_RESULT: {
      auto r = copyPayload<TDS_DeviceCalCfgResult>(data);
      conn->calcfg_results.push_back(
          {r.ChannelNumber, r.ReceiverID, r.Command, r.Result});
      break;
    }
    case SUPLA_DS_CALL_SET_CHANNEL_CONFIG: {
      auto c = copyPayload<TSDS_SetChannelConfig>(data);
      if (data.size() !=
          offsetof(TSDS_SetChannelConfig, Config) + c.ConfigSize) {
        throw std::runtime_error("invalid SET_CHANNEL_CONFIG size");
      }
      ChannelConfigRequest request;
      request.channel = c.ChannelNumber;
      request.func = c.Func;
      request.config_type = c.ConfigType;
      request.config.assign(c.Config, c.Config + c.ConfigSize);
      conn->configs.push_back(request);
      if (conn->server->channel_config_result == SUPLA_CONFIG_RESULT_TRUE) {
        conn->server->stored_configs[{c.ChannelNumber, c.ConfigType}] =
            request.config;
      }
      TSDS_SetChannelConfigResult result = {};
      result.Result =
          static_cast<unsigned char>(conn->server->channel_config_result);
      result.ConfigType = c.ConfigType;
      result.ChannelNumber = c.ChannelNumber;
      sendPacket(conn, SUPLA_SD_CALL_SET_CHANNEL_CONFIG_RESULT, &result,
                 sizeof(result));
      break;
    }
    case SUPLA_DS_CALL_GET_CHANNEL_CONFIG: {
      auto r = copyPayload<TDS_GetChannelConfigRequest>(data);
      conn->get_configs.push_back({r.ChannelNumber, r.ConfigType});
      TSD_ChannelConfig config = {};
      config.ChannelNumber = r.ChannelNumber;
      config.ConfigType = r.ConfigType;
      auto it = conn->server->stored_configs.find(
          {r.ChannelNumber, r.ConfigType});
      if (it != conn->server->stored_configs.end()) {
        config.ConfigSize =
            static_cast<unsigned _supla_int16_t>(it->second.size());
        memcpy(config.Config, it->second.data(), it->second.size());
      }
      sendPacket(conn, SUPLA_SD_CALL_GET_CHANNEL_CONFIG_RESULT, &config,
                 offsetof(TSD_ChannelConfig, Config) + config.ConfigSize);
      break;
    }
    case SUPLA_DS_CALL_SET_CHANNEL_CONFIG_RESULT: {
      auto r = copyPayload<TSDS_SetChannelConfigResult>(data);
      conn->config_results.push_back(
          {r.ChannelNumber, r.ConfigType, r.Result});
      break;
    }
    case SUPLA_DCS_CALL_SET_CHANNEL_CAPTION: {
      auto c = copyPayload<TDCS_SetCaption>(data);
      size_t size = data.size() - offsetof(TDCS_SetCaption, Caption);
      if (c.CaptionSize != size || size == 0 || c.Caption[size - 1] != '\0') {
        throw std::runtime_error("invalid SET_CHANNEL_CAPTION");
      }
      conn->captions.push_back({c.ChannelNumber, c.Caption});
      break;
    }
    case SUPLA_DS_CALL_CHANNEL_SET_VALUE_RESULT: {
      auto r = copyPayload<TDS_SuplaChannelNewValueResult>(data);
      conn->set_results.push_back({r.ChannelNumber, r.SenderID, r.Success});
      break;
    }
    case SUPLA_DCS_CALL_PING_SERVER: {
      conn->pings++;
      TSDC_SuplaPingServerResult result = {};
      sendPacket(conn, SUPLA_SDC_CALL_PING_SERVER_RESULT, &result,
                 sizeof(result));
      break;
    }
    case SUPLA_DCS_CALL_SET_ACTIVITY_TIMEOUT: {
      conn->activity_timeout_requests++;
      auto req = copyPayload<TDCS_SuplaSetActivityTimeout>(data);
      TSDC_SuplaSetActivityTimeoutResult result = {};
      result.activity_timeout = req.activity_timeout;
      result.min = 10;
      result.max = 240;
      sendPacket(conn, SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT, &result,
                 sizeof(result));
      break;
    }
    default:
      throw std::runtime_error("unexpected call " + std::to_string(call_id));
  }
}

class FakeTransport : public Z2S_MultiDeviceTransport {
 public:
  explicit FakeTransport(FakeSuplaServer *server) : server_(server) {}
  ~FakeTransport() override { close(); }

  bool connect(const char *, int) override {
    if (server_->refuse_connections) {
      status_ = Z2S_MultiDeviceTransportStatus::Failed;
      return true;
    }
    conn_ = server_->accept();
    status_ = Z2S_MultiDeviceTransportStatus::Connected;
    return true;
  }
  Z2S_MultiDeviceTransportStatus status() override {
    if (conn_ && conn_->closed) status_ = Z2S_MultiDeviceTransportStatus::Failed;
    return status_;
  }
  int read(void *buf, int len) override {
    if (!conn_ || conn_->closed) return 0;
    if (conn_->to_device.empty()) return -1;
    int n = 0;
    char *out = static_cast<char *>(buf);
    while (n < len && !conn_->to_device.empty()) {
      out[n++] = conn_->to_device.front();
      conn_->to_device.pop_front();
    }
    return n;
  }
  bool write(const void *buf, int len) override {
    if (!conn_ || conn_->closed) return false;
    const char *in = static_cast<const char *>(buf);
    conn_->to_server.insert(conn_->to_server.end(), in, in + len);
    return true;
  }
  bool flush() override { return !(conn_ && conn_->closed); }
  void close() override {
    if (conn_) conn_->closed = true;
    conn_.reset();
    status_ = Z2S_MultiDeviceTransportStatus::Idle;
  }

 private:
  FakeSuplaServer *server_;
  std::shared_ptr<FakeConnection> conn_;
  Z2S_MultiDeviceTransportStatus status_ = Z2S_MultiDeviceTransportStatus::Idle;
};

}  // namespace

FakeSuplaServer::FakeSuplaServer() = default;
FakeSuplaServer::~FakeSuplaServer() = default;

std::shared_ptr<FakeConnection> FakeSuplaServer::accept() {
  auto conn = std::make_shared<FakeConnection>();
  conn->server = this;
  connections.push_back(conn);
  accepted_count++;
  return conn;
}

void FakeSuplaServer::iterate() {
  for (auto &conn : connections) {
    if (conn->closed) continue;
    auto &q = conn->to_server;
    while (q.size() >= kHeaderSize) {
      for (size_t i = 0; i < kTagSize; i++) {
        if (q[i] != kTag[i]) throw std::runtime_error("missing begin tag");
      }
      uint32_t data_size = readU32(q, kTagSize + 1 + 8);
      size_t total = kHeaderSize + data_size + kTagSize;
      if (q.size() < total) break;
      for (size_t i = 0; i < kTagSize; i++) {
        if (q[kHeaderSize + data_size + i] != kTag[i]) {
          throw std::runtime_error("missing end tag");
        }
      }
      conn->version = static_cast<unsigned char>(q[kTagSize]);
      uint32_t call_id = readU32(q, kTagSize + 1 + 4);
      std::vector<char> data(q.begin() + kHeaderSize,
                             q.begin() + kHeaderSize + data_size);
      q.erase(q.begin(), q.begin() + total);
      handlePacket(conn.get(), call_id, data);
    }
  }
}

void FakeSuplaServer::closeAll() {
  for (auto &conn : connections) conn->closed = true;
}

void FakeSuplaServer::sendGetChannelState(FakeConnection *conn, int channel,
                                          int sender_id) {
  TCSD_ChannelStateRequest request = {};
  request.SenderID = sender_id;
  request.ChannelNumber = static_cast<unsigned char>(channel);
  sendPacket(conn, SUPLA_CSD_CALL_GET_CHANNEL_STATE, &request, sizeof(request));
}

void FakeSuplaServer::sendSetValue(FakeConnection *conn, int channel,
                                   int sender_id, const char value[8],
                                   uint32_t duration_ms) {
  TSD_SuplaChannelNewValue v = {};
  v.SenderID = sender_id;
  v.DurationMS = duration_ms;
  v.ChannelNumber = static_cast<unsigned char>(channel);
  memcpy(v.value, value, sizeof(v.value));
  sendPacket(conn, SUPLA_SD_CALL_CHANNEL_SET_VALUE, &v, sizeof(v));
}

void FakeSuplaServer::sendGroupSetValue(FakeConnection *conn, int channel,
                                        const char value[8]) {
  TSD_SuplaChannelGroupNewValue v = {};
  v.ChannelNumber = static_cast<unsigned char>(channel);
  v.GroupID = 1;
  memcpy(v.value, value, sizeof(v.value));
  sendPacket(conn, SUPLA_SD_CALL_CHANNELGROUP_SET_VALUE, &v, sizeof(v));
}

void FakeSuplaServer::sendCalCfg(FakeConnection *conn, int channel,
                                 int sender_id, int command) {
  TSD_DeviceCalCfgRequest request = {};
  request.SenderID = sender_id;
  request.ChannelNumber = channel;
  request.Command = command;
  request.SuperUserAuthorized = 1;
  sendPacket(conn, SUPLA_SD_CALL_DEVICE_CALCFG_REQUEST, &request,
             offsetof(TSD_DeviceCalCfgRequest, Data));
}

void FakeSuplaServer::sendSetChannelConfig(FakeConnection *conn, int channel,
                                           int func, int config_type,
                                           const void *config, int size) {
  TSDS_SetChannelConfig c = {};
  c.ChannelNumber = static_cast<unsigned char>(channel);
  c.Func = func;
  c.ConfigType = static_cast<unsigned char>(config_type);
  c.ConfigSize = static_cast<unsigned _supla_int16_t>(size);
  memcpy(c.Config, config, size);
  sendPacket(conn, SUPLA_SD_CALL_SET_CHANNEL_CONFIG, &c,
             offsetof(TSDS_SetChannelConfig, Config) + c.ConfigSize);
}

std::unique_ptr<Z2S_MultiDeviceTransport> FakeTransportFactory::create() {
  return std::make_unique<FakeTransport>(server_);
}

}  // namespace z2s_md_test
