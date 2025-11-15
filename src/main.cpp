#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "order.hpp"
#include "order_generator.hpp"
#include "vec_deque.hpp"

namespace {

constexpr std::array<std::size_t, 7> kSizes{10, 50, 100, 500, 1000, 10'000, 100'000};
constexpr std::size_t kMutationCount = 4'096;
constexpr std::size_t kQueryCount = 4'096;
constexpr double kHitRatio = 0.5;

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

std::size_t churn_ops_for_size(std::size_t size) {
  if (size < 10) {
    return 0;
  }
  return std::max<std::size_t>(1, size / 10);
}

template <typename Container>
void apply_churn(Container&, OrderGenerator&, std::size_t) {}

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

inline void pop_front_impl(std::deque<Order>& container) { container.pop_front(); }
inline void pop_front_impl(VecDeque<Order>& container) { container.pop_front(); }
inline void pop_front_impl(std::vector<Order>& container) {
  if (!container.empty()) {
    container.erase(container.begin());
  }
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
    std::size_t checksum = 0;
    for (auto id : query_ids) {
      auto it = search(container, id);
      bool found = (it != container.end() && it->id == id);
      checksum += found ? 1 : 0;
      benchmark::DoNotOptimize(it);
    }
    benchmark::DoNotOptimize(checksum);
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

template <typename Iter, typename T, typename Compare>
Iter branchless_lower_bound(Iter first, Iter last, const T& value, Compare comp) {
  using difference_type = typename std::iterator_traits<Iter>::difference_type;
  difference_type count = last - first;
  while (count > 0) {
    difference_type step = count >> 1;
    Iter mid = first + step;
    const bool less = comp(*mid, value);
    first = less ? mid + 1 : first;
    count = less ? (count - (step + 1)) : step;
  }
  return first;
}

constexpr auto BranchlessLowerBoundSearch = [](auto& container, std::uint64_t id) {
  return branchless_lower_bound(
      container.begin(), container.end(), id,
      [](const Order& lhs, std::uint64_t rhs) { return lhs.id < rhs; });
};

template <typename Container>
void RunPushBackBenchmark(benchmark::State& state) {
  const std::size_t size = static_cast<std::size_t>(state.range(0));
  OrderGenerator generator(555 + size);
  Container container = make_container<Container>(generator.generate(size));
  if constexpr (requires(Container& c, std::size_t n) { c.reserve(n); }) {
    container.reserve(size + kMutationCount);
  }

  for (auto _ : state) {
    state.PauseTiming();
    container.clear();
    state.ResumeTiming();
    for (std::size_t i = 0; i < kMutationCount; ++i) {
      container.push_back(generator.next_order());
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kMutationCount));
  state.SetComplexityN(static_cast<long>(size));
}

template <typename Container>
void RunPopFrontBenchmark(benchmark::State& state) {
  const std::size_t size = static_cast<std::size_t>(state.range(0));
  OrderGenerator generator(777 + size);
  Container container = make_container<Container>(generator.generate(size + kMutationCount));

  for (auto _ : state) {
    state.PauseTiming();
    while (container.size() < kMutationCount) {
      auto refill = generator.generate(kMutationCount);
      for (const auto& order : refill) {
        container.push_back(order);
      }
    }
    state.ResumeTiming();
    for (std::size_t i = 0; i < kMutationCount; ++i) {
      pop_front_impl(container);
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kMutationCount));
  state.SetComplexityN(static_cast<long>(size));
}

template <typename Container, typename Search>
void RegisterBenchmarks(const std::string& name, Search search) {
  auto* bench = benchmark::RegisterBenchmark(name.c_str(),
                                             [](benchmark::State& state, Search search_fn) {
                                               RunBenchmark<Container>(state, search_fn);
                                             },
                                             search);
  for (auto size : kSizes) {
    bench->Arg(static_cast<int>(size));
  }
}

template <typename Container>
void RegisterPushBenchmarks(const std::string& name) {
  auto* bench = benchmark::RegisterBenchmark(
      name.c_str(),
      [](benchmark::State& state) {
        RunPushBackBenchmark<Container>(state);
      });
  for (auto size : kSizes) {
    bench->Arg(static_cast<int>(size));
  }
}

template <typename Container>
void RegisterPopBenchmarks(const std::string& name) {
  auto* bench = benchmark::RegisterBenchmark(
      name.c_str(),
      [](benchmark::State& state) {
        RunPopFrontBenchmark<Container>(state);
      });
  for (auto size : kSizes) {
    bench->Arg(static_cast<int>(size));
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::benchmark::Initialize(&argc, argv);

  RegisterBenchmarks<std::vector<Order>>("Vector/StdLowerBound", StdLowerBoundSearch);
  RegisterBenchmarks<std::vector<Order>>("Vector/BranchlessLowerBound", BranchlessLowerBoundSearch);

  RegisterBenchmarks<std::deque<Order>>("Deque/StdLowerBound", StdLowerBoundSearch);
  RegisterBenchmarks<std::deque<Order>>("Deque/BranchlessLowerBound", BranchlessLowerBoundSearch);

  RegisterBenchmarks<VecDeque<Order>>("VecDeque/StdLowerBound", StdLowerBoundSearch);
  RegisterBenchmarks<VecDeque<Order>>("VecDeque/BranchlessLowerBound", BranchlessLowerBoundSearch);
  RegisterPushBenchmarks<std::vector<Order>>("Vector/PushBack");
  RegisterPushBenchmarks<std::deque<Order>>("Deque/PushBack");
  RegisterPushBenchmarks<VecDeque<Order>>("VecDeque/PushBack");

  RegisterPopBenchmarks<std::vector<Order>>("Vector/PopFront");
  RegisterPopBenchmarks<std::deque<Order>>("Deque/PopFront");
  RegisterPopBenchmarks<VecDeque<Order>>("VecDeque/PopFront");

  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
