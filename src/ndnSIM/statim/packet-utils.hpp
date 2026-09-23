/**
 * Copyright (c) 2014-2016,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for the Statim simulation artifact, 2026-09-12.
// Upstream-derived portions: see THIRD-PARTY.md (repository root).
// Statim modifications and additional linking permission: see LICENSE.md.

#ifndef STATIM_UTILS_HPP
#define STATIM_UTILS_HPP

#include <memory>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/tlv.hpp>
#include <ndn-cxx/name.hpp>

namespace ns3
{
namespace ndn
{
namespace statim
{

inline bool judgeNameIsTrace(const ::ndn::Name& name)
{
  if (name.size() < 4)
  {
    return false;
  }
  return name[1].toUri() == "trace";
}

inline std::shared_ptr<::ndn::Name> extractDataNameFromTrace(const ::ndn::Name& name)
{
  auto dataName = std::make_shared<::ndn::Name>();
  if (name.size() < 4)
  {
    return dataName;
  }
  dataName->append(name[0]);
  dataName->append(name.getSubName(2, name.size() - 3));
  return dataName;
}

inline ::ndn::time::milliseconds extractTraceTtl(const ::ndn::Data& data)
{
  try
  {
    auto content = data.getContent();
    content.parse();
    auto innerData = content.get(0x06);
    innerData.parse();
    auto innerContent = innerData.get(::ndn::tlv::Content);
    innerContent.parse();
    auto expiryBlock = innerContent.get(0x6d);
    // This optional nested expiration field uses two-byte big-endian milliseconds.
    // This parser is retained for encoded-content fixtures. The KITE network
    // path obtains its route lifetime from the saved TI, independently of TD content.
    if (expiryBlock.value_size() != 2)
    {
      return ::ndn::time::minutes(5);
    }
    const uint8_t* val = expiryBlock.value();
    uint64_t ms = (static_cast<uint64_t>(val[0]) << 8) | static_cast<uint64_t>(val[1]);
    return ::ndn::time::milliseconds(ms);
  }
  catch (const ::ndn::tlv::Error&)
  {
    return ::ndn::time::minutes(5);
  }
}

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_UTILS_HPP
