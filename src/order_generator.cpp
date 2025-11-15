#include "order_generator.hpp"

#include <algorithm>
#include <random>

OrderGenerator::OrderGenerator(std::uint64_t seed)
    : rng_{seed}, nextId_{1}, baseTimestamp_{1'000'000} {}

Order OrderGenerator::next_order() {
  Order order;
  order.id = nextId_;
  nextId_ += 1 + static_cast<std::uint64_t>(rng_() & 0x3);
  order.exchangeTimestamp = baseTimestamp_ + (order.id << 5) + (rng_() & 0xFFFF);
  order.volume = static_cast<std::int32_t>((rng_() % 2000) - 1000);
  order.isOwn = (rng_() & 0x1) == 0;
  return order;
}

std::vector<Order> OrderGenerator::generate(std::size_t count) {
  std::vector<Order> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(next_order());
  }
  return out;
}

std::vector<std::uint64_t> make_query_ids(const std::vector<Order>& orders,
                                          std::size_t count,
                                          double hit_ratio,
                                          std::mt19937_64& rng) {
  std::vector<std::uint64_t> ids;
  ids.reserve(count);
  if (orders.empty()) {
    return ids;
  }
  std::bernoulli_distribution hit(hit_ratio);
  std::uniform_int_distribution<std::size_t> hit_dist(0, orders.size() - 1);
  const std::uint64_t miss_base = orders.back().id + 1;
  std::uniform_int_distribution<std::uint64_t> miss_offset(1, orders.back().id);

  for (std::size_t i = 0; i < count; ++i) {
    if (hit(rng)) {
      ids.push_back(orders[hit_dist(rng)].id);
    } else {
      ids.push_back(miss_base + miss_offset(rng));
    }
  }
  return ids;
}
