/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <gtest/gtest.h>

#include "fboss/agent/LookupClassRouteUpdater.h"
#include "fboss/agent/NeighborUpdater.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/RouteUpdater.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/test/HwTestHandle.h"
#include "fboss/agent/test/TestUtils.h"

#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>

using folly::IPAddressV4;
using folly::IPAddressV6;

namespace facebook::fboss {

template <typename AddrT>
class LookupClassRouteUpdaterTest : public ::testing::Test {
 public:
  using Func = std::function<void()>;
  using StateUpdateFn = SwSwitch::StateUpdateFn;

  void SetUp() override {
    handle_ = createTestHandle(testStateAWithLookupClasses());
    sw_ = handle_->getSw();
  }

  void verifyStateUpdate(Func func) {
    runInUpdateEventBaseAndWait(std::move(func));
  }

  void verifyStateUpdateAfterNeighborCachePropagation(Func func) {
    // internally, calls through neighbor updater are proxied to the
    // NeighborCache thread, which can eventually end up scheduling a
    // new state update back on to the update evb. This helper waits
    // until the change has made it through all these proxy layers.
    runInUpdateEvbAndWaitAfterNeighborCachePropagation(std::move(func));
  }

  void TearDown() override {
    schedulePendingTestStateUpdates();
  }

  void updateState(folly::StringPiece name, StateUpdateFn func) {
    sw_->updateStateBlocking(name, func);
  }

  VlanID kVlan() const {
    return VlanID(1);
  }

  PortID kPortID() const {
    return PortID(1);
  }

  ClientID kClientID() const {
    return ClientID(1001);
  }

  RouterID kRid() const {
    return RouterID{0};
  }

  InterfaceID kInterfaceID() const {
    return InterfaceID(1);
  }

  MacAddress kMacAddressA() const {
    return MacAddress("01:02:03:04:05:06");
  }

  MacAddress kMacAddressB() const {
    return MacAddress("01:02:03:04:05:07");
  }

  AddrT kIpAddressA() {
    if constexpr (std::is_same_v<AddrT, folly::IPAddressV4>) {
      return IPAddressV4("10.0.0.2");
    } else {
      return IPAddressV6("2401:db00:2110:3001::0002");
    }
  }

  AddrT kIpAddressB() {
    if constexpr (std::is_same_v<AddrT, folly::IPAddressV4>) {
      return IPAddressV4("10.0.0.3");
    } else {
      return IPAddressV6("2401:db00:2110:3001::0003");
    }
  }

  AddrT kIpAddressC() {
    if constexpr (std::is_same_v<AddrT, folly::IPAddressV4>) {
      return IPAddressV4("10.0.0.4");
    } else {
      return IPAddressV6("2401:db00:2110:3001::0004");
    }
  }

  RoutePrefix<AddrT> kroutePrefix1() const {
    if constexpr (std::is_same_v<AddrT, folly::IPAddressV4>) {
      return RoutePrefix<AddrT>{folly::IPAddressV4{"10.1.4.0"}, 24};
    } else {
      return RoutePrefix<AddrT>{folly::IPAddressV6{"2803:6080:d038:3066::"},
                                64};
    }
  }

  RoutePrefix<AddrT> kroutePrefix2() const {
    if constexpr (std::is_same_v<AddrT, folly::IPAddressV4>) {
      return RoutePrefix<AddrT>{folly::IPAddressV4{"11.1.4.0"}, 24};
    } else {
      return RoutePrefix<AddrT>{folly::IPAddressV6{"2803:6080:d038:3067::"},
                                64};
    }
  }

  void resolveNeighbor(AddrT ipAddress, MacAddress macAddress) {
    /*
     * Cause a neighbor entry to resolve by receiving appropriate ARP/NDP, and
     * assert if valid CLASSID is associated with the newly resolved neighbor.
     */
    if constexpr (std::is_same<AddrT, folly::IPAddressV4>::value) {
      sw_->getNeighborUpdater()->receivedArpMine(
          kVlan(),
          ipAddress,
          macAddress,
          PortDescriptor(kPortID()),
          ArpOpCode::ARP_OP_REPLY);
    } else {
      sw_->getNeighborUpdater()->receivedNdpMine(
          kVlan(),
          ipAddress,
          macAddress,
          PortDescriptor(kPortID()),
          ICMPv6Type::ICMPV6_TYPE_NDP_NEIGHBOR_ADVERTISEMENT,
          0);
    }

    sw_->getNeighborUpdater()->waitForPendingUpdates();
    waitForBackgroundThread(sw_);
    waitForStateUpdates(sw_);
    sw_->getNeighborUpdater()->waitForPendingUpdates();
    waitForStateUpdates(sw_);
  }

