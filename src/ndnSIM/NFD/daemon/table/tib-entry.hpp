/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
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

#ifndef NFD_DAEMON_TABLE_TIB_ENTRY_HPP
#define NFD_DAEMON_TABLE_TIB_ENTRY_HPP

#include "tib-nexthop.hpp"
#include "core/scheduler.hpp"

namespace nfd {

namespace name_tree {
class Entry;
} // namespace name_tree

namespace tib {

typedef std::vector<NextHop> NextHopList;

/** \brief represents a TIB entry, nexthops will have lifetime, when all nexthops timeout, this entry will be deleted
 */
class Entry : noncopyable
{
public:
  explicit
  Entry(const Name& prefix);

  const Name&
  getPrefix() const
  {
    return m_prefix;
  }

  const NextHopList&
  getNextHops() const
  {
    return m_nextHops;
  }

  /** \return whether this Entry has any NextHop record
   */
  bool
  hasNextHops() const
  {
    return !m_nextHops.empty();
  }

  /** \return whether there is a NextHop record for \p face
   */
  bool
  hasNextHop(const Face& face) const;

  /** \brief adds a NextHop record
   *
   *  If a NextHop record for \p face already exists, its cost is updated.
   */
  void
  addNextHop(Face& face, uint64_t cost);

  /** \brief removes a NextHop record
   *
   *  If no NextHop record for face exists, do nothing.
   */
  void
  removeNextHop(const Face& face);

private:
  /** \note This method is non-const because mutable iterators are needed by callers.
   */
  NextHopList::iterator
  findNextHop(const Face& face);

  /** \brief sorts the nexthop list
   */
  void
  sortNextHops();

public:
  void
  setNextHopTimer(const Face& face, scheduler::EventId event);

  NextHopList::iterator
  cancelNextHopTimer(const Face& face);

  void
  cancelAllNextHopTimers();

  bool
  judgeAndUpdateSeq(uint64_t seq);

private:
  Name m_prefix;
  NextHopList m_nextHops;

  name_tree::Entry* m_nameTreeEntry;

  uint64_t m_seq;

  friend class name_tree::Entry;
};

} // namespace tib
} // namespace nfd

#endif // NFD_DAEMON_TABLE_TIB_ENTRY_HPP
