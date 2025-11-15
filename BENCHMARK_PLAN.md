# Binary Search Benchmark Plan

## Overview
- Implement a C++20 benchmark suite using Google Benchmark (`benchmark::State`) to measure `std::lower_bound` (and optionally custom binary search) over multiple containers storing `Order` structs.
- Target release builds via CMake with at least `-O3` to reflect realistic performance expectations.

## Data Model
- `struct Order { uint64_t id; uint64_t exchangeTimestamp; int32_t volume; bool isOwn; };`
- Containers remain sorted by `id`. Deterministic generators create monotonically increasing IDs plus varied timestamps/volumes so insertions preserve ordering.

## Containers
1. `std::vector<Order>` initialized from generated dataset.
2. `std::deque<Order>` initialized similarly; before benchmarking run configurable push/pop churn (e.g., pop front, insert new max ID at back) to simulate book updates.
3. Custom Rust-style `VecDeque<Order>` implemented in C++20:
   - Circular buffer backed by contiguous storage with power-of-two capacity, head/tail indices, amortized growth.
   - Provide random-access iterators so `std::lower_bound` can operate directly.
   - Apply the same churn operations as `std::deque`.

## Benchmark Structure (Google Benchmark)
- Fixture setup builds the container, applies churn, and prepares a list of query IDs (mix of hits/misses). Run the same benchmark code over container sizes `{10, 50, 100, 500, 1000, 10'000, 100'000}` by either parameterizing the fixture or registering separate benchmarks per size.
- Each benchmark run iterates over the precomputed IDs, calling `std::lower_bound` (and optional custom binary search) while accumulating results to prevent DCE.
- Register separate benchmarks for each container/algorithm combination (e.g., `BM_VectorStdLowerBound`, `BM_DequeStdLowerBound`, `BM_VecDequeStdLowerBound`, etc.).
- Use `benchmark::DoNotOptimize`/`benchmark::ClobberMemory` helpers appropriately.

## Build & Tooling
- Use CMake (minimum C++20) with Release configuration and `-O3`. Example:
  ```sh
  cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O3" -S . -B build
  cmake --build build --config Release
  ```
- Link against Google Benchmark (either vendored via FetchContent or relying on system installation).
- Ensure benchmarks execute via `build/benchmarks/binary_search_bench`.

## Next Steps
1. Scaffold CMake project with Google Benchmark dependency.
2. Implement `Order` struct, data generators, and custom VecDeque.
3. Write benchmark fixtures/tests as outlined.
