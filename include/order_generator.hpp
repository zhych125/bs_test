#pragma once

#include "order.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

class OrderGenerator {
 public:
  explicit OrderGenerator(std::uint64_t seed = 42);

  Order next_order();

  std::vector<Order> generate(std::size_t count);

 private:
  std::mt19937_64 rng_;
  std::uint64_t nextId_;
  std::uint64_t baseTimestamp_;
};

std::vector<std::uint64_t> make_query_ids(const std::vector<Order>& orders,
                                          std::size_t count,
                                          double hit_ratio,
                                          std::mt19937_64& rng);
