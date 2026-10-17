// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Z2S_MULTIDEVICE_TRANSPORT_H_
#define Z2S_MULTIDEVICE_TRANSPORT_H_

#include <memory>

enum class Z2S_MultiDeviceTransportStatus { Idle, Connecting, Connected, Failed };

// Non-blocking, stream oriented connection to the Supla server (TCP + TLS),
// one per session. Implemented per platform.
class Z2S_MultiDeviceTransport {
 public:
  virtual ~Z2S_MultiDeviceTransport() = default;
  // Starts a connection. Returns false on immediate failure.
  virtual bool connect(const char *host, int port) = 0;
  // Advances the connection/handshake and reports the current status.
  virtual Z2S_MultiDeviceTransportStatus status() = 0;
  // Returns the number of bytes read (> 0), -1 when no data is available
  // yet, 0 when the connection was closed or failed.
  virtual int read(void *buf, int len) = 0;
  // Queues data for sending. Returns false when the connection is broken.
  virtual bool write(const void *buf, int len) = 0;
  // Sends as much queued data as possible. Returns false on error.
  virtual bool flush() = 0;
  virtual void close() = 0;
};

class Z2S_MultiDeviceTransportFactory {
 public:
  virtual ~Z2S_MultiDeviceTransportFactory() = default;
  virtual std::unique_ptr<Z2S_MultiDeviceTransport> create() = 0;
};

#endif  // Z2S_MULTIDEVICE_TRANSPORT_H_
