// SPDX-License-Identifier: GPL-2.0-or-later

#include "Z2S_MultiDeviceClientTransport.h"

#include <supla/network/client.h>
#include <supla/network/network.h>

Z2S_MultiDeviceClientTransport::Z2S_MultiDeviceClientTransport(
    bool ssl, const char *ca_cert)
    : ssl_(ssl), ca_cert_(ca_cert) {}

Z2S_MultiDeviceClientTransport::~Z2S_MultiDeviceClientTransport() {
  close();
  delete client_;
}

bool Z2S_MultiDeviceClientTransport::connect(const char *host, int port) {
  close();
  if (client_ == nullptr) {
    client_ = Supla::Network::Instance() != nullptr
                  ? Supla::Network::Instance()->createClient()
                  : Supla::ClientBuilder();
    if (client_ == nullptr) return false;
    if (ssl_) {
      client_->setSSLEnabled(true);
      client_->setCACert(ca_cert_);
    }
  }
  connected_ = client_->connect(host, port) == 1;
  // A failed connection is reported by status().
  return true;
}

Z2S_MultiDeviceTransportStatus Z2S_MultiDeviceClientTransport::status() {
  if (client_ == nullptr) return Z2S_MultiDeviceTransportStatus::Idle;
  if (!connected_ || !client_->connected()) {
    return Z2S_MultiDeviceTransportStatus::Failed;
  }
  return Z2S_MultiDeviceTransportStatus::Connected;
}

int Z2S_MultiDeviceClientTransport::read(void *buf, int len) {
  if (client_ == nullptr || !connected_ || !client_->connected()) return 0;
  int r = client_->read(static_cast<uint8_t *>(buf), len);
  return r > 0 ? r : -1;
}

bool Z2S_MultiDeviceClientTransport::write(const void *buf, int len) {
  if (client_ == nullptr || !connected_) return false;
  return client_->write(buf, len) == static_cast<size_t>(len);
}

bool Z2S_MultiDeviceClientTransport::flush() {
  return client_ != nullptr && connected_ && client_->connected();
}

void Z2S_MultiDeviceClientTransport::close() {
  if (client_ != nullptr) client_->stop();
  connected_ = false;
}
