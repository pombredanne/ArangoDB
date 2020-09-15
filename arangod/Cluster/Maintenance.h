////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2018 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_MAINTENANCE_MAINTENANCE_H
#define ARANGODB_MAINTENANCE_MAINTENANCE_H

#include "Agency/Node.h"
#include "Basics/Result.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/MaintenanceFeature.h"

namespace arangodb {

class LogicalCollection;

namespace maintenance {

// A few constants for priorities used in Maintenance for ActionDescriptions:

// For fast track:
constexpr int NORMAL_PRIORITY = 1;
constexpr int FOLLOWER_PRIORITY = 1;
constexpr int LEADER_PRIORITY = 2;
constexpr int HIGHER_PRIORITY = 2;
constexpr int RESIGN_PRIORITY = 3;

// For non fast track:
constexpr int INDEX_PRIORITY = 2;
constexpr int SYNCHRONIZE_PRIORITY = 1;

using Transactions = std::vector<std::pair<VPackBuilder, VPackBuilder>>;

arangodb::Result diffPlanLocalForDatabases(VPackSlice const&,
                                           std::vector<std::string> const&,
                                           std::vector<std::string>&,
                                           std::vector<std::string>&);

/**
 * @brief          Difference Plan and local for phase 1 of Maintenance run
 *
 * @param plan     Snapshot of agency's planned state
 * @param planIndex Raft index of this plan version
 * @param local    Snapshot of local state
 * @param serverId This server's UUID
 * @param errors   Copy of last maintenance feature errors
 * @param feature   The feature itself
 * @param actions  Resulting actions from difference are packed in here
 *
 * @return         Result
 */
arangodb::Result diffPlanLocal(VPackSlice const& plan, uint64_t planIndex,
                               VPackSlice const& local,
                               std::string const& serverId,
                               MaintenanceFeature::errors_t& errors,
                               MaintenanceFeature& feature,
                               std::vector<std::shared_ptr<ActionDescription>>& actions,
                               MaintenanceFeature::ShardActionMap const& shardActionMap);

/**
 * @brief          Difference Plan and local for phase 1 of Maintenance run
 *
 * @param plan     Snapshot of agency's planned state
 * @param planIndex Raft index of this plan version
 * @param current  Snapshot of agency's current state
 * @param local    Snapshot of local state
 * @param serverId This server's UUID
 * @param feature  Maintenance feature
 * @param report   Report
 *
 * @return         Result
 */
arangodb::Result executePlan(VPackSlice const& plan, uint64_t planIndex, VPackSlice const& local,
                             std::string const& serverId,
                             arangodb::MaintenanceFeature& feature, VPackBuilder& report,
                             arangodb::MaintenanceFeature::ShardActionMap const& shardActionMap);

/**
 * @brief          Difference local and current states for phase 2 of
 * Maintenance
 *
 * @param local    Snapshot of local state
 * @param current  Snapshot of agency's current state
 * @param serverId This server's UUID
 * @param report   Resulting agency transaction, which is to be sent
 *
 * @return         Result
 */
arangodb::Result diffLocalCurrent(VPackSlice const& local, VPackSlice const& current,
                                  std::string const& serverId, Transactions& report,
                                  MaintenanceFeature::ShardActionMap const& shardActionMap);

/**
 * @brief          Phase one: Execute plan, shard replication startups
 *
 * @param plan     Snapshot of agency's planned state
 * @param planIndex Raft index of this version of the plan
 * @param current  Snapshot of agency's current state
 * @param local    Snapshot of local state
 * @param serverId This server's UUID
 * @param feature  Maintenance feature
 * @param report   Resulting agency transaction, which is to be sent
 *
 * @return         Result
 */
arangodb::Result phaseOne(VPackSlice const& plan, uint64_t planIndex, VPackSlice const& local,
                          std::string const& serverId,
                          MaintenanceFeature& feature, VPackBuilder& report,
                          MaintenanceFeature::ShardActionMap const& shardActionMap);

/**
 * @brief          Phase two: Report in agency
 *
 * @param plan     Snapshot of agency's planned state
 * @param current  Snapshot of agency's current state
 * @param local    Snapshot of local state
 * @param serverId This server's UUID
 * @param feature  Maintenance feature
 * @param report   Report on what we did
 *
 * @return         Result
 */
arangodb::Result phaseTwo(VPackSlice const& plan, VPackSlice const& cur,
                          VPackSlice const& local, std::string const& serverId,
                          MaintenanceFeature& feature, VPackBuilder& report,
                          MaintenanceFeature::ShardActionMap const& shardActionMap);

/**
 * @brief          Report local changes to current
 *
 * @param plan     Snapshot of agency's planned state
 * @param current  Snapshot of agency's current state
 * @param local    Snapshot of local state
 * @param serverId This server's UUID
 * @param report   Report on what we did
 *
 * @return         Result
 */
struct ShardStatistics {
  uint64_t numShards;
  uint64_t numLeaderShards;
  uint64_t numOutOfSyncShards;
  uint64_t numNotReplicated;
};

arangodb::Result reportInCurrent(VPackSlice const& plan, VPackSlice const& cur,
                                 VPackSlice const& local,
                                 MaintenanceFeature::errors_t const& allErrors,
                                 std::string const& serverId, VPackBuilder& report,
                                 ShardStatistics& shardStats);

/**
 * @brief            Schedule synchroneous replications
 *
 * @param  plan      Plan's snapshot
 * @param  current   Current's scnapshot
 * @param  local     Local snapshot
 * @param  serverId  My server's uuid
 * @param  feature   Maintenance feature
 *
 * @return           Success story (always ok)
 */
arangodb::Result syncReplicatedShardsWithLeaders(
    VPackSlice const& plan, VPackSlice const& current, VPackSlice const& local,
    std::string const& serverId, MaintenanceFeature& feature,
    MaintenanceFeature::ShardActionMap const& shardActionMap);

}  // namespace maintenance
}  // namespace arangodb

#endif
