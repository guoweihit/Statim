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

#include "tib.hpp"
#include "pit-entry.hpp"
#include "measurements-entry.hpp"

#include <boost/concept/assert.hpp>
#include <boost/concept_check.hpp>
#include <type_traits>

namespace nfd {
namespace tib {

const unique_ptr<Entry> Tib::s_emptyEntry = make_unique<Entry>(Name());

// http://en.cppreference.com/w/cpp/concept/ForwardIterator
BOOST_CONCEPT_ASSERT((boost::ForwardIterator<Tib::const_iterator>));
// boost::ForwardIterator follows SGI standard http://www.sgi.com/tech/stl/ForwardIterator.html,
// which doesn't require DefaultConstructible
#ifdef HAVE_IS_DEFAULT_CONSTRUCTIBLE
static_assert(std::is_default_constructible<Tib::const_iterator>::value,
              "Tib::const_iterator must be default-constructible");
#else
BOOST_CONCEPT_ASSERT((boost::DefaultConstructible<Tib::const_iterator>));
#endif // HAVE_IS_DEFAULT_CONSTRUCTIBLE

static inline bool
nteHasTibEntry(const name_tree::Entry& nte)
{
  return nte.getTibEntry() != nullptr;
}

Tib::Tib(NameTree& nameTree)
  : m_nameTree(nameTree)
  , m_nItems(0)
{
}

template<typename K>
const Entry&
Tib::findLongestPrefixMatchImpl(const K& key) const
{
  name_tree::Entry* nte = m_nameTree.findLongestPrefixMatch(key, &nteHasTibEntry);
  if (nte != nullptr) {
    return *nte->getTibEntry();
  }
  return *s_emptyEntry;
}

const Entry&
Tib::findLongestPrefixMatch(const Name& prefix) const
{
  return this->findLongestPrefixMatchImpl(prefix);
}

const Entry&
Tib::findLongestPrefixMatch(const pit::Entry& pitEntry) const
{
  return this->findLongestPrefixMatchImpl(pitEntry);
}

const Entry&
Tib::findLongestPrefixMatch(const measurements::Entry& measurementsEntry) const
{
  return this->findLongestPrefixMatchImpl(measurementsEntry);
}

Entry*
Tib::findExactMatch(const Name& prefix)
{
  name_tree::Entry* nte = m_nameTree.findExactMatch(prefix);
  if (nte != nullptr)
    return nte->getTibEntry();

  return nullptr;
}

const Entry*
Tib::findExactMatch(const pit::Entry& pitEntry) const
{
  name_tree::Entry* nte = m_nameTree.findExactMatch(pitEntry.getInterest().getName());
  if (nte != nullptr)
    return nte->getTibEntry();

  return nullptr;
}

std::pair<Entry*, bool>
Tib::insert(const Name& prefix)
{
  name_tree::Entry& nte = m_nameTree.lookup(prefix);
  Entry* entry = nte.getTibEntry();
  if (entry != nullptr) {
    return std::make_pair(entry, false);
  }

  nte.setTibEntry(make_unique<Entry>(prefix));
  ++m_nItems;
  return std::make_pair(nte.getTibEntry(), true);
}

void
Tib::erase(name_tree::Entry* nte, bool canDeleteNte)
{
  BOOST_ASSERT(nte != nullptr);

  nte->setTibEntry(nullptr);
  if (canDeleteNte) {
    m_nameTree.eraseIfEmpty(nte);
  }
  --m_nItems;
}

void
Tib::erase(const Name& prefix)
{
  name_tree::Entry* nte = m_nameTree.findExactMatch(prefix);
  if (nte != nullptr) {
    this->erase(nte);
  }
}

void
Tib::erase(const Entry& entry)
{
  name_tree::Entry* nte = m_nameTree.getEntry(entry);
  if (nte == nullptr) { // don't try to erase s_emptyEntry
    BOOST_ASSERT(&entry == s_emptyEntry.get());
    return;
  }
  this->erase(nte);
}

void
Tib::removeNextHop(Entry& entry, const Face& face)
{
  entry.removeNextHop(face);

  if (!entry.hasNextHops()) {
    name_tree::Entry* nte = m_nameTree.getEntry(entry);
    this->erase(nte, false);
  }
}

Tib::Range
Tib::getRange() const
{
  return m_nameTree.fullEnumerate(&nteHasTibEntry) |
         boost::adaptors::transformed(name_tree::GetTableEntry<Entry>(&name_tree::Entry::getTibEntry));
}

} // namespace tib
} // namespace nfd
