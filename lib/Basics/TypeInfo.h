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
/// @author Andrey Abramov
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BASICS_TYPEINFO_H
#define ARANGODB_BASICS_TYPEINFO_H 1

#include <string_view>

namespace arangodb {

////////////////////////////////////////////////////////////////////////////////
/// @class TypeInfo
/// @brief holds meta information about a type, e.g. name and identifier
////////////////////////////////////////////////////////////////////////////////
class TypeInfo {
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief unique type identifier
  /// @note can be used to get an instance of underlying type
  //////////////////////////////////////////////////////////////////////////////
  using TypeId = TypeInfo(*)() noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief default constructor produces invalid type identifier
  //////////////////////////////////////////////////////////////////////////////
  constexpr TypeInfo() noexcept
    : TypeInfo(nullptr, "") {
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @return true if type_info is valid, false - otherwise
  //////////////////////////////////////////////////////////////////////////////
  constexpr explicit operator bool() const noexcept {
    return nullptr != _id;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @return true if current object is equal to a denoted by 'rhs'
  //////////////////////////////////////////////////////////////////////////////
  constexpr bool operator==(const TypeInfo& rhs) const noexcept {
    return _id == rhs._id;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @return true if current object is not equal to a denoted by 'rhs'
  //////////////////////////////////////////////////////////////////////////////
  constexpr bool operator!=(const TypeInfo& rhs) const noexcept {
    return !(*this == rhs);
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @return true if current object is less than to a denoted by 'rhs'
  //////////////////////////////////////////////////////////////////////////////
  constexpr bool operator<(const TypeInfo& rhs) const noexcept {
    return _name < rhs._name;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @return type identifier
  //////////////////////////////////////////////////////////////////////////////
  constexpr TypeId id() const noexcept { return _id; }

  //////////////////////////////////////////////////////////////////////////////
  /// @return type name
  //////////////////////////////////////////////////////////////////////////////
  constexpr std::string_view name() const noexcept { return _name; }

 private:
  template<typename T>
  friend struct Type;

  constexpr explicit TypeInfo(
    TypeId id, std::string_view name) noexcept
    : _id(id), _name(name) {
  }

  TypeId _id;
  std::string_view _name;
}; // TypeInfo

////////////////////////////////////////////////////////////////////////////////
/// @class Type
/// @tparam T type for which one needs access meta information
/// @brief convenient helper for accessing meta information
////////////////////////////////////////////////////////////////////////////////
template<typename T>
struct Type {
  //////////////////////////////////////////////////////////////////////////////
  /// @returns an instance of "type_info" object holding meta information of
  ///          type denoted by template parameter "T"
  //////////////////////////////////////////////////////////////////////////////
  static constexpr TypeInfo get() noexcept {
    return TypeInfo{id(), ""};
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @returns type identifier of a type denoted by template parameter "T"
  //////////////////////////////////////////////////////////////////////////////
  static constexpr TypeInfo::TypeId id() noexcept {
    return &get;
  }
}; // Type

} // arangodb

#endif // ARANGODB_BASICS_TYPEINFO_H
