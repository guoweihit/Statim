// SPDX-License-Identifier: GPL-3.0-or-later
// See the repository license and its additional permission for linking with ns-3.

#include "ns3/ndnSIM/statim/state/pending-interest-table.hpp"
#include "ns3/ndnSIM/statim/state/temporary-forwarding-table.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using namespace ns3::ndn::statim;
using ::ndn::Interest;
using ::ndn::Name;
using ::ndn::time::microseconds;
using ::ndn::time::milliseconds;

void check(bool condition, const char* expression, int line)
{
  if (!condition)
  {
    std::ostringstream message;
    message << "line " << line << ": " << expression;
    throw std::runtime_error(message.str());
  }
}

// Unlike assert, these checks also execute when the build defines NDEBUG.
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

struct ManualClock
{
  TimePoint now = TimePoint(microseconds(0));

  ClockFunction function()
  {
    return [this] { return now; };
  }
  void advance(Lifetime duration) { now += duration; }
};

Interest interest(const Name& name, std::uint32_t nonce, milliseconds lifetime = milliseconds(100))
{
  Interest result(name, lifetime);
  result.setNonce(nonce);
  return result;
}

void expectPorts(const std::set<Port>& actual, std::initializer_list<Port> expected)
{
  CHECK(actual == std::set<Port>(expected));
}

void expectHops(const ForwardingEntry* entry, const Name& prefix,
                std::initializer_list<Port> expected)
{
  CHECK(entry != nullptr);
  CHECK(entry->getPrefix() == prefix);
  std::set<Port> actual;
  for (const auto& hop : entry->getNextHops())
  {
    CHECK(hop.first == hop.second.port);
    actual.insert(hop.second.port);
  }
  expectPorts(actual, expected);
}

void pendingOwnsFirstStackInterest()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  CHECK(table.size() == 0);
  CHECK(!table.erase(Name("/absent")));
  PendingInterestTable::InterestView remembered;
  {
    Interest original = interest(Name("/stack/item"), 10, milliseconds(25));
    auto result = table.insert(original, 7);
    CHECK(result.accepted());
    CHECK(result.created());
    CHECK(!result.aggregated());
    CHECK(!result.duplicate());
    CHECK(result.interest.get() != &original);
    remembered = result.interest;
    original.setName(Name("/changed"));
    original.setInterestLifetime(milliseconds(999));
  }
  CHECK(table.size() == 1);
  CHECK(remembered->getName() == Name("/stack/item"));
  CHECK(remembered->getNonce() == 10);
  CHECK(remembered->getInterestLifetime() == milliseconds(25));
  CHECK(table.erase(Name("/stack/item")));
  CHECK(!table.erase(Name("/stack/item")));
  CHECK(table.size() == 0);
  CHECK(remembered->getName() == Name("/stack/item"));
}

void pendingDuplicateDoesNotAddOrPrune()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  auto first = table.insert(interest(Name("/duplicate"), 42, milliseconds(10)), 3);
  clock.advance(milliseconds(10));
  auto duplicate = table.insert(interest(Name("/duplicate"), 42, milliseconds(900)), 9);
  CHECK(!duplicate.accepted());
  CHECK(!duplicate.created());
  CHECK(!duplicate.aggregated());
  CHECK(duplicate.duplicate());
  CHECK(duplicate.interest == first.interest);
  CHECK(table.size() == 1);
  // Rejection preserves the previously stored ingress port 3.
  expectPorts(table.takeDataMatchPorts(Name("/duplicate/data")), {3});
  CHECK(table.size() == 0);
}

void pendingAggregationRemembersFirstNonce()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  auto first = table.insert(interest(Name("/aggregate"), 1), 8);
  auto second = table.insert(interest(Name("/aggregate"), 2), 4);
  CHECK(second.accepted());
  CHECK(!second.created());
  CHECK(second.aggregated());
  CHECK(!second.duplicate());
  CHECK(second.interest == first.interest);
  CHECK(second.interest->getNonce() == 1);
  // Duplicate detection is intentionally against the first Interest only.
  CHECK(table.insert(interest(Name("/aggregate"), 2), 4).aggregated());
  CHECK(table.insert(interest(Name("/aggregate"), 1), 6).duplicate());
  CHECK(table.size() == 1);
  expectPorts(table.takeDataMatchPorts(Name("/aggregate")), {4, 8});
}

