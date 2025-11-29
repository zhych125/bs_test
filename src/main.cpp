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

#include <absl/container/flat_hash_set.h>

#include "block_level.hpp"
#include "order.hpp"
#include "order_generator.hpp"
#include "vec_deque.hpp"

namespace {

using OrderVolumeBreakdown = VolumeBreakdown<Order>;

constexpr std::array<std::size_t, 7> kSizes{10, 50, 100, 500, 1000, 10'000, 100'000};
constexpr std::size_t kQueryCount = 4'096;
constexpr double kHitRatio = 0.5;

using Clock = std::chrono::steady_clock;

constexpr std::size_t kCacheThrashBytes = 2 * 1024 * 1024;

inline void ThrashCache(std::vector<std::uint8_t>& buffer) {
  for (std::size_t i = 0; i < buffer.size(); i += 64) {
    buffer[i] += static_cast<std::uint8_t>(i);
  }
  benchmark::DoNotOptimize(buffer.data());
}

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
OrderVolumeBreakdown make_container(const std::vector<Order>& orders) {
  OrderVolumeBreakdown out;
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
void apply_churn(OrderVolumeBreakdown& container, OrderGenerator& generator, std::size_t operations) {
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
bool erase_order(OrderVolumeBreakdown& container, std::uint64_t id) {
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
  std::vector<std::uint8_t> cache_buffer(kCacheThrashBytes, 0);

  std::mt19937_64 query_rng(111 * size + 7);
  std::uniform_int_distribution<std::size_t> index_dist;
  if (!snapshot.empty()) {
    index_dist = std::uniform_int_distribution<std::size_t>(0, snapshot.size() - 1);
  }

  for (auto _ : state) {
    const bool want_hit = (query_rng() & 1u) == 0;
    std::uint64_t id = static_cast<std::uint64_t>(query_rng());
    if (want_hit && !snapshot.empty()) {
      id = snapshot[index_dist(query_rng)].id;
    } else {
      id ^= 0x5bd1'0000'0000'0000ull;
    }

    ThrashCache(cache_buffer);
    const auto start = Clock::now();
    auto it = search(container, id);
    benchmark::DoNotOptimize(it);
    const auto end = Clock::now();
    state.SetIterationTime(std::chrono::duration<double>(end - start).count());
  }

  state.SetItemsProcessed(state.iterations());
  state.SetComplexityN(static_cast<long>(size));
}

constexpr auto StdLowerBoundSearch = [](auto& container, std::uint64_t id) {
  return std::lower_bound(
      container.begin(), container.end(), id,
      [](const Order& lhs, std::uint64_t rhs) { return lhs.id < rhs; });
};

constexpr auto VolumeBreakdownFindSearch = [](auto& container, std::uint64_t id) {
  return container.find(id);
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

template <typename Container, typename RangeSelector>
void RunRangeIterationBenchmark(benchmark::State& state, RangeSelector select_range) {
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
  std::mt19937_64 remove_rng(200'000 + size);

  std::vector<std::uint8_t> cache_buffer(kCacheThrashBytes, 0);
  std::size_t last_selected = 0;
  for (auto _ : state) {
    ThrashCache(cache_buffer);
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
    const auto start = Clock::now();
    auto range = select_range(static_cast<const Container&>(container), bounds.first, bounds.second);
    std::int64_t volume_sum = 0;
    std::size_t count = 0;
    for (auto it = range.first; it != range.second; ++it) {
      volume_sum += it->volume;
      ++count;
    }
    last_selected = count;
    benchmark::DoNotOptimize(volume_sum);
    const auto end = Clock::now();
    state.SetIterationTime(std::chrono::duration<double>(end - start).count());
  }

  const double ratio =
      container.empty() ? 0.0 : static_cast<double>(last_selected) / container.size();
  state.counters["selected_ratio"] = ratio;
  state.SetComplexityN(static_cast<long>(size));
}

template <typename Container, typename RangeSelector>
void RunCumsumSliceRangeBenchmark(benchmark::State& state, std::size_t target_len, RangeSelector select_range) {
  const std::size_t size = static_cast<std::size_t>(state.range(0));
  OrderGenerator generator(40'000 + size);
  auto base = generator.generate(size);
  Container container = make_container<Container>(base);

  OrderGenerator churn_gen(60'000 + size);
  apply_churn(container, churn_gen, churn_ops_for_size(size));

  if (container.size() == 0 || container.size() < target_len) {
    state.SkipWithError("Slice larger than container");
    return;
  }

  std::vector<std::int64_t> prefix;
  prefix.reserve(container.size());
  std::int64_t total_volume = 0;
  for (const auto& order : container) {
    total_volume += order.volume;
    prefix.push_back(total_volume);
  }

  const std::int64_t target_volume = static_cast<std::int64_t>(static_cast<double>(total_volume) * 0.3);
  std::size_t start_idx = 0;
  while (start_idx < prefix.size() && prefix[start_idx] < target_volume) {
    ++start_idx;
  }
  if (start_idx >= container.size()) {
    start_idx = container.size() - target_len;
  }
  if (start_idx + target_len > container.size()) {
    start_idx = container.size() - target_len;
  }
  const std::size_t end_idx = std::min(container.size(), start_idx + target_len);
  const std::int64_t lower_volume = (start_idx == 0) ? 0 : prefix[start_idx - 1];
  const std::int64_t upper_volume = (end_idx == 0) ? 0 : prefix[end_idx - 1];

  const std::size_t slice_len = end_idx - start_idx;

  std::vector<std::uint8_t> cache_buffer(kCacheThrashBytes, 0);
  for (auto _ : state) {
    ThrashCache(cache_buffer);
    const auto start = Clock::now();
    auto range = select_range(static_cast<const Container&>(container), lower_volume, upper_volume);
    std::int64_t volume_sum = 0;
    for (auto it = range.first; it != range.second; ++it) {
      volume_sum += it->volume;
    }
    benchmark::DoNotOptimize(volume_sum);
    const auto finish = Clock::now();
    state.SetIterationTime(std::chrono::duration<double>(finish - start).count());
  }

  state.counters["selected_ratio"] =
      container.empty() ? 0.0 : static_cast<double>(slice_len) / container.size();
  state.SetComplexityN(static_cast<long>(slice_len));
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

  std::vector<std::uint8_t> cache_buffer(kCacheThrashBytes, 0);
  for (auto _ : state) {
    ThrashCache(cache_buffer);
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

template <typename Container>
void RunSteadyPushPopBenchmark(benchmark::State& state, bool time_push_back) {
  const std::size_t size = static_cast<std::size_t>(state.range(0));
  OrderGenerator base_gen(100'000 + size);
  Container container = make_container<Container>(base_gen.generate(size));

  OrderGenerator churn_gen(120'000 + size);
  apply_churn(container, churn_gen, churn_ops_for_size(size));

  absl::flat_hash_set<std::uint64_t> id_set;
  for (const auto& order : container) {
    id_set.insert(order.id);
  }

  OrderGenerator op_gen(180'000 + size);

  std::vector<std::uint8_t> cache_buffer(kCacheThrashBytes, 0);
  for (auto _ : state) {
    ThrashCache(cache_buffer);
    auto new_order = op_gen.next_order();
    const auto start = Clock::now();
    if (time_push_back) {
      container.push_back(new_order);
      id_set.insert(new_order.id);
    } else {
      if (!container.empty()) {
        id_set.erase(container.front().id);
        container.pop_front();
      }
    }
    const auto end = Clock::now();
    state.SetIterationTime(std::chrono::duration<double>(end - start).count());
    if (time_push_back) {
      if (!container.empty()) {
        id_set.erase(container.front().id);
        container.pop_front();
      }
    } else {
      container.push_back(new_order);
      id_set.insert(new_order.id);
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

constexpr std::array<std::size_t, 5> kFixedSlices{10, 50, 100, 500, 1000};

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

template <typename Container>
void RegisterSteadyPushPopBenchmarks(const std::string& prefix) {
  auto* push_back = benchmark::RegisterBenchmark(
      (prefix + "/PushBack").c_str(),
      [](benchmark::State& state) {
        RunSteadyPushPopBenchmark<Container>(state, true);
      });
  push_back->UseManualTime();
  auto* pop_front = benchmark::RegisterBenchmark(
      (prefix + "/PopFront").c_str(),
      [](benchmark::State& state) {
        RunSteadyPushPopBenchmark<Container>(state, false);
      });
  pop_front->UseManualTime();
  for (auto size : kSizes) {
    push_back->Arg(static_cast<int>(size));
    pop_front->Arg(static_cast<int>(size));
  }
}

template <typename Container>
void RegisterRangeViewBenchmarks(const std::string& prefix) {
  auto* contiguous = benchmark::RegisterBenchmark(
      (prefix + "/RangeIter/Contiguous").c_str(),
      [](benchmark::State& state) {
        RunRangeIterationBenchmark<Container>(
            state, [](const Container& cont, std::int64_t lower, std::int64_t upper) {
              if constexpr (std::is_same_v<Container, OrderVolumeBreakdown>) {
                auto range = cont.volume_range(lower, upper);
                return std::make_pair(range.first, range.second);
              } else {
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
                return std::make_pair(first, last);
              }
            });
      });
  contiguous->UseManualTime();
  for (auto size : kSizes) {
    contiguous->Arg(static_cast<int>(size));
  }
}

template <typename Container>
void RegisterFixedSliceRangeBenchmarks(const std::string& prefix) {
  for (auto slice : kFixedSlices) {
    auto* bench = benchmark::RegisterBenchmark(
        (prefix + "/RangeIter/FixedSlice/" + std::to_string(slice)).c_str(),
        [slice](benchmark::State& state) {
          RunCumsumSliceRangeBenchmark<Container>(
              state, slice,
              [](const Container& cont, std::int64_t lower, std::int64_t upper) {
                if constexpr (std::is_same_v<Container, OrderVolumeBreakdown>) {
                  auto range = cont.volume_range(lower, upper);
                  return std::make_pair(range.first, range.second);
                } else {
                  std::int64_t sum = 0;
                  auto it = cont.begin();
                  while (it != cont.end() && sum + it->volume <= lower) {
                    sum += it->volume;
                    ++it;
                  }
                  auto begin_it = it;
                  while (it != cont.end() && sum < upper) {
                    sum += it->volume;
                    ++it;
                  }
                  return std::make_pair(begin_it, it);
                }
              });
        });
    bench->UseManualTime();
    for (auto size : kSizes) {
      if (size >= slice) {
        bench->Arg(static_cast<int>(size));
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  ::benchmark::Initialize(&argc, argv);

  RegisterBenchmarks<std::vector<Order>>("Vector/StdLowerBound", StdLowerBoundSearch);

  RegisterBenchmarks<std::deque<Order>>("Deque/StdLowerBound", StdLowerBoundSearch);

  RegisterBenchmarks<VecDeque<Order>>("VecDeque/StdLowerBound", StdLowerBoundSearch);
  RegisterBenchmarks<OrderVolumeBreakdown>("VolumeBreakdown/Find", VolumeBreakdownFindSearch);
  RegisterRangeViewBenchmarks<std::vector<Order>>("Vector");
  RegisterRangeViewBenchmarks<std::deque<Order>>("Deque");
  RegisterRangeViewBenchmarks<VecDeque<Order>>("VecDeque");
  RegisterRangeViewBenchmarks<OrderVolumeBreakdown>("VolumeBreakdown");
  RegisterFixedSliceRangeBenchmarks<std::vector<Order>>("Vector");
  RegisterFixedSliceRangeBenchmarks<std::deque<Order>>("Deque");
  RegisterFixedSliceRangeBenchmarks<VecDeque<Order>>("VecDeque");
  RegisterFixedSliceRangeBenchmarks<OrderVolumeBreakdown>("VolumeBreakdown");

  RegisterRemoveBenchmarks<std::vector<Order>>("Vector/RemoveMiddle");
  RegisterRemoveBenchmarks<std::deque<Order>>("Deque/RemoveMiddle");
  RegisterRemoveBenchmarks<VecDeque<Order>>("VecDeque/RemoveMiddle");
  RegisterRemoveBenchmarks<OrderVolumeBreakdown>("VolumeBreakdown/RemoveMiddle");
  RegisterSteadyPushPopBenchmarks<std::deque<Order>>("Deque/Steady");
  RegisterSteadyPushPopBenchmarks<VecDeque<Order>>("VecDeque/Steady");
  RegisterSteadyPushPopBenchmarks<OrderVolumeBreakdown>("VolumeBreakdown/Steady");

  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
