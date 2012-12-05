/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __HIERARCHICAL_ALLOCATOR_PROCESS_HPP__
#define __HIERARCHICAL_ALLOCATOR_PROCESS_HPP__

#include <process/delay.hpp>
#include <process/timeout.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/stopwatch.hpp>

#include "common/resources.hpp"

#include "master/allocator.hpp"
#include "master/master.hpp"
#include "master/sorter.hpp"


namespace mesos {
namespace internal {
namespace master {

// Forward declarations.
class Filter;

// Our struct to keep track
// of the different resource types
// for the purposes of resources revocation
struct FrameworkResources 
{
  FrameworkResources() {};
  Resources activeRes;  // currently used on slave
  Resources guaranteedRes;
  Resources revokedRes; // what we told it to lose, overlaps w/ activeRes
};

// Implements the basic allocator algorithm - first pick
// a user by some criteria, then pick one of their
// frameworks to allocate to.
template <class UserSorter, class FrameworkSorter>
class HierarchicalAllocatorProcess : public AllocatorProcess
{
public:
  HierarchicalAllocatorProcess() : initialized(false) {}

  virtual ~HierarchicalAllocatorProcess() {}

  process::PID<HierarchicalAllocatorProcess<UserSorter, FrameworkSorter> > self()
  {
    return process::PID<HierarchicalAllocatorProcess<UserSorter, FrameworkSorter> >(this);
  }

  void initialize(const Flags& flags,
		  const process::PID<Master>& _master);

  void frameworkAdded(const FrameworkID& frameworkId,
			      const FrameworkInfo& frameworkInfo,
			      const Resources& used);

  void frameworkRemoved(const FrameworkID& frameworkId);

  void frameworkActivated(const FrameworkID& frameworkId,
                                  const FrameworkInfo& frameworkInfo);

  void frameworkDeactivated(const FrameworkID& frameworkId);

  void slaveAdded(const SlaveID& slaveId,
		  const SlaveInfo& slaveInfo,
		  const hashmap<FrameworkID, Resources>& used);

  void slaveRemoved(const SlaveID& slaveId);

  void updateWhitelist(
      const Option<hashset<std::string> >& whitelist);

  void resourcesRequested(
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  void resourcesUnused(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resourcesUnused,
      const Resources& resourcesUsed,
      const Option<Filters>& filters);

  void resourcesRecovered(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& resources);

  void offersRevived(const FrameworkID& frameworkId);

protected:
  // Allocate any allocatable resources.
  hashmap<SlaveID, Resources> filterAllocatable(const hashset<SlaveID>& slaveIds, bool writeLog=false);
  
  // Callback for doing batch allocations.
  void batch();

  // Check and call for resource revocation
  void checkUtilization();

  // Allocate any allocatable resources.
  void allocate();

  // Allocate resources just from the specified slave.
  void allocate(const SlaveID& slaveId);

  // Allocate resources from the specified slaves.
  void allocate(const hashset<SlaveID>& slaveIds);

  // Remove a filter for the specified framework.
  void expire(const FrameworkID& frameworkId, Filter* filter);

  // Checks whether the slave is whitelisted.
  bool isWhitelisted(const SlaveID& slave);

  // Returns true if there is a filter for this framework
  // on this slave.
  bool isFiltered(const FrameworkID& frameworkId,
		  const SlaveID& slaveId,
		  const Resources& resources);

  bool initialized;

  Flags flags;
  PID<Master> master;

  // Maps FrameworkIDs to user names.
  hashmap<FrameworkID, std::string> users;

  // Maps user names to the Sorter object which contains
  // all of that user's frameworks.
  hashmap<std::string, FrameworkSorter*> sorters;

  // Maps slaves to their allocatable resources.
  hashmap<SlaveID, Resources> allocatable;

  // Contains all active slaves.
  hashmap<SlaveID, SlaveInfo> slaves;

  // Filters that have been added by frameworks.
  multihashmap<FrameworkID, Filter*> filters;

  // Slaves to send offers for.
  Option<hashset<std::string> > whitelist;

  // Sorter containing all active users.
  UserSorter* userSorter;

  // Map of framework to resource types
  hashmap<FrameworkID, FrameworkResources> frameworks;
};


// Used to represent "filters" for resources unused in offers.
class Filter
{
public:
  virtual ~Filter() {}
  virtual bool filter(const SlaveID& slaveId, const Resources& resources) = 0;
};


class RefusedFilter : public Filter
{
public:
  RefusedFilter(const SlaveID& _slaveId,
                const Resources& _resources,
                const Timeout& _timeout)
    : slaveId(_slaveId),
      resources(_resources),
      timeout(_timeout) {}