  void unresolveNeighbor(folly::IPAddress ipAddress) {
    sw_->getNeighborUpdater()->flushEntry(kVlan(), ipAddress);

    sw_->getNeighborUpdater()->waitForPendingUpdates();
    waitForBackgroundThread(sw_);
    waitForStateUpdates(sw_);
  }

  void verifyClassIDHelper(
      RoutePrefix<AddrT> routePrefix,
      std::optional<cfg ::AclLookupClass> classID) {
    this->verifyStateUpdateAfterNeighborCachePropagation([=]() {
      auto state = sw_->getState();
      auto vlan = state->getVlans()->getVlan(kVlan());
      auto routeTableRib = state->getRouteTables()
                               ->getRouteTable(kRid())
                               ->template getRib<AddrT>();

      auto route = routeTableRib->routes()->getRouteIf(routePrefix);
      XLOG(DBG) << route->str();
      EXPECT_EQ(route->getClassID(), classID);
    });
  }

  void addRoute(RoutePrefix<AddrT> routePrefix, std::vector<AddrT> nextHops) {
    this->updateState(
        "Add new route", [=](const std::shared_ptr<SwitchState>& state) {
          auto newState = state->clone();
          auto routeTables = state->getRouteTables();

          RouteNextHopSet nexthops;
          for (const auto& nextHop : nextHops) {
            nexthops.emplace(UnresolvedNextHop(nextHop, UCMP_DEFAULT_WEIGHT));
          }

          RouteUpdater updater(routeTables);
          updater.addRoute(
              this->kRid(),
              routePrefix.network,
              routePrefix.mask,
              this->kClientID(),
              RouteNextHopEntry(nexthops, AdminDistance::MAX_ADMIN_DISTANCE));

          auto newRouteTables = updater.updateDone();
          newRouteTables->publish();
          newState->resetRouteTables(newRouteTables);

          return newState;
        });

    waitForStateUpdates(this->sw_);
    this->sw_->getNeighborUpdater()->waitForPendingUpdates();
    waitForBackgroundThread(this->sw_);
    waitForStateUpdates(this->sw_);
  }

  void removeNeighbor(const AddrT& ip) {
    this->updateState(
        "Add new route", [=](const std::shared_ptr<SwitchState>& state) {
          auto newState = state->clone();

          VlanID vlanId = newState->getInterfaces()
                              ->getInterface(this->kInterfaceID())
                              ->getVlanID();
          Vlan* vlan = newState->getVlans()->getVlanIf(VlanID(vlanId)).get();
          auto* neighborTable =
              vlan->template getNeighborEntryTable<AddrT>().get()->modify(
                  &vlan, &newState);
          neighborTable->removeEntry(ip);
          return newState;
        });
    waitForStateUpdates(this->sw_);
    this->sw_->getNeighborUpdater()->waitForPendingUpdates();
    waitForBackgroundThread(this->sw_);
    waitForStateUpdates(this->sw_);
  }

 protected:
  void runInUpdateEventBaseAndWait(Func func) {
    auto* evb = sw_->getUpdateEvb();
    evb->runInEventBaseThreadAndWait(std::move(func));
  }

  void runInUpdateEvbAndWaitAfterNeighborCachePropagation(Func func) {
    schedulePendingTestStateUpdates();
    this->sw_->getNeighborUpdater()->waitForPendingUpdates();
    runInUpdateEventBaseAndWait(std::move(func));
  }

  void schedulePendingTestStateUpdates() {
    runInUpdateEventBaseAndWait([]() {});
  }

  std::unique_ptr<HwTestHandle> handle_;
  SwSwitch* sw_;
};

using TestTypes = ::testing::Types<folly::IPAddressV4, folly::IPAddressV6>;

TYPED_TEST_CASE(LookupClassRouteUpdaterTest, TestTypes);

// Test cases verifying Route changes

TYPED_TEST(LookupClassRouteUpdaterTest, AddRouteThenResolveNextHop) {
  this->addRoute(this->kroutePrefix1(), {this->kIpAddressA()});
  this->resolveNeighbor(this->kIpAddressA(), this->kMacAddressA());

  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);
}

