// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Z2S_MULTIDEVICE_CLIENT_TRANSPORT_H_
#define Z2S_MULTIDEVICE_CLIENT_TRANSPORT_H_

#include "Z2S_MultiDeviceTransport.h"

namespace Supla {
class Client;
}

// Transport on the network client of supla-device (TLS like the gateway's
// own connection). connect() blocks until the TLS handshake is done.
class Z2S_MultiDeviceClientTransport : public Z2S_MultiDeviceTransport {
 public:
  // ca_cert == nullptr: the server certificate is not verified.
  Z2S_MultiDeviceClientTransport(bool ssl, const char *ca_cert);
  ~Z2S_MultiDeviceClientTransport() override;

  bool connect(const char *host, int port) override;
  Z2S_MultiDeviceTransportStatus status() override;
  int read(void *buf, int len) override;
  bool write(const void *buf, int len) override;
  bool flush() override;
  void close() override;

 private:
  bool ssl_;
  const char *ca_cert_;
  Supla::Client *client_ = nullptr;
  bool connected_ = false;
};

class Z2S_MultiDeviceClientTransportFactory
    : public Z2S_MultiDeviceTransportFactory {
 public:
  void setSsl(bool ssl, const char *ca_cert) {
    ssl_ = ssl;
    ca_cert_ = ca_cert;
  }
  std::unique_ptr<Z2S_MultiDeviceTransport> create() override {
    return std::unique_ptr<Z2S_MultiDeviceTransport>(
        new Z2S_MultiDeviceClientTransport(ssl_, ca_cert_));
  }

 private:
  bool ssl_ = true;
  const char *ca_cert_ = nullptr;
};

#endif  // Z2S_MULTIDEVICE_CLIENT_TRANSPORT_H_
