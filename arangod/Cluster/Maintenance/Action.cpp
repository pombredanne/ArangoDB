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

#include "Cluster/Maintenance/Action.h"

#include "Cluster/Maintenance/Actions/CreateCollection.h"
#include "Cluster/Maintenance/Actions/CreateDatabase.h"
#include "Cluster/Maintenance/Actions/DropCollection.h"
#include "Cluster/Maintenance/Actions/DropDatabase.h"
#include "Cluster/Maintenance/Actions/DropIndex.h"
#include "Cluster/Maintenance/Actions/EnsureIndex.h"
#include "Cluster/Maintenance/Actions/NonAction.h"
#include "Cluster/Maintenance/Actions/ResignShardLeadership.h"
#include "Cluster/Maintenance/Actions/SynchronizeShard.h"
#include "Cluster/Maintenance/Actions/TakeoverShardLeadership.h"
#include "Cluster/Maintenance/Actions/UpdateCollection.h"
#include "Cluster/Maintenance/Actions/UpgradeCollection.h"
#include "Cluster/Maintenance/MaintenanceStrings.h"

#include "Logger/Logger.h"

using namespace arangodb;
using namespace arangodb::maintenance;

using factories_t =
    std::unordered_map<std::string, std::function<std::shared_ptr<ActionBase>(MaintenanceFeature&, ActionDescription const&)>>;

static factories_t const factories = factories_t{

    {CREATE_COLLECTION,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<CreateCollection>(f, a);
     }},

    {CREATE_DATABASE,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<CreateDatabase>(f, a);
     }},

    {DROP_COLLECTION,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<DropCollection>(f, a);
     }},

    {DROP_DATABASE,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<DropDatabase>(f, a);
     }},

    {DROP_INDEX,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<DropIndex>(f, a);
     }},

    {ENSURE_INDEX,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<EnsureIndex>(f, a);
     }},

    {RESIGN_SHARD_LEADERSHIP,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<ResignShardLeadership>(f, a);
     }},

    {SYNCHRONIZE_SHARD,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<SynchronizeShard>(f, a);
     }},

    {UPGRADE_COLLECTION,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<UpgradeCollection>(f, a);
     }},

    {UPDATE_COLLECTION,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<UpdateCollection>(f, a);
     }},

    {TAKEOVER_SHARD_LEADERSHIP,
     [](MaintenanceFeature& f, ActionDescription const& a) {
       return std::make_shared<TakeoverShardLeadership>(f, a);
     }},

};

Action::Action(MaintenanceFeature& feature, ActionDescription const& description)
    : _action(nullptr) {
  TRI_ASSERT(description.has(NAME));
  create(feature, description);
}

Action::Action(MaintenanceFeature& feature, ActionDescription&& description)
    : _action(nullptr) {
  TRI_ASSERT(description.has(NAME));
  create(feature, std::move(description));
}

Action::Action(MaintenanceFeature& feature, std::shared_ptr<ActionDescription> const& description)
    : _action(nullptr) {
  TRI_ASSERT(description->has(NAME));
  create(feature, *description);
}

Action::Action(std::shared_ptr<ActionBase> action)
    : _action(std::move(action)) {}

Action::~Action() = default;

void Action::create(MaintenanceFeature& feature, ActionDescription const& description) {
  auto factory = factories.find(description.name());

  _action = (factory != factories.end())
                ? factory->second(feature, description)
                : std::make_shared<NonAction>(feature, description);
}

ActionDescription const& Action::describe() const {
  TRI_ASSERT(_action != nullptr);
  return _action->describe();
}

arangodb::MaintenanceFeature& Action::feature() const {
  TRI_ASSERT(_action != nullptr);
  return _action->feature();
}

std::shared_ptr<VPackBuilder> const Action::properties() const {
  return describe().properties();
}

bool Action::first() {
  TRI_ASSERT(_action != nullptr);
  return _action->first();
}

bool Action::next() {
  TRI_ASSERT(_action != nullptr);
  return _action->next();
}

bool Action::matches(std::unordered_set<std::string> const& labels) const {
  TRI_ASSERT(_action != nullptr);
  return _action->matches(labels);
}

arangodb::Result Action::result() {
  TRI_ASSERT(_action != nullptr);
  return _action->result();
}

arangodb::Result Action::kill(Signal const& signal) {
  TRI_ASSERT(_action != nullptr);
  return _action->kill(signal);
}

arangodb::Result Action::progress(double& p) {
  TRI_ASSERT(_action != nullptr);
  return _action->progress(p);
}

ActionState Action::getState() const { return _action->getState(); }

void Action::startStats() { _action->startStats(); }

void Action::incStats() { _action->incStats(); }

void Action::endStats() { _action->endStats(); }

void Action::toVelocyPack(arangodb::velocypack::Builder& builder) const {
  TRI_ASSERT(_action != nullptr);
  _action->toVelocyPack(builder);
}

bool Action::operator<(Action const& other) const {
  // This is to sort actions in a priority queue, therefore, the higher, the
  // higher the priority. FastTrack is always higher, priority counts then,
  // and finally creation time (earlier is higher):
  if (!fastTrack() && other.fastTrack()) {
    return true;
  }
  if (fastTrack() && !other.fastTrack()) {
    return false;
  }
  if (priority() < other.priority()) {
    return true;
  }
  if (priority() > other.priority()) {
    return false;
  }
  if (getCreateTime() > other.getCreateTime()) {
    // Intentional inversion! smaller time is higher priority.
    return true;
  }
  return false;
}

namespace std {
ostream& operator<<(ostream& out, arangodb::maintenance::Action const& d) {
  out << d.toVelocyPack().toJson();
  return out;
}
}  // namespace std
