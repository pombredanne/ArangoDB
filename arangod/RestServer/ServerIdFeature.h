////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_REST_SERVER_SERVER_ID_FEATURE_H
#define ARANGODB_REST_SERVER_SERVER_ID_FEATURE_H 1

#include "ApplicationFeatures/ApplicationFeature.h"
#include "Basics/debugging.h"
#include "ProgramOptions/ProgramOptions.h"
#include "VocBase/Identifiers/ServerId.h"

namespace arangodb {

class ServerIdFeature final : public application_features::ApplicationFeature {
 public:
  explicit ServerIdFeature(application_features::ApplicationServer& server);

  void start() override final;

  static ServerId getId() {
    TRI_ASSERT(SERVERID.isSet());
    return SERVERID;
  }

  // fake the server id from the outside. used for testing only
  static void setId(ServerId serverId) { SERVERID = serverId; }

 private:
  /// @brief generates a new server id
  void generateId();

  /// @brief reads server id from file
  int readId();

  /// @brief writes server id to file
  int writeId();

  /// @brief read / create the server id on startup
  int determineId(bool checkVersion);

  std::string _idFilename;

  static ServerId SERVERID;
};

}  // namespace arangodb

#endif