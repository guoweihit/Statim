// SPDX-License-Identifier: GPL-3.0-or-later
// Additional ns-3 linking permission: see LICENSE.md.

#ifndef STATIM_PORT_TABLE_HPP
#define STATIM_PORT_TABLE_HPP

#include "ns3/net-device.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <functional>
#include <map>

namespace ns3
{
namespace ndn
{
namespace statim
{

class PortTable
{
public:
  PortTable() : m_nextPort(1) {}

  uint16_t addDevice(Ptr<NetDevice> device)
  {
    uint16_t port = m_nextPort++;
    m_portToDevice[port] = device;
    m_deviceToPort[PeekPointer(device)] = port;
    return port;
  }

  Ptr<NetDevice> getDevice(uint16_t port) const
  {
    auto it = m_portToDevice.find(port);
    return (it != m_portToDevice.end()) ? it->second : nullptr;
  }

  uint16_t getPort(Ptr<NetDevice> device) const
  {
    auto it = m_deviceToPort.find(PeekPointer(device));
    return (it != m_deviceToPort.end()) ? it->second : 0;
  }

  uint16_t getPortByRaw(NetDevice* device) const
  {
    auto it = m_deviceToPort.find(device);
    return (it != m_deviceToPort.end()) ? it->second : 0;
  }

  bool hasPort(uint16_t port) const { return m_portToDevice.count(port) > 0; }

  size_t size() const { return m_portToDevice.size(); }

  enum : uint16_t
  {
    CONTROLLER_PORT = 65535
  };

  using SendCallback = std::function<void(uint16_t port, Ptr<Packet> packet)>;
  void setSendCallback(SendCallback cb) { m_sendCallback = cb; }
  const SendCallback& getSendCallback() const { return m_sendCallback; }

private:
  std::map<uint16_t, Ptr<NetDevice>> m_portToDevice;
  std::map<NetDevice*, uint16_t> m_deviceToPort;
  uint16_t m_nextPort;
  SendCallback m_sendCallback;
};

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_PORT_TABLE_HPP
