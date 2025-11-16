#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "block_order_book.hpp"
#include "order.hpp"
#include "order_generator.hpp"
#include "vec_deque.hpp"

namespace {

constexpr std::array<std::size_t, 7> kSizes{10, 50, 100, 500, 1000, 10'000, 100'000};
constexpr std::size_t kQueryCount = 4'096;
constexpr double kHitRatio = 0.5;

using Clock = std::chrono::steady_clock;

template <typename Container>
Container make_container(const std::vector<Order>& orders);

template <>
std::vector<Order> make_container(const std::vector<Order>& orders) {
  return orders;
}

template <>
std::deque<Order> make_container(const std::vector<Order>& orders) {
  return std::deque<Order>(orders.begin(), orders.end());
}

template <>
VecDeque<Order> make_container(const std::vector<Order>& orders) {
  VecDeque<Order> out;
  for (const auto& order : orders) {
    out.push_back(order);
  }
  return out;
}

template <>
BlockOrderBook make_container(const std::vector<Order>& orders) {
  BlockOrderBook out;
  for (const auto& order : orders) {
    out.push_back(order);
  }
  return out;
}

std::size_t churn_ops_for_size(std::size_t size) {
  if (size < 10) {
    return 0;
  }
  return std::max<std::size_t>(1, size / 10);
}

template <typename Container>
void apply_churn(Container&, OrderGenerator&, std::size_t) {}

template <>
void apply_churn(std::vector<Order>& container, OrderGenerator& generator, std::size_t operations) {
  if (container.empty()) {
    return;
  }
  operations = std::min(operations, container.size());
  container.erase(container.begin(), container.begin() + static_cast<std::ptrdiff_t>(operations));
  for (std::size_t i = 0; i < operations; ++i) {
    container.push_back(generator.next_order());
  }
}

template <>
void apply_churn(std::deque<Order>& container, OrderGenerator& generator, std::size_t operations) {
  if (container.empty()) {
    return;
  }
  for (std::size_t i = 0; i < operations; ++i) {
    container.pop_front();
    container.push_back(generator.next_order());
  }
}

template <>
void apply_churn(VecDeque<Order>& container, OrderGenerator& generator, std::size_t operations) {
  if (container.empty()) {
    return;
  }
  for (std::size_t i = 0; i < operations; ++i) {
    container.pop_front();
    container.push_back(generator.next_order());
  }
}

template <>
void apply_churn(BlockOrderBook& container, OrderGenerator& generator, std::size_t operations) {
  if (container.empty()) {
    return;
  }
  for (std::size_t i = 0; i < operations; ++i) {
    container.pop_front();
    container.push_back(generator.next_order());
  }
}

template <typename Container>
auto find_order_iterator(Container& container, std::uint64_t id) {
  return std::lower_bound(
      container.begin(), container.end(), id,
      [](const Order& lhs, std::uint64_t rhs) { return lhs.id < rhs; });
}

template <typename Container>
bool erase_order(Container& container, std::uint64_t id) {
  auto it = find_order_iterator(container, id);
  if (it == container.end() || it->id != id) {
    return false;
  }
  container.erase(it);
  return true;
}

template <>
bool erase_order(BlockOrderBook& container, std::uint64_t id) {
  return container.erase_by_id(id);
}

template <typename Container, typename Search>
void RunBenchmark(benchmark::State& state, Search search) {
  const std::size_t size = static_cast<std::size_t>(state.range(0));
  OrderGenerator generator(123);
  auto orders = generator.generate(size);

  Container container = make_container<Container>(orders);

  OrderGenerator churn_generator(10'000 + size);
  apply_churn(container, churn_generator, churn_ops_for_size(size));

  std::vector<Order> snapshot(container.begin(), container.end());

  std::mt19937_64 query_rng(111 * size + 7);
  auto query_ids = make_query_ids(snapshot, kQueryCount, kHitRatio, query_rng);

  for (auto _ : state) {
    const auto start = Clock::now();
    std::size_t checksum = 0;
    for (auto id : query_ids) {
      auto it = search(container, id);
      bool found = (it != container.end() && it->id == id);
      checksum += found ? 1 : 0;
      benchmark::DoNotOptimize(it);
    }
    benchmark::DoNotOptimize(checksum);
    const auto end = Clock::now();
    state.SetIterationTime(std::chrono::duration<double>(end - start).count());
  }
  const auto queries_per_iteration = static_cast<std::int64_t>(query_ids.size());
  state.SetItemsProcessed(state.iterations() * queries_per_iteration);
  state.SetComplexityN(static_cast<long>(size));
}

constexpr auto StdLowerBoundSearch = [](auto& container, std::uint64_t id) {
  return std::lower_bound(
      container.begin(), container.end(), id,
      [](const Order& lhs, std::uint64_t rhs) { return lhs.id < rhs; });
};

std::pair<std::int64_t, std::int64_t> compute_sum_bounds(const std::vector<Order>& orders) {
  if (orders.empty()) {
    return {0, 0};
  }
  std::vector<std::int64_t> prefix;
  prefix.reserve(orders.size());
  std::int64_t sum = 0;
  for (const auto& order : orders) {
    sum += order.volume;
    prefix.push_back(sum);
  }
  std::vector<std::int64_t> sorted = prefix;
  std::sort(sorted.begin(), sorted.end());
  auto pick = [&](double q) -> std::int64_t {
    if (sorted.empty()) {
      return 0;
    }
    const double clamped = std::clamp(q, 0.0, 1.0);
    std::size_t idx =
        static_cast<std::size_t>(clamped * static_cast<double>(sorted.size() - 1));
    if (idx >= sorted.size()) {
      idx = sorted.size() - 1;
    }
    return sorted[idx];
  };
  auto lower = pick(0.35);
  auto upper = pick(0.65);
  if (lower > upper) {
    std::swap(lower, upper);
  }
  return {lower, upper};
}

template <typename Container, typename CopyFn>
void RunBulkCopyBenchmark(benchmark::State& state, CopyFn copy_fn) {
  const std::size_t size = static_cast<std::size_t>(state.range(0));
  OrderGenerator generator(333 + size);
  auto base = generator.generate(size);
  Container container = make_container<Container>(base);

  OrderGenerator churn_gen(50'000 + size);
  apply_churn(container, churn_gen, churn_ops_for_size(size));
  auto bounds = compute_sum_bounds(std::vector<Order>(container.begin(), container.end()));

  OrderGenerator replenish_gen(80'000 + size);
  std::vector<std::uint64_t> removal_ids;
  removal_ids.reserve(container.size());
  for (const auto& order : container) {
    removal_ids.push_back(order.id);
  }
  std::mt19937_64 remove_rng(150'000 + size);

  std::size_t last_copied = 0;
  for (auto _ : state) {
    if (!removal_ids.empty()) {
      std::size_t idx = static_cast<std::size_t>(remove_rng() % removal_ids.size());
      const auto target_id = removal_ids[idx];
      if (erase_order(container, target_id)) {
        removal_ids[idx] = removal_ids.back();
        removal_ids.pop_back();
        auto new_order = replenish_gen.next_order();
        container.push_back(new_order);
        removal_ids.push_back(new_order.id);
      }
    }
    std::vector<Order> buffer;
    const auto start = Clock::now();
    copy_fn(container, bounds.first, bounds.second, buffer);
    last_copied = buffer.size();
    benchmark::DoNotOptimize(buffer.data());
    const auto end = Clock::now();
    state.SetIterationTime(std::chrono::duration<double>(end - start).count());
  }

  const double ratio =
      container.empty() ? 0.0 : static_cast<double>(last_copied) / container.size();
  state.counters["selected_ratio"] = ratio;
  state.SetComplexityN(static_cast<long>(size));
}

template <typename Container>
void RunRemoveBenchmark(benchmark::State& state) {
  const std::size_t size = static_cast<std::size_t>(state.range(0));
  OrderGenerator base_gen(600 + size);
  Container container = make_container<Container>(base_gen.generate(size));

  OrderGenerator churn_gen(70'000 + size);
  apply_churn(container, churn_gen, churn_ops_for_size(size));

  OrderGenerator replenish_gen(90'000 + size);
  std::vector<std::uint64_t> removal_ids;
  removal_ids.reserve(container.size());
  for (const auto& order : container) {
    removal_ids.push_back(order.id);
  }
  std::mt19937_64 remove_rng(1'000 + size);

  for (auto _ : state) {
    if (removal_ids.empty()) {
      break;
    }
    std::size_t idx = static_cast<std::size_t>(remove_rng() % removal_ids.size());
    const auto target_id = removal_ids[idx];

    const auto start = Clock::now();
    bool removed = erase_order(container, target_id);
    const auto end = Clock::now();
    state.SetIterationTime(std::chrono::duration<double>(end - start).count());

    if (removed) {
      removal_ids[idx] = removal_ids.back();
      removal_ids.pop_back();
      auto new_order = replenish_gen.next_order();
      container.push_back(new_order);
      removal_ids.push_back(new_order.id);
    }
  }
  state.SetItemsProcessed(state.iterations());
  state.SetComplexityN(static_cast<long>(size));
}

template <typename Container, typename Search>
void RegisterBenchmarks(const std::string& name, Search search) {
  auto* bench = benchmark::RegisterBenchmark(name.c_str(),
                                             [](benchmark::State& state, Search search_fn) {
                                               RunBenchmark<Container>(state, search_fn);
                                             },
                                             search);
  bench->UseManualTime();
  for (auto size : kSizes) {
    bench->Arg(static_cast<int>(size));
  }
}

template <typename Container>
void RegisterBulkCopyBenchmarks(const std::string& prefix) {
  auto scalar = benchmark::RegisterBenchmark(
      (prefix + "/Scalar").c_str(),
      [](benchmark::State& state) {
        RunBulkCopyBenchmark<Container>(
            state, [](const Container& cont, std::int64_t lower, std::int64_t upper, std::vector<Order>& out) {
              std::int64_t sum = 0;
              for (const auto& order : cont) {
                sum += order.volume;
                if (sum >= lower && sum <= upper) {
                  out.push_back(order);
                } else if (sum > upper) {
                  break;
                }
              }
            });
      });
  scalar->UseManualTime();
  auto contiguous = benchmark::RegisterBenchmark(
      (prefix + "/Contiguous").c_str(),
      [](benchmark::State& state) {
        RunBulkCopyBenchmark<Container>(
            state, [](const Container& cont, std::int64_t lower, std::int64_t upper, std::vector<Order>& out) {
              std::int64_t sum = 0;
              auto first = cont.begin();
              while (first != cont.end() && sum < lower) {
                sum += first->volume;
                if (sum < lower) {
                  ++first;
                }
              }
              auto last = first;
              while (last != cont.end() && sum <= upper) {
                sum += last->volume;
                if (sum <= upper) {
                  ++last;
                }
              }
              if (first != cont.end() && first != last) {
                const auto count = static_cast<std::size_t>(std::distance(first, last));
                out.reserve(count);
                out.insert(out.end(), first, last);
              }
            });
      });
  contiguous->UseManualTime();
  for (auto size : kSizes) {
    scalar->Arg(static_cast<int>(size));
    contiguous->Arg(static_cast<int>(size));
  }
}

template <>
void RegisterBulkCopyBenchmarks<BlockOrderBook>(const std::string& prefix) {
  auto scalar = benchmark::RegisterBenchmark(
      (prefix + "/Scalar").c_str(),
      [](benchmark::State& state) {
        RunBulkCopyBenchmark<BlockOrderBook>(
            state, [](const BlockOrderBook& cont, std::int64_t lower, std::int64_t upper, std::vector<Order>& out) {
              std::int64_t sum = 0;
              for (auto it = cont.begin(); it != cont.end(); ++it) {
                sum += it->volume;
                if (sum >= lower && sum <= upper) {
                  out.push_back(*it);
                } else if (sum > upper) {
                  break;
                }
              }
            });
      });
  scalar->UseManualTime();
  auto contiguous = benchmark::RegisterBenchmark(
      (prefix + "/Contiguous").c_str(),
      [](benchmark::State& state) {
        RunBulkCopyBenchmark<BlockOrderBook>(
            state, [](const BlockOrderBook& cont, std::int64_t lower, std::int64_t upper, std::vector<Order>& out) {
              std::int64_t sum = 0;
              auto first = cont.begin();
              while (first != cont.end() && sum < lower) {
                sum += first->volume;
                if (sum < lower) {
                  ++first;
                }
              }
              auto last = first;
              while (last != cont.end() && sum <= upper) {
                sum += last->volume;
                if (sum <= upper) {
                  ++last;
                }
              }
              if (first != cont.end() && first != last) {
                cont.copy_range_including_tombstones(first, last, out);
              }
            });
      });
  contiguous->UseManualTime();
  for (auto size : kSizes) {
    scalar->Arg(static_cast<int>(size));
    contiguous->Arg(static_cast<int>(size));
  }
}

template <typename Container>
void RegisterRemoveBenchmarks(const std::string& name) {
  auto* bench = benchmark::RegisterBenchmark(
      name.c_str(),
      [](benchmark::State& state) {
        RunRemoveBenchmark<Container>(state);
      });
  bench->UseManualTime();
  for (auto size : kSizes) {
    bench->Arg(static_cast<int>(size));
  }
}
}  // namespace

int main(int argc, char** argv) {
  ::benchmark::Initialize(&argc, argv);

  RegisterBenchmarks<std::vector<Order>>("Vector/StdLowerBound", StdLowerBoundSearch);

  RegisterBenchmarks<std::deque<Order>>("Deque/StdLowerBound", StdLowerBoundSearch);

  RegisterBenchmarks<VecDeque<Order>>("VecDeque/StdLowerBound", StdLowerBoundSearch);
  RegisterBenchmarks<BlockOrderBook>("BlockOrderBook/StdLowerBound", StdLowerBoundSearch);
  RegisterBulkCopyBenchmarks<std::vector<Order>>("Vector/BulkCopy");
  RegisterBulkCopyBenchmarks<std::deque<Order>>("Deque/BulkCopy");
  RegisterBulkCopyBenchmarks<VecDeque<Order>>("VecDeque/BulkCopy");
  RegisterBulkCopyBenchmarks<BlockOrderBook>("BlockOrderBook/BulkCopy");

  RegisterRemoveBenchmarks<std::vector<Order>>("Vector/RemoveMiddle");
  RegisterRemoveBenchmarks<std::deque<Order>>("Deque/RemoveMiddle");
  RegisterRemoveBenchmarks<VecDeque<Order>>("VecDeque/RemoveMiddle");
  RegisterRemoveBenchmarks<BlockOrderBook>("BlockOrderBook/RemoveMiddle");

  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