void pendingUsesEachIngressInsertionTimeAndLifetime()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  table.insert(interest(Name("/expiry"), 1, milliseconds(10)), 1);
  clock.advance(milliseconds(5));
  table.insert(interest(Name("/expiry"), 2, milliseconds(20)), 2);
  clock.advance(milliseconds(5));
  CHECK(table.insert(interest(Name("/expiry"), 3, milliseconds(4)), 3).aggregated());
  // First ingress expires exactly at 10 ms; the ingress added at 5 ms survives.
  expectPorts(table.takeDataMatchPorts(Name("/expiry")), {2, 3});

  table.insert(interest(Name("/repeat"), 1, milliseconds(2)), 1);
  clock.advance(milliseconds(1));
  table.insert(interest(Name("/repeat"), 2, milliseconds(3)), 2);
  clock.advance(milliseconds(2));
  table.insert(interest(Name("/repeat"), 3, milliseconds(10)), 3);
  clock.advance(milliseconds(1));
  table.insert(interest(Name("/repeat"), 4, milliseconds(10)), 4);
  expectPorts(table.takeDataMatchPorts(Name("/repeat")), {3, 4});
}

void pendingConsumptionDoesNotIndependentlyExpire()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  table.insert(interest(Name("/elapsed"), 1, milliseconds(1)), 12);
  clock.advance(milliseconds(1000));
  CHECK(table.size() == 1);
  CHECK(table.findByPrefix(Name("/elapsed")).size() == 1);
  expectPorts(table.takeDataMatchPorts(Name("/elapsed/data")), {12});
  CHECK(table.takeDataMatchPorts(Name("/elapsed/data")).empty());
}

void pendingNonpositiveLifetimePolicy()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  table.insert(interest(Name("/zero"), 1, milliseconds(0)), 1);
  table.insert(interest(Name("/negative"), 1, milliseconds(-2)), 2);
  // Data satisfaction consumes these stored ingress records at their current age.
  expectPorts(table.takeDataMatchPorts(Name("/zero")), {1});
  expectPorts(table.takeDataMatchPorts(Name("/negative")), {2});

  table.insert(interest(Name("/zero"), 1, milliseconds(0)), 3);
  table.insert(interest(Name("/zero"), 2), 4);
  expectPorts(table.takeDataMatchPorts(Name("/zero")), {4});
  table.insert(interest(Name("/negative"), 1, milliseconds(-2)), 5);
  table.insert(interest(Name("/negative"), 2), 6);
  expectPorts(table.takeDataMatchPorts(Name("/negative")), {6});
}

void pendingConsumesAllAncestorsAndDeduplicatesPorts()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  table.insert(interest(Name("/"), 1), 9);
  table.insert(interest(Name("/a"), 2), 2);
  table.insert(interest(Name("/a/b"), 3), 9);
  table.insert(interest(Name("/a/b"), 4), 3);
  table.insert(interest(Name("/a/b/c"), 5), 4);
  table.insert(interest(Name("/a/b/c/child"), 6), 5);
  table.insert(interest(Name("/a/bb"), 7), 6);
  table.insert(interest(Name("/unrelated"), 8), 7);
  expectPorts(table.takeDataMatchPorts(Name("/a/b/c")), {2, 3, 4, 9});
  CHECK(table.size() == 3);
  CHECK(table.takeDataMatchPorts(Name("/a/b/c")).empty());
  CHECK(table.findByPrefix(Name("/a/b/c/child")).size() == 1);
  CHECK(table.findByPrefix(Name("/a/bb")).size() == 1);
  expectPorts(table.takeDataMatchPorts(Name("/a/b/c/child")), {5});

  table.insert(interest(Name("/"), 9), 11);
  expectPorts(table.takeDataMatchPorts(Name("/")), {11});
  CHECK(table.size() == 2);
}

