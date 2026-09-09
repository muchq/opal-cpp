#ifndef OPAL_CORE_CONTAINER_TRAITS_H_
#define OPAL_CORE_CONTAINER_TRAITS_H_

#include <map>
#include <optional>
#include <type_traits>
#include <vector>

namespace opal::internal {

// Detection for the containers generated members use — shared by the
// element-wise dispatchers in hash.h (HashValue) and print.h (DebugAppend).

template <typename T>
struct IsVector : std::false_type {};
template <typename E, typename A>
struct IsVector<std::vector<E, A>> : std::true_type {};

template <typename T>
struct IsMap : std::false_type {};
template <typename K, typename V, typename C, typename A>
struct IsMap<std::map<K, V, C, A>> : std::true_type {};

template <typename T>
struct IsOptional : std::false_type {};
template <typename E>
struct IsOptional<std::optional<E>> : std::true_type {};

}  // namespace opal::internal

#endif  // OPAL_CORE_CONTAINER_TRAITS_H_
