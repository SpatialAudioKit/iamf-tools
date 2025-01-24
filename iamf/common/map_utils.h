/*
 * Copyright (c) 2022, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear License
 * and the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear
 * License was not distributed with this source code in the LICENSE file, you
 * can obtain it at www.aomedia.org/license/software-license/bsd-3-c-c. If the
 * Alliance for Open Media Patent License 1.0 was not distributed with this
 * source code in the PATENTS file, you can obtain it at
 * www.aomedia.org/license/patent.
 */
#ifndef COMMON_MAP_UTILS_H_
#define COMMON_MAP_UTILS_H_

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace iamf_tools {

/*!\brief Looks up a key in a map and returns a status or value.
 *
 * When lookup fails the error message will contain the `context` string
 * followed by "= $KEY", where $KEY is the stringified `key`.
 *
 * Some mapping have sufficient context in the typenames, for example:
 *   - Input Map: A map of `PersonName` to `Birthday`.
 *   - Typename-based context: "`Birthday` for `PersonName`".
 *   - Output message: "`Birthday` for `PersonName`= John was not found in the
 *                       map.".
 *
 * Some mappings provide insufficient context in the typenames. Or the typenames
 * would be easily confused. Variable names or phrases should be used as
 * context:
 *   - Input Map: A map of `absl::string_view` names to `int` ages.
 *   - Variable-based context: "`age` for `name`".
 *   - Phrase-based context: or "Age for name".
 * Or:
 *   - Input Map: A map of `proto::Type` to `iamf_tools::Type`.
 *   - Phrase-based context: "Internal version of proto `Type`".
 *
 * \param map Map to search.
 * \param key Key to search for.
 * \param context Context to insert into the error message for debugging
 *        purposes.
 * \return Associated value if lookup is successful. `absl::NotFoundError()`
 *         when lookup fails.
 */
template <typename T, typename U>
absl::StatusOr<U> LookupInMap(const absl::flat_hash_map<T, U>& map,
                              const T& key, absl::string_view context) {
  auto iter = map.find(key);
  if (iter != map.end()) [[likely]] {
    return iter->second;
  }

  return absl::NotFoundError(absl::StrCat(
      context, "= ", key, " was not found in the map.",
      map.empty() ? " The map is empty. Did initialization fail?" : ""));
}

/*!\brief Looks up a key in a map and copies the value to the output argument.
 *
 * \param map Map to search.
 * \param key Key to search for.
 * \param context Context to insert into the error message for debugging
 *        purposes. Forwared to `LookupInMap` which has detailed documentation
 *        on usage.
 * \param value Output argument to write the value to.
 * \return `absl::OkStatus()` if lookup is successful. `absl::NotFoundError()`
 *         when lookup fails.
 */
template <typename T, typename U>
absl::Status CopyFromMap(const absl::flat_hash_map<T, U>& map, const T& key,
                         absl::string_view context, U& value) {
  const auto& result = LookupInMap(map, key, context);
  if (result.ok()) [[likely]] {
    value = *result;
  }
  return result.status();
}

/*!\brief Returns a map for static storage from a container of pairs.
 *
 * \param pairs Container of pairs to convert to a map. The first value must be
 *              unique among all pairs.
 * \return Map suitable for static storage. Or an empty map if the first value
 *         of a pair is not unique.
 */
template <class InputContainer>
auto BuildStaticMapFromPairs(const InputContainer& pairs) {
  typedef absl::flat_hash_map<typename InputContainer::value_type::first_type,
                              typename InputContainer::value_type::second_type>
      MapFromPairs;
  return absl::NoDestructor<MapFromPairs>([&]() {
    MapFromPairs map_from_pairs;
    for (const auto& [key, value] : pairs) {
      const auto& [unused_iter, inserted] = map_from_pairs.insert({key, value});
      if (!inserted) [[unlikely]] {
        LOG(ERROR) << "Failed building map from pairs. Duplicate key= "
                   << absl::StrCat(key) << ". Key type= " << typeid(key).name()
                   << " value type= " << typeid(value).name();
        return MapFromPairs{};
      }
    }
    return map_from_pairs;
  }());
}

/*!\brief Returns a map for static storage from a container of inverted pairs.
 *
 * \param pairs Container of pairs to invert and to convert to a map. The second
 *              value must be unique among all pairs.
 * \return Map suitable for static storage. Or an empty map if the second value
 *         of a pair is not unique.
 */
template <class InputContainer>
auto BuildStaticMapFromInvertedPairs(const InputContainer& pairs) {
  typedef absl::flat_hash_map<typename InputContainer::value_type::second_type,
                              typename InputContainer::value_type::first_type>
      MapFromInvertedPairs;
  return absl::NoDestructor<MapFromInvertedPairs>([&]() {
    MapFromInvertedPairs map_from_inverted_pairs;
    for (const auto& [value, key] : pairs) {
      const auto& [unused_iter, inserted] =
          map_from_inverted_pairs.insert({key, value});
      if (!inserted) [[unlikely]] {
        LOG(ERROR) << "Failed building map from pairs. Duplicate key= "
                   << absl::StrCat(key) << ". Key type: " << typeid(key).name()
                   << " value type: " << typeid(value).name();
        return MapFromInvertedPairs{};
      }
    }
    return map_from_inverted_pairs;
  }());
}

}  // namespace iamf_tools

#endif  // COMMON_MAP_UTILS_H_