void pendingDescendantOrderAndNonceAbsence()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  std::vector<Name> names = {Name("/p/zz"), Name("/p/a"), Name("/p"), Name("/p/bb")};
  table.insert(interest(names[0], 1), 1);
  table.insert(interest(names[1], 99), 2);
  Interest withoutNonce(names[2], milliseconds(100));
  CHECK(!withoutNonce.hasNonce());
  auto missing = table.insert(withoutNonce, 3);
  table.insert(interest(names[3], 50), 4);
  table.insert(interest(Name("/peer"), 0), 5);
  CHECK(!withoutNonce.hasNonce());
  CHECK(!missing.interest->hasNonce());

  const auto listed = table.findByPrefix(Name("/p"));
  std::sort(names.begin(), names.end());
  CHECK(listed.size() == names.size());
  for (std::size_t index = 0; index < names.size(); ++index)
  {
    CHECK(listed[index]->getName() == names[index]);
  }
  CHECK(!missing.interest->hasNonce());
  CHECK(table.size() == 5);
  CHECK(table.findByPrefix(Name("/")).size() == 5);
  CHECK(table.findByPrefix(Name("/missing")).empty());
  CHECK(table.erase(Name("/p")));
  CHECK(listed.front()->getName() == Name("/p"));
  CHECK(!listed.front()->hasNonce());
}

void pendingNonceMustBePresentOnBothInterests()
{
  ManualClock clock;
  PendingInterestTable table(clock.function());
  Interest noNonce(Name("/none"), milliseconds(100));
  auto first = table.insert(noNonce, 1);
  CHECK(table.insert(noNonce, 2).aggregated());
  CHECK(table.insert(interest(Name("/none"), 7), 3).aggregated());
  CHECK(table.insert(interest(Name("/none"), 7), 4).aggregated());
  CHECK(!noNonce.hasNonce());
  CHECK(!first.interest->hasNonce());
  expectPorts(table.takeDataMatchPorts(Name("/none")), {1, 2, 3, 4});

  table.insert(interest(Name("/present"), 7), 5);
  Interest laterWithoutNonce(Name("/present"), milliseconds(100));
  CHECK(table.insert(laterWithoutNonce, 6).aggregated());
  CHECK(!laterWithoutNonce.hasNonce());
  expectPorts(table.takeDataMatchPorts(Name("/present")), {5, 6});
}

void forwardingExactLongestAndRoot()
{
  ManualClock clock;
  TemporaryForwardingTable table(clock.function());
  CHECK(table.size() == 0);
  CHECK(table.findExact(Name("/")) == nullptr);
  CHECK(table.findLongestPrefix(Name("/anything")) == nullptr);
  CHECK(!table.erase(Name("/absent")));
  auto first = table.insert(Name("/"), 1, milliseconds(100));
  CHECK(first.prefixCreated);
  CHECK(first.hopAdded);
  auto second = table.insert(Name("/"), 2, milliseconds(100));
  CHECK(!second.prefixCreated);
  CHECK(second.hopAdded);
  table.insert(Name("/a"), 3, milliseconds(100));
  table.insert(Name("/a/b"), 4, milliseconds(100));
  expectHops(table.findExact(Name("/")), Name("/"), {1, 2});
  expectHops(table.findLongestPrefix(Name("/")), Name("/"), {1, 2});
  expectHops(table.findLongestPrefix(Name("/a/b/data")), Name("/a/b"), {4});
  expectHops(table.findLongestPrefix(Name("/a/b")), Name("/a/b"), {4});
  expectHops(table.findLongestPrefix(Name("/a/bb")), Name("/a"), {3});
  expectHops(table.findLongestPrefix(Name("/other")), Name("/"), {1, 2});
  CHECK(table.findExact(Name("/a/b/data")) == nullptr);
  CHECK(table.size() == 3);
}

void forwardingExpiryBoundaryAndFallback()
{
  ManualClock clock;
  TemporaryForwardingTable table(clock.function());
  table.insert(Name("/"), 1, microseconds(30));
  table.insert(Name("/a"), 2, microseconds(20));
  table.insert(Name("/a/b"), 3, microseconds(10));
  clock.advance(microseconds(9));
  expectHops(table.findLongestPrefix(Name("/a/b/c")), Name("/a/b"), {3});
  clock.advance(microseconds(1));
  expectHops(table.findLongestPrefix(Name("/a/b/c")), Name("/a"), {2});
  CHECK(table.size() == 2);
  CHECK(table.findExact(Name("/a/b")) == nullptr);
  clock.advance(microseconds(10));
  expectHops(table.findLongestPrefix(Name("/a/b/c")), Name("/"), {1});
  CHECK(table.size() == 1);
  clock.advance(microseconds(10));
  CHECK(table.findLongestPrefix(Name("/a/b/c")) == nullptr);
  CHECK(table.size() == 0);
}

