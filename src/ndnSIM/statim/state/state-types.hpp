// SPDX-License-Identifier: GPL-3.0-or-later
// See the repository license and its additional permission for linking with ns-3.

#ifndef STATIM_STATE_TYPES_HPP
#define STATIM_STATE_TYPES_HPP

#include <ndn-cxx/util/time.hpp>

#include <cstdint>
#include <functional>

namespace ns3
{
namespace ndn
{
namespace statim
{

using Port = std::uint16_t;
using StateClock = ::ndn::time::steady_clock;
using TimePoint = StateClock::time_point;
using Lifetime = ::ndn::time::microseconds;
using ClockFunction = std::function<TimePoint()>;

} // namespace statim
} // namespace ndn
} // namespace ns3

#endif // STATIM_STATE_TYPES_HPP
