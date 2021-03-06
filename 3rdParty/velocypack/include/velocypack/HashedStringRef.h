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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef VELOCYPACK_HASHED_STRINGREF_H
#define VELOCYPACK_HASHED_STRINGREF_H 1

#include <cstring>
#include <functional>
#include <algorithm>
#include <string>
#include <iosfwd>

#include "velocypack/velocypack-common.h"

namespace arangodb {
namespace velocypack {
class Slice;
class StringRef;

class HashedStringRef {
 public:
  /// @brief create an empty HashedStringRef
  HashedStringRef() noexcept 
    : _data(""), 
      _length(0), 
      _hash(hash("", 0)) {}

  /// @brief create a HashedStringRef from a string plus length
  HashedStringRef(char const* data, uint32_t length) noexcept 
    : _data(data), 
      _length(length),
      _hash(hash(_data, _length)) {}
 
  /// @brief create a HashedStringRef from a string plus length and hash.
  /// note: the hash is *not* validated. it is the caller's responsibility
  /// to ensure the hash value is correct
  HashedStringRef(char const* data, uint32_t length, uint32_t hash) noexcept 
    : _data(data), 
      _length(length), 
      _hash(hash) {}
  
  /// @brief create a HashedStringRef from a VPack slice (must be of type String)
  explicit HashedStringRef(Slice slice);
  
  /// @brief create a HashedStringRef from another HashedStringRef
  constexpr HashedStringRef(HashedStringRef const& other) noexcept
      : _data(other._data), 
        _length(other._length), 
        _hash(other._hash) {}
  
  /// @brief move a HashedStringRef from another HashedStringRef
  constexpr HashedStringRef(HashedStringRef&& other) noexcept
      : _data(other._data), 
        _length(other._length), 
        _hash(other._hash) {}
  
  /// @brief create a HashedStringRef from another HashedStringRef
  HashedStringRef& operator=(HashedStringRef const& other) noexcept {
    _data = other._data;
    _length = other._length;
    _hash = other._hash;
    return *this;
  }
  
  /// @brief move a HashedStringRef from another HashedStringRef
  HashedStringRef& operator=(HashedStringRef&& other) noexcept {
    _data = other._data;
    _length = other._length;
    _hash = other._hash;
    return *this;
  }
  
  /// @brief create a HashedStringRef from a VPack slice of type String
  HashedStringRef& operator=(Slice slice);
  
  HashedStringRef substr(std::size_t pos = 0, std::size_t count = std::string::npos) const;
  
  char at(std::size_t index) const;
  
  std::size_t find(char c, std::size_t offset = 0) const noexcept;
  
  std::size_t rfind(char c, std::size_t offset = std::string::npos) const noexcept;

  int compare(HashedStringRef const& other) const noexcept;
  
  template<typename OtherType>
  int compare(OtherType const& other) const noexcept { return compare(HashedStringRef(other)); }
  
  bool equals(HashedStringRef const& other) const noexcept;
  
  template<typename OtherType>
  bool equals(OtherType const& other) const noexcept { return equals(HashedStringRef(other)); }
  
  std::string toString() const {
    return std::string(_data, _length);
  }

  StringRef stringRef() const noexcept;

  constexpr inline bool empty() const noexcept {
    return (_length == 0);
  }
 
  inline char const* begin() const noexcept {
    return _data;
  }
  
  inline char const* cbegin() const noexcept {
    return _data;
  }
 
  inline char const* end() const noexcept {
    return _data + _length;
  }
  
  inline char const* cend() const noexcept {
    return _data + _length;
  }

  inline char front() const noexcept { return _data[0]; }

  inline char back() const noexcept { return _data[_length - 1]; }

  inline void pop_back() { --_length; }
  
  inline char operator[](std::size_t index) const noexcept { 
    return _data[index];
  }
  
  constexpr inline char const* data() const noexcept {
    return _data;
  }

  constexpr inline std::size_t size() const noexcept {
    return _length;
  }

  constexpr inline std::size_t length() const noexcept {
    return _length;
  }
  
  constexpr inline uint32_t hash() const noexcept {
    return _hash;
  }

 private:
  inline uint32_t hash(char const* data, uint32_t length) const noexcept {
    return VELOCYPACK_HASH32(data, length, 0xdeadbeef);
  }

 private:
  char const* _data;
  uint32_t _length;
  uint32_t _hash;
};

std::ostream& operator<<(std::ostream& stream, HashedStringRef const& ref);
} // namespace velocypack
} // namespace arangodb

inline bool operator==(arangodb::velocypack::HashedStringRef const& lhs, arangodb::velocypack::HashedStringRef const& rhs) noexcept {
  return (lhs.size() == rhs.size() && 
          lhs.hash() == rhs.hash() && 
          memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
}

inline bool operator!=(arangodb::velocypack::HashedStringRef const& lhs, arangodb::velocypack::HashedStringRef const& rhs) noexcept {
  return !(lhs == rhs);
}

inline bool operator==(arangodb::velocypack::HashedStringRef const& lhs, std::string const& rhs) noexcept {
  return (lhs.size() == rhs.size() && 
          memcmp(lhs.data(), rhs.c_str(), lhs.size()) == 0);
}

inline bool operator!=(arangodb::velocypack::HashedStringRef const& lhs, std::string const& rhs) noexcept {
  return !(lhs == rhs);
}

inline bool operator<(arangodb::velocypack::HashedStringRef const& lhs, arangodb::velocypack::HashedStringRef const& rhs) noexcept {
  return (lhs.compare(rhs) < 0);
}

inline bool operator>(arangodb::velocypack::HashedStringRef const& lhs, arangodb::velocypack::HashedStringRef const& rhs) noexcept {
  return (lhs.compare(rhs) > 0);
}

namespace std {

template <>
struct hash<arangodb::velocypack::HashedStringRef> {
  std::size_t operator()(arangodb::velocypack::HashedStringRef const& value) const noexcept {
    return static_cast<std::size_t>(value.hash());
  }
};

template <>
struct equal_to<arangodb::velocypack::HashedStringRef> {
  bool operator()(arangodb::velocypack::HashedStringRef const& lhs,
                  arangodb::velocypack::HashedStringRef const& rhs) const noexcept {
    return (lhs.size() == rhs.size() &&
            lhs.hash() == rhs.hash() &&
            (memcmp(lhs.data(), rhs.data(), lhs.size()) == 0));
  }
};

}

#endif