TYPED_TEST(LookupClassRouteUpdaterTest, ResolveNextHopThenAddRoute) {
  this->resolveNeighbor(this->kIpAddressA(), this->kMacAddressA());
  this->addRoute(this->kroutePrefix1(), {this->kIpAddressA()});

  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);
}

TYPED_TEST(LookupClassRouteUpdaterTest, AddRouteThenResolveTwoNextHop) {
  this->addRoute(
      this->kroutePrefix1(), {this->kIpAddressA(), this->kIpAddressB()});
  this->resolveNeighbor(this->kIpAddressA(), this->kMacAddressA());
  this->resolveNeighbor(this->kIpAddressB(), this->kMacAddressB());

  /*
   * In the current implementation, route inherits classID of the first
   * reachable nexthop. This test verifies that.
   * An accidental change in those semantics would be caught by this test
   * failure.
   */
  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);
}

TYPED_TEST(LookupClassRouteUpdaterTest, ResolveTwoNextHopThenAddRoute) {
  this->resolveNeighbor(this->kIpAddressA(), this->kMacAddressA());
  this->resolveNeighbor(this->kIpAddressB(), this->kMacAddressB());
  this->addRoute(
      this->kroutePrefix1(), {this->kIpAddressA(), this->kIpAddressB()});

  /*
   * In the current implementation, route inherits classID of the first
   * reachable nexthop. This test verifies that.
   * An accidental change in those semantics would be caught by this test
   * failure.
   */
  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);
}

TYPED_TEST(LookupClassRouteUpdaterTest, AddRouteThenResolveSecondNextHopOnly) {
  this->addRoute(
      this->kroutePrefix1(), {this->kIpAddressA(), this->kIpAddressB()});
  this->resolveNeighbor(this->kIpAddressB(), this->kMacAddressB());

  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);
}

TYPED_TEST(LookupClassRouteUpdaterTest, ResolveSecondNextHopOnlyThenAddRoute) {
  this->resolveNeighbor(this->kIpAddressB(), this->kMacAddressB());
  this->addRoute(
      this->kroutePrefix1(), {this->kIpAddressA(), this->kIpAddressB()});

  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);
}

TYPED_TEST(
    LookupClassRouteUpdaterTest,
    AddRouteWithTwoNextHopsThenUnresolveFirstNextHop) {
  this->addRoute(
      this->kroutePrefix1(), {this->kIpAddressA(), this->kIpAddressB()});
  this->resolveNeighbor(this->kIpAddressA(), this->kMacAddressA());
  this->resolveNeighbor(this->kIpAddressB(), this->kMacAddressB());

  /*
   * In the current implementation, route inherits classID of the first
   * reachable nexthop. This test verifies that.
   * An accidental change in those semantics would be caught by this test
   * failure.
   */
  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);

  this->unresolveNeighbor(this->kIpAddressA());
  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_1);
}

TYPED_TEST(
    LookupClassRouteUpdaterTest,
    AddRouteWithTwoNextHopsThenRemoveFirstNextHop) {
  this->addRoute(
      this->kroutePrefix1(), {this->kIpAddressA(), this->kIpAddressB()});
  this->resolveNeighbor(this->kIpAddressA(), this->kMacAddressA());
  this->resolveNeighbor(this->kIpAddressB(), this->kMacAddressB());

  /*
   * In the current implementation, route inherits classID of the first
   * reachable nexthop. This test verifies that.
   * An accidental change in those semantics would be caught by this test
   * failure.
   */
  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);

  this->removeNeighbor(this->kIpAddressA());
  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_1);
}

TYPED_TEST(LookupClassRouteUpdaterTest, ChangeNextHopSet) {
  this->addRoute(this->kroutePrefix1(), {this->kIpAddressA()});
  this->verifyClassIDHelper(this->kroutePrefix1(), std::nullopt);

  this->resolveNeighbor(this->kIpAddressB(), this->kMacAddressB());

  this->addRoute(
      this->kroutePrefix1(), {this->kIpAddressA(), this->kIpAddressB()});

  this->verifyClassIDHelper(
      this->kroutePrefix1(), cfg::AclLookupClass::CLASS_QUEUE_PER_HOST_QUEUE_0);
}

} // namespace facebook::fboss