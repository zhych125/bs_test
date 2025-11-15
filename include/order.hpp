#pragma once

#include <cstdint>
#include <compare>

struct Order {
  std::uint64_t id{};
  std::uint64_t exchangeTimestamp{};
  std::int32_t volume{};
  bool isOwn{};

  auto operator<=>(const Order&) const = default;
};

struct OrderIdLess {
  bool operator()(const Order& lhs, const Order& rhs) const { return lhs.id < rhs.id; }
  bool operator()(const Order& lhs, std::uint64_t rhsId) const { return lhs.id < rhsId; }
  bool operator()(std::uint64_t lhsId, const Order& rhs) const { return lhsId < rhs.id; }
};