void forwardingDuplicatePortDoesNotRenew()
{
  ManualClock clock;
  TemporaryForwardingTable table(clock.function());
  table.insert(Name("/route"), 7, microseconds(10));
  const TimePoint originalDeadline = table.findExact(Name("/route"))->getNextHops().at(7).deadline;
  CHECK(originalDeadline == clock.now + microseconds(10));
  clock.advance(microseconds(9));
  auto duplicate = table.insert(Name("/route"), 7, microseconds(100));
  CHECK(!duplicate.prefixCreated);
  CHECK(!duplicate.hopAdded);
  CHECK(table.findExact(Name("/route"))->getNextHops().at(7).deadline == originalDeadline);
  table.insert(Name("/route"), 8, microseconds(100));
  clock.advance(microseconds(1));
  expectHops(table.findExact(Name("/route")), Name("/route"), {8});
  auto restored = table.insert(Name("/route"), 7, microseconds(5));
  CHECK(!restored.prefixCreated);
  CHECK(restored.hopAdded);
  CHECK(table.findExact(Name("/route"))->getNextHops().at(7).deadline ==
        clock.now + microseconds(5));
}

void forwardingInsertPrunesExpiredHops()
{
  ManualClock clock;
  TemporaryForwardingTable table(clock.function());
  table.insert(Name("/route"), 1, microseconds(1));
  table.insert(Name("/route"), 2, microseconds(10));
  clock.advance(microseconds(1));
  auto replacement = table.insert(Name("/route"), 1, microseconds(20));
  CHECK(!replacement.prefixCreated);
  CHECK(replacement.hopAdded);
  expectHops(table.findExact(Name("/route")), Name("/route"), {1, 2});
  CHECK(table.findExact(Name("/route"))->getNextHops().at(1).deadline ==
        clock.now + microseconds(20));

  table.insert(Name("/all-expired"), 3, microseconds(1));
  clock.advance(microseconds(1));
  auto afterExpiry = table.insert(Name("/all-expired"), 4, microseconds(5));
  // Hop replacement retains the existing prefix allocation.
  CHECK(!afterExpiry.prefixCreated);
  CHECK(afterExpiry.hopAdded);
  expectHops(table.findExact(Name("/all-expired")), Name("/all-expired"), {4});
}

void forwardingNonpositiveLifetimes()
{
  ManualClock clock;
  TemporaryForwardingTable table(clock.function());
  table.insert(Name("/"), 1, microseconds(10));
  auto zero = table.insert(Name("/zero"), 2, microseconds(0));
  CHECK(zero.prefixCreated);
  CHECK(zero.hopAdded);
  CHECK(table.findExact(Name("/zero")) == nullptr);
  table.insert(Name("/negative"), 3, microseconds(-1));
  expectHops(table.findLongestPrefix(Name("/negative/data")), Name("/"), {1});
  CHECK(table.findExact(Name("/negative")) == nullptr);
  CHECK(table.size() == 1);

  table.insert(Name("/mixed"), 4, microseconds(5));
  table.insert(Name("/mixed"), 5, microseconds(0));
  expectHops(table.findExact(Name("/mixed")), Name("/mixed"), {4});
  auto duplicate = table.insert(Name("/mixed"), 4, microseconds(-10));
  CHECK(!duplicate.hopAdded);
  expectHops(table.findExact(Name("/mixed")), Name("/mixed"), {4});
}

void forwardingExplicitReplacementKeepsSingleCurrentHop()
{
  ManualClock clock;
  TemporaryForwardingTable table(clock.function());
  const Name prefix("/mobile/producer");
  for (Port port = 1; port <= 6; ++port)
  {
    CHECK(table.erase(prefix) == (port != 1));
    auto result = table.insert(prefix, port, milliseconds(100));
    CHECK(result.prefixCreated);
    CHECK(result.hopAdded);
    expectHops(table.findLongestPrefix(Name("/mobile/producer/data")), prefix, {port});
    CHECK(table.size() == 1);
    clock.advance(milliseconds(1));
  }
  CHECK(table.erase(prefix));
  CHECK(!table.erase(prefix));
  CHECK(table.findExact(prefix) == nullptr);
}