  virtual bool filter(const SlaveID& slaveId, const Resources& resources)
  {
    return slaveId == this->slaveId &&
      resources <= this->resources && // Refused resources are superset.
      timeout.remaining() > Seconds(0);
  }

  const SlaveID slaveId;
  const Resources resources;
  const Timeout timeout;
};


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::initialize(
    const Flags& _flags,
    const process::PID<Master>& _master)
{
  flags = _flags;
  master = _master;
  initialized = true;
  userSorter = new UserSorter();

  delay(flags.allocation_interval, self(),
	&HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::batch);
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::frameworkAdded(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo,
    const Resources& used)
{
  CHECK(initialized);

  std::string user  = frameworkInfo.user();
  if (!userSorter->contains(user)) {
    userSorter->add(user);
    sorters[user] = new FrameworkSorter();
  }

  CHECK(!sorters[user]->contains(frameworkId.value()));
  sorters[user]->add(frameworkId.value());

  // Update the allocation to this framework.
  userSorter->allocated(user, used);
  sorters[user]->add(used);
  sorters[user]->allocated(frameworkId.value(), used);

  users[frameworkId] = frameworkInfo.user();

  frameworks[frameworkId] = FrameworkResources();

  LOG(INFO) << "Added framework " << frameworkId;

  allocate();
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::frameworkRemoved(const FrameworkID& frameworkId)
{
  CHECK(initialized);

  std::string user = users[frameworkId];
  // Might not be in 'sorters[user]' because it was previously
  // deactivated and never re-added.
  if (sorters[user]->contains(frameworkId.value())) {
    Resources allocation = sorters[user]->allocation(frameworkId.value());
    userSorter->unallocated(user, allocation);
    sorters[user]->remove(allocation);
    sorters[user]->remove(frameworkId.value());
  }

  users.erase(frameworkId);
  frameworks.erase(frameworkId);

  // If this user doesn't have any more active frameworks, remove it.
  if (sorters[user]->count() == 0) {
    Sorter* s = sorters[user];
    sorters.erase(user);
    delete s;

    userSorter->remove(user);
  }

  foreach (Filter* filter, filters.get(frameworkId)) {
    filters.remove(frameworkId, filter);

    // Do not delete the filter, see comments in
    // HierarchicalAllocatorProcess::offersRevived and
    // HierarchicalAllocatorProcess::expire.
  }

  filters.remove(frameworkId);

  LOG(INFO) << "Removed framework " << frameworkId;
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::frameworkActivated(
    const FrameworkID& frameworkId,
    const FrameworkInfo& frameworkInfo)
{
  CHECK(initialized);

  std::string user = frameworkInfo.user();
  sorters[user]->activate(frameworkId.value());

  LOG(INFO) << "Activated framework " << frameworkId;

  allocate();
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::frameworkDeactivated(const FrameworkID& frameworkId)
{
  CHECK(initialized);

  std::string user = users[frameworkId];
  sorters[user]->deactivate(frameworkId.value());

  // Note that the Sorter *does not* remove the resources allocated
  // to this framework. For now, this is important because if the
  // framework fails over and is activated, we still want a record
  // of the resources that it is using. We might be able to collapse
  // the added/removed and activated/deactivated in the future.

  foreach (Filter* filter, filters.get(frameworkId)) {
    filters.remove(frameworkId, filter);

    // Do not delete the filter, see comments in
    // HierarchicalAllocatorProcess::offersRevived and
    // HierarchicalAllocatorProcess::expire.
  }

  filters.remove(frameworkId);

  LOG(INFO) << "Deactivated framework " << frameworkId;
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::slaveAdded(
    const SlaveID& slaveId,
    const SlaveInfo& slaveInfo,
    const hashmap<FrameworkID, Resources>& used)
{
  CHECK(initialized);

  CHECK(!slaves.contains(slaveId));

  slaves[slaveId] = slaveInfo;

  userSorter->add(slaveInfo.resources());

  Resources unused = slaveInfo.resources();

  foreachpair (const FrameworkID& frameworkId, const Resources& resources, used) {
    if (users.contains(frameworkId)) {
      const std::string& user = users[frameworkId];
      sorters[user]->add(resources);
      sorters[user]->allocated(frameworkId.value(), resources);
      userSorter->allocated(user, resources);
    }

    unused -= resources; // Only want to allocate resources that are not used!
  }

  allocatable[slaveId] = unused;

  LOG(INFO) << "Added slave " << slaveId << " (" << slaveInfo.hostname()
            << ") with " << slaveInfo.resources()
            << " (and " << unused << " available)";

  allocate(slaveId);
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::slaveRemoved(const SlaveID& slaveId)
{
  CHECK(initialized);

  CHECK(slaves.contains(slaveId));

  userSorter->remove(slaves[slaveId].resources());

  slaves.erase(slaveId);

  allocatable.erase(slaveId);

  // Note that we DO NOT actually delete any filters associated with
  // this slave, that will occur when the delayed
  // HierarchicalAllocatorProcess::expire gets invoked (or the framework
  // that applied the filters gets removed).

  LOG(INFO) << "Removed slave " << slaveId;
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::updateWhitelist(
    const Option<hashset<std::string> >& _whitelist)
{
  CHECK(initialized);

  whitelist = _whitelist;

  if (whitelist.isSome()) {
    LOG(INFO) << "Updated slave white list:";
    foreach (const std::string& hostname, whitelist.get()) {
      LOG(INFO) << "\t" << hostname;
    }
  }
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::resourcesRequested(
    const FrameworkID& frameworkId,
    const std::vector<Request>& requests)
{
  CHECK(initialized);
  CHECK(frameworks.contains(frameworkId));

  // update guaranteed resources
  foreach (const Request& request, requests) {
    frameworks[frameworkId].guaranteedRes = request.resources();
    break;
  }

  LOG(INFO) << "Received resource request from framework " << frameworkId << ", guarantee is now " << frameworks[frameworkId].guaranteedRes;
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::resourcesUnused(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resourcesUnused,
    const Resources& resourcesUsed,
    const Option<Filters>& filters)
{
  CHECK(initialized);

  // Updated resources used
  CHECK(frameworks.contains(frameworkId));
  frameworks[frameworkId].activeRes += resourcesUsed;

  if (resourcesUnused.allocatable().size() == 0) {
    return;
  }

  VLOG(1) << "Framework " << frameworkId
          << " left " << resourcesUnused.allocatable()
          << " unused on slave " << slaveId;

  // Update resources allocated to framework. It is
  // not possible for the user to not be in users
  // because resourcesUnused is only called as the
  // result of a valid task launch by an active
  // framework that doesn't use the entire offer.
  CHECK(users.contains(frameworkId));

  std::string user = users[frameworkId];
  sorters[user]->unallocated(frameworkId.value(), resourcesUnused);
  sorters[user]->remove(resourcesUnused);
  userSorter->unallocated(user, resourcesUnused);

  // Update resources allocatable on slave.
  CHECK(allocatable.contains(slaveId));
  allocatable[slaveId] += resourcesUnused;

  // Create a refused resources filter.
  Seconds seconds(filters.isSome()
                  ? filters.get().refuse_seconds()
                  : Filters().refuse_seconds());

  if (seconds != Seconds(0)) {
    LOG(INFO) << "Framework " << frameworkId
	      << " filtered slave " << slaveId
	      << " for " << seconds;

    // Create a new filter and delay it's expiration.
    mesos::internal::master::Filter* filter =
      new RefusedFilter(slaveId, resourcesUnused, Timeout(seconds));

    this->filters.put(frameworkId, filter);

    delay(seconds, self(),
          &HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::expire,
          frameworkId,
          filter);
  }
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::resourcesRecovered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources)
{
  CHECK(initialized);

  if (resources.allocatable().size() == 0) {
    return;
  }

  // Updated resources allocated to framework (if framework still
  // exists, which it might not in the event that we dispatched
  // Master::offer before we received AllocatorProcess::frameworkRemoved
  // or AllocatorProcess::frameworkDeactivated, in which case we will
  // have already recovered all of its resources).
  if (users.contains(frameworkId) &&
      sorters[users[frameworkId]]->contains(frameworkId.value())) {
    std::string user = users[frameworkId];
    sorters[user]->unallocated(frameworkId.value(), resources);
    sorters[user]->remove(resources);
    userSorter->unallocated(user, resources);

    CHECK(frameworks.contains(frameworkId));
    frameworks[frameworkId].activeRes -= resources;
    frameworks[frameworkId].revokedRes = Resources();
  }

  // Update resources allocatable on slave (if slave still exists,
  // which it might not in the event that we dispatched Master::offer
  // before we received Allocator::slaveRemoved).
  if (allocatable.contains(slaveId)) {
    allocatable[slaveId] += resources;

    VLOG(1) << "Recovered " << resources.allocatable()
            << " on slave " << slaveId
            << " from framework " << frameworkId;

  }
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::offersRevived(const FrameworkID& frameworkId)
{
  CHECK(initialized);

  foreach (Filter* filter, filters.get(frameworkId)) {
    filters.remove(frameworkId, filter);

    // We delete each actual Filter when
    // HierarchicalAllocatorProcess::expire gets invoked. If we delete the
    // Filter here it's possible that the same Filter (i.e., same
    // address) could get reused and HierarchicalAllocatorProcess::expire
    // would expire that filter too soon. Note that this only works
    // right now because ALL Filter types "expire".
  }

  filters.remove(frameworkId);

  LOG(INFO) << "Removed filters for framework " << frameworkId;

  allocate();
}

template <class UserSorter, class FrameworkSorter>
hashmap<SlaveID, Resources> HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::filterAllocatable(const hashset<SlaveID>& slaveIds, bool writeLog)
{
  // Get out only "available" resources (i.e., resources that are
  // allocatable and above a certain threshold, see below).
  hashmap<SlaveID, Resources> available;
  foreachpair (const SlaveID& slaveId, Resources resources, allocatable) {
    if (!slaveIds.contains(slaveId)) {
      continue;
    }

    if (isWhitelisted(slaveId)) {
      resources = resources.allocatable(); // Make sure they're allocatable.

      // TODO(benh): For now, only make offers when there is some cpu
      // and memory left. This is an artifact of the original code that
      // only offered when there was at least 1 cpu "unit" available,
      // and without doing this a framework might get offered resources
      // with only memory available (which it obviously will decline)
      // and then end up waiting the default Filters::refuse_seconds
      // (unless the framework set it to something different).

      Value::Scalar none;
      Value::Scalar cpus = resources.get("cpus", none);
      Value::Scalar mem = resources.get("mem", none);

      if (cpus.value() >= MIN_CPUS && mem.value() > MIN_MEM) {
        if(writeLog) {
	  VLOG(1) << "Found available resources: " << resources
	  	  << " on slave " << slaveId;
        }
	available[slaveId] = resources;
      }
    }
  }

  return available;
}

template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::batch()
{
  CHECK(initialized);
  checkUtilization();
  allocate();
  delay(flags.allocation_interval, self(),
	&HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::batch);
}

template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::checkUtilization()
{

  
  Resources available;
  foreachvalue (const SlaveInfo& slaveInfo, slaves) {
    available += Resources(slaveInfo.resources());
  }
  LOG(INFO) << "SYSTEM TOTAL: " << available;

  // Find needed resources to fullfill unsatisfied guarantees
  Resources needed;
  foreachvalue (const FrameworkResources& framework, frameworks) {
    available = available - framework.activeRes + framework.revokedRes;
    foreach(const Resource& guarantee, framework.guaranteedRes) {
      Option<Resource> currentUse = framework.activeRes.get(guarantee);
      
      if(currentUse.isNone()) {
        needed = needed + guarantee;
      } else if(currentUse.get() <= guarantee) {
        needed = needed + (guarantee - currentUse.get());
      }
    }
  }

  LOG(INFO) << "Available: " << available << " , Needed: " << needed;

  needed = needed - available;
  LOG(INFO) << "Revocation target: " << needed;
  if (!needed.hasPositive()) return;

  // find someone over their guarantee
  // make available + revoke == needed to match guarantee
  hashmap<FrameworkID, Resources> revokeMap;
  foreachpair (const FrameworkID& frameworkId, const FrameworkResources& framework, frameworks) {
    Resources excess = framework.activeRes - framework.guaranteedRes - framework.revokedRes;
    Resources toBeRevoked;
    
    // figure out which resources we are using in excess that actually are needed
    foreach(const Resource& currentNeed, needed) {
      Option<Resource> currentExcess = excess.get(currentNeed);
      // Only continue revocation if actually in excess and the resource is scalar
      // TODO: handle non-scalar revocations
      if(currentExcess.isNone() || !currentExcess.get().has_scalar()) continue;
      if (currentExcess.get().scalar().value() > 0) {
        if(currentNeed <= currentExcess.get()) toBeRevoked += currentNeed;
        else toBeRevoked += currentExcess.get();
      }
    }
    // assign framework to be revoked, update how many resources still need revoked
    if (toBeRevoked.hasPositive()) {
      revokeMap[frameworkId] = toBeRevoked;
      needed = needed - toBeRevoked;
      if (!needed.hasPositive()) break;
    }
  }
  // Actually send the revocation commands to master
  foreachpair (const FrameworkID& frameworkId, const Resources& resources, revokeMap) {
    frameworks[frameworkId].revokedRes += resources;
    dispatch(master, &Master::revokeFramework, frameworkId, resources);
  }

}

template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::allocate()
{
  CHECK(initialized);

  Stopwatch stopwatch;
  stopwatch.start();

  allocate(slaves.keys());

  LOG(INFO) << "Performed allocation for " << slaves.size()
            << " slaves in " << stopwatch.elapsed();
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::allocate(const SlaveID& slaveId)
{
  CHECK(initialized);

  hashset<SlaveID> slaveIds;
  slaveIds.insert(slaveId);

  Stopwatch stopwatch;
  stopwatch.start();

  allocate(slaveIds);

  LOG(INFO) << "Performed allocation for slave " << slaveId
            << " in " << stopwatch.elapsed();
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::allocate(const hashset<SlaveID>& slaveIds)
{
  CHECK(initialized);

  if (userSorter->count() == 0) {
    VLOG(1) << "No users to allocate resources!";
    return;
  }

  hashmap<SlaveID, Resources> available = filterAllocatable(slaveIds, true);

  if (available.size() == 0) {
    VLOG(1) << "No resources available to allocate!";
    return;
  }

  foreach (const std::string& user, userSorter->sort()) {
    foreach (const std::string& frameworkIdValue, sorters[user]->sort()) {
      FrameworkID frameworkId;
      frameworkId.set_value(frameworkIdValue);

      Resources allocatedResources;
      hashmap<SlaveID, Resources> offerable;
      foreachpair (const SlaveID& slaveId, const Resources& resources, available) {
	// Check whether or not this framework filters this slave.
	bool filtered = isFiltered(frameworkId, slaveId, resources);

	if (!filtered) {
	  VLOG(1) << "Offering " << resources
		  << " on slave " << slaveId
		  << " to framework " << frameworkId;
	  offerable[slaveId] = resources;

	  // Update framework and slave resources.
	  allocatable[slaveId] -= resources;
	  allocatedResources += resources;
	}
      }

      if (offerable.size() > 0) {
	foreachkey (const SlaveID& slaveId, offerable) {
	  available.erase(slaveId);
	}

	sorters[user]->add(allocatedResources);
	sorters[user]->allocated(frameworkIdValue, allocatedResources);
	userSorter->allocated(user, allocatedResources);

	dispatch(master, &Master::offer, frameworkId, offerable);
      }
    }
  }
}


template <class UserSorter, class FrameworkSorter>
void HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::expire(
    const FrameworkID& frameworkId,
    Filter* filter)
{
  // The filter might have already been removed (e.g., if the
  // framework no longer exists or in
  // HierarchicalAllocatorProcess::offersRevived) but not yet deleted (to
  // keep the address from getting reused possibly causing premature
  // expiration).
  if (users.contains(frameworkId) &&
      filters.contains(frameworkId, filter)) {
    filters.remove(frameworkId, filter);
  }

  delete filter;
}


template <class UserSorter, class FrameworkSorter>
bool HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::isWhitelisted(
    const SlaveID& slaveId)
{
  CHECK(initialized);

  CHECK(slaves.contains(slaveId));

  return whitelist.isNone() ||
    whitelist.get().contains(slaves[slaveId].hostname());
}


template <class UserSorter, class FrameworkSorter>
bool HierarchicalAllocatorProcess<UserSorter, FrameworkSorter>::isFiltered(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const Resources& resources)
{
  bool filtered = false;
  foreach (Filter* filter, filters.get(frameworkId)) {
    if (filter->filter(slaveId, resources)) {
      VLOG(1) << "Filtered " << resources
	      << " on slave " << slaveId
	      << " for framework " << frameworkId;
      filtered = true;
      break;
    }
  }
  return filtered;
}

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __HIERARCHICAL_ALLOCATOR_PROCESS_HPP__
