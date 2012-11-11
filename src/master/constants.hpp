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

#ifndef __MASTER_CONSTANTS_HPP__
#define __MASTER_CONSTANTS_HPP__

#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace master {

// TODO(benh): Add units after constants.
// TODO(benh): Also make configuration options be constants.

// Maximum number of slot offers to have outstanding for each framework.
const int MAX_OFFERS_PER_FRAMEWORK = 50;

// Minimum number of cpus / task.
const uint32_t MIN_CPUS = 1;

// Minimum amount of memory / task.
const uint32_t MIN_MEM = 32 * Megabyte;

// Maximum number of CPUs per machine.
const uint32_t MAX_CPUS = 1000 * 1000;

// Maximum amount of memory / machine.
const uint32_t MAX_MEM = 1024 * 1024 * Megabyte;

// Amount of time within which a slave PING should be received.
const Duration SLAVE_PING_TIMEOUT = Seconds(15.0);

// Maximum number of ping timeouts until slave is considered failed.
const uint32_t MAX_SLAVE_PING_TIMEOUTS = 5;

// Maximum number of completed frameworks to store in the cache.
// TODO(thomasm): Make configurable.
const uint32_t MAX_COMPLETED_FRAMEWORKS = 50;

// Maximum number of completed tasks per framework to store in the
// cache.  TODO(thomasm): Make configurable.
const uint32_t MAX_COMPLETED_TASKS_PER_FRAMEWORK = 1000;

// Time interval to check for updated watchers list.
const Duration WHITELIST_WATCH_INTERVAL = Seconds(5.0);
const Duration INITIAL_OFFER_TIMEOUT = Milliseconds(500.0);

} // namespace mesos {
} // namespace internal {
} // namespace master {

#endif // __MASTER_CONSTANTS_HPP__
