// Statim simulation modifications, 2026-09-12. Original notices retained below.
/**
 * Copyright (c) 2011-2016  Regents of the University of California.
 *
 * This file is part of ndnSIM. See AUTHORS for complete list of ndnSIM authors and
 * contributors.
 *
 * ndnSIM is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndnSIM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndnSIM, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

#include "ndn-net-device-transport.hpp"

#include "../helper/ndn-stack-helper.hpp"
#include "ndn-block-header.hpp"
#include "ndn-l3-protocol.hpp"
#include "../utils/ndn-ns3-packet-tag.hpp"
#include "ns3/ndnSIM/statim/forwarding-engine.hpp"
#include "ns3/ndnSIM/statim/packet-header.hpp"

#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/lp/packet.hpp>
#include <ndn-cxx/lp/tags.hpp>

NS_LOG_COMPONENT_DEFINE("ndn.NetDeviceTransport");

namespace ns3 {
namespace ndn {

NetDeviceTransport::NetDeviceTransport(Ptr<Node> node,
                                       const Ptr<NetDevice>& netDevice,
                                       const std::string& localUri,
                                       const std::string& remoteUri,
                                       bool enableStatimPacket,
                                       ::ndn::nfd::FaceScope scope,
                                       ::ndn::nfd::FacePersistency persistency,
                                       ::ndn::nfd::LinkType linkType)
  : m_netDevice(netDevice)
  , m_node(node)
  , m_enableStatimPacket(enableStatimPacket)
{
  this->setLocalUri(FaceUri(localUri));
  this->setRemoteUri(FaceUri(remoteUri));
  this->setScope(scope);
  this->setPersistency(persistency);
  this->setLinkType(linkType);
  // this->setMtu(udp::computeMtu(m_socket.local_endpoint())); // not sure what should be here

  NS_LOG_FUNCTION(this << "Creating an ndnSIM transport instance for netDevice with URI"
                  << this->getLocalUri());

  NS_ASSERT_MSG(m_netDevice != 0, "NetDeviceFace needs to be assigned a valid NetDevice");

  m_node->RegisterProtocolHandler(MakeCallback(&NetDeviceTransport::receiveFromNetDevice, this),
                                  L3Protocol::ETHERNET_FRAME_TYPE, m_netDevice,
                                  true );
}

NetDeviceTransport::~NetDeviceTransport()
{
  NS_LOG_FUNCTION_NOARGS();
}

void
NetDeviceTransport::beforeChangePersistency(::ndn::nfd::FacePersistency newPersistency)
{
  NS_LOG_FUNCTION(this << "Changing persistency for netDevice with URI"
                  << this->getLocalUri() << "currently does nothing");
  // do nothing for now
}

void
NetDeviceTransport::doClose()
{
  NS_LOG_FUNCTION(this << "Closing transport for netDevice with URI"
                  << this->getLocalUri());

  // set the state of the transport to "CLOSED"
  this->setState(nfd::face::TransportState::CLOSED);
}

void
NetDeviceTransport::doSend(Packet&& packet)
{
  NS_LOG_FUNCTION(this << "Sending packet from netDevice with URI"
                  << this->getLocalUri());
  Ptr<L3Protocol> l3 = m_node->GetObject<L3Protocol>();
  if (l3->getEnableStatimPacket() && !l3->isUsingStatimEngine()) {
    ::ndn::Block lpBlock = packet.packet;

    ::ndn::Block ndnBlock = lpBlock;
    uint64_t hopCount = 0;
    if (lpBlock.type() == ::ndn::lp::tlv::LpPacket) {
      ::ndn::lp::Packet lpPkt(lpBlock);
      if (lpPkt.has<::ndn::lp::HopCountTagField>()) {
        hopCount = lpPkt.get<::ndn::lp::HopCountTagField>();
      }
      if (lpPkt.has<::ndn::lp::FragmentField>()) {
        ::ndn::Buffer::const_iterator fragBegin, fragEnd;
        std::tie(fragBegin, fragEnd) = lpPkt.get<::ndn::lp::FragmentField>(0);
        ndnBlock = ::ndn::Block(&*fragBegin, std::distance(fragBegin, fragEnd));
      }
    }

    ::ndn::Name name;
    if (ndnBlock.type() == ::ndn::tlv::Interest) {
      name = ::ndn::Interest(ndnBlock).getName();
    }
    else if (ndnBlock.type() == ::ndn::tlv::Data) {
      name = ::ndn::Data(ndnBlock).getName();
    }
    statim::PacketHeader statimHeader(name);

    if (ndnBlock.type() == ::ndn::tlv::Data) {
      statimHeader.setSwitchId(static_cast<uint8_t>(hopCount));
    }

    BlockHeader blockHeader(ndnBlock);
    Ptr<ns3::Packet> ns3Packet = Create<ns3::Packet>();
    ns3Packet->AddHeader(blockHeader);
    ns3Packet->AddHeader(statimHeader);
    m_netDevice->Send(ns3Packet, m_netDevice->GetBroadcast(), L3Protocol::ETHERNET_FRAME_TYPE);
    return;
  }

  BlockHeader header(packet);
  Ptr<ns3::Packet> ns3Packet = Create<ns3::Packet>();
  ns3Packet->AddHeader(header);
  m_netDevice->Send(ns3Packet, m_netDevice->GetBroadcast(), L3Protocol::ETHERNET_FRAME_TYPE);
}

// callback
void
NetDeviceTransport::receiveFromNetDevice(Ptr<NetDevice> device,
                                      Ptr<const ns3::Packet> p,
                                      uint16_t protocol,
                                      const Address& from, const Address& to,
                                      NetDevice::PacketType packetType)
{
  NS_LOG_FUNCTION(device << p << protocol << from << to << packetType);
  Ptr<L3Protocol> l3 = m_node->GetObject<L3Protocol>();

  if (l3 != nullptr && l3->isUsingStatimEngine()) {
    auto engine = l3->getStatimEngine();
    uint16_t port = engine->getPortTable().getPortByRaw(PeekPointer(device));
    engine->onReceiveRawPacket(port, p);
    return;
  }

  if (l3 != nullptr && l3->getEnableStatimPacket()) {
    Ptr<ns3::Packet> packet = p->Copy();

    statim::PacketHeader statimHeader;
    if (packet->RemoveHeader(statimHeader) != statim::PacketHeader::SERIALIZED_SIZE) {
      return;
    }
    uint8_t hopCount = statimHeader.getSwitchId();

    BlockHeader blockHeader;
    packet->RemoveHeader(blockHeader);
    ::ndn::Block ndnBlock = blockHeader.getBlock();

    ::ndn::lp::Packet lpPkt(ndnBlock);
    lpPkt.add<::ndn::lp::HopCountTagField>(hopCount);

    auto nfdPacket = Packet(lpPkt.wireEncode());
    this->receive(std::move(nfdPacket));
    return;
  }

  Ptr<ns3::Packet> packet = p->Copy();
  BlockHeader header;
  packet->RemoveHeader(header);
  auto nfdPacket = Packet(std::move(header.getBlock()));
  this->receive(std::move(nfdPacket));
}

Ptr<NetDevice>
NetDeviceTransport::GetNetDevice() const
{
  return m_netDevice;
}

} // namespace ndn
} // namespace ns3