void bothTablesSupportNamesLongerThan32Components()
{
  ManualClock clock;
  PendingInterestTable pending(clock.function());
  TemporaryForwardingTable forwarding(clock.function());
  std::string uri;
  for (int index = 0; index < 48; ++index)
  {
    uri += "/component" + std::to_string(index);
  }
  const Name longName(uri);
  const Name longAncestor = longName.getPrefix(40);
  CHECK(longName.size() == 48);
  pending.insert(interest(longAncestor, 1), 1);
  pending.insert(interest(longName, 2), 2);
  CHECK(pending.findByPrefix(longAncestor).size() == 2);
  expectPorts(pending.takeDataMatchPorts(Name(uri + "/data")), {1, 2});
  CHECK(pending.size() == 0);

  forwarding.insert(longAncestor, 3, microseconds(10));
  forwarding.insert(longName, 4, microseconds(1));
  expectHops(forwarding.findExact(longName), longName, {4});
  expectHops(forwarding.findLongestPrefix(Name(uri + "/data")), longName, {4});
  clock.advance(microseconds(1));
  expectHops(forwarding.findLongestPrefix(Name(uri + "/data")), longAncestor, {3});
  CHECK(forwarding.size() == 1);
}

void instancesAndTablesRemainIndependent()
{
  ManualClock firstClock;
  ManualClock secondClock;
  PendingInterestTable firstPending(firstClock.function());
  PendingInterestTable secondPending(secondClock.function());
  TemporaryForwardingTable firstForwarding(firstClock.function());
  TemporaryForwardingTable secondForwarding(secondClock.function());
  const Name name("/same");
  firstPending.insert(interest(name, 1), 1);
  secondPending.insert(interest(name, 1), 2);
  firstForwarding.insert(name, 3, microseconds(1));
  secondForwarding.insert(name, 4, microseconds(1));
  firstClock.advance(microseconds(1));
  CHECK(firstForwarding.findExact(name) == nullptr);
  expectHops(secondForwarding.findExact(name), name, {4});
  expectPorts(firstPending.takeDataMatchPorts(name), {1});
  CHECK(firstPending.size() == 0);
  CHECK(secondPending.size() == 1);
  expectPorts(secondPending.takeDataMatchPorts(name), {2});
  expectHops(secondForwarding.findExact(name), name, {4});
}

} // namespace

int main()
{
  struct TestCase
  {
    const char* name;
    void (*run)();
  };
  const TestCase cases[] = {
      {"pending owns first stack Interest", pendingOwnsFirstStackInterest},
      {"pending duplicate does not add or prune", pendingDuplicateDoesNotAddOrPrune},
      {"pending aggregation remembers first nonce", pendingAggregationRemembersFirstNonce},
      {"pending uses ingress insertion time and lifetime",
       pendingUsesEachIngressInsertionTimeAndLifetime},
      {"pending consumption does not independently expire",
       pendingConsumptionDoesNotIndependentlyExpire},
      {"pending nonpositive lifetime policy", pendingNonpositiveLifetimePolicy},
      {"pending consumes all ancestors and deduplicates ports",
       pendingConsumesAllAncestorsAndDeduplicatesPorts},
      {"pending descendant order and nonce absence", pendingDescendantOrderAndNonceAbsence},
      {"pending nonce must be present on both Interests", pendingNonceMustBePresentOnBothInterests},
      {"forwarding exact, longest, and root", forwardingExactLongestAndRoot},
      {"forwarding expiry boundary and fallback", forwardingExpiryBoundaryAndFallback},
      {"forwarding duplicate port does not renew", forwardingDuplicatePortDoesNotRenew},
      {"forwarding insertion prunes expired hops", forwardingInsertPrunesExpiredHops},
      {"forwarding nonpositive lifetimes", forwardingNonpositiveLifetimes},
      {"forwarding replacement keeps single current hop",
       forwardingExplicitReplacementKeepsSingleCurrentHop},
      {"both tables support names longer than 32 components",
       bothTablesSupportNamesLongerThan32Components},
      {"instances and tables remain independent", instancesAndTablesRemainIndependent},
  };

  std::size_t failures = 0;
  for (const auto& test : cases)
  {
    try
    {
      test.run();
      std::cout << "PASS " << test.name << '\n';
    }
    catch (const std::exception& error)
    {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    }
    catch (...)
    {
      ++failures;
      std::cerr << "FAIL " << test.name << ": unknown exception\n";
    }
  }
  const std::size_t total = sizeof(cases) / sizeof(cases[0]);
  std::cout << total - failures << '/' << total << " state-table cases passed\n";
  return failures == 0 ? 0 : 1;
}
