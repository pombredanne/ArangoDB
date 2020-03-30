////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_VOCBASE_IDENTIFIERS_FILE_ID_H
#define ARANGOD_VOCBASE_IDENTIFIERS_FILE_ID_H 1

#include "Basics/Identifier.h"

namespace arangodb {

/// @brief server id type
class FileId : public arangodb::basics::Identifier {
 public:
  constexpr FileId() noexcept : Identifier() {}
  constexpr explicit FileId(BaseType id) noexcept : Identifier(id) {}

 public:
  /// @brief create a not-set file id
  static constexpr FileId none() { return FileId{0}; }
};

static_assert(sizeof(FileId) == sizeof(FileId::BaseType),
              "invalid size of FileId");
}  // namespace arangodb

DECLARE_HASH_FOR_IDENTIFIER(arangodb::FileId)

#endif
