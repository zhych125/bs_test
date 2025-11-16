# Benchmark Notes

## Data Model & Containers
- `Order` is a sorted struct (`id`, `exchangeTimestamp`, `volume`, `isOwn`). All generators produce monotonically increasing IDs and positive volumes so cumulative sums are strictly increasing.
- Every benchmark size `{10, 50, 100, 500, 1000, 10’000, 100’000}` starts from the same vector of orders. `std::vector`, `std::deque`, and the custom `VecDeque` are constructed from that vector, then passed through `apply_churn` (pop front + push back) to mimic a rolling order book while keeping logical ordering consistent.

## Benchmarks
1. **Binary Search (`StdLowerBound`)**
   - Shared set of query IDs (`kQueryCount`, configurable hit ratio) is generated from the churned snapshot so all containers probe identical hits/misses.
   - Timed loop runs `std::lower_bound` on the container and accumulates a checksum to prevent dead-code elimination. Items processed == number of queries.

2. **Bulk Copy (`BulkCopy/Scalar`, `BulkCopy/Contiguous`)**
   - Each benchmark precomputes lower/upper cumulative-sum bounds (35%/65% quantiles) but recomputes the running sum inside the timed loop.
   - Scalar mode scans the entire container, pushing matching orders and breaking once the sum exceeds the upper bound.
   - Contiguous mode walks to the first element ≥ lower, then to the last element ≤ upper, and `insert`s that contiguous slice. Buffer allocation happens inside the timed loop so costs are measured.

3. **Remove Middle (`RemoveMiddle`)**
   - After churn, record the live order IDs in a vector. Each iteration picks a pseudo-random index from that list, finds/erases the order via binary search, and pauses timing while appending a freshly generated order (its ID goes back into the list so size stays constant).
   - Erase logic is container-specific (`vector::erase`, `deque::erase`, `VecDeque::erase`), but the benchmark harness is shared.
   - Measurements include search + erase; replenishment and bookkeeping are excluded via `state.PauseTiming()`.

## Notes
- Push/pop benchmarks were removed to avoid unrealistic pre-reserve behavior; the suite now focuses on binary search, bulk copy, and middle removal.
- `scripts/run_bench.py` wraps `build/binary_search_bench` with `--benchmark_out=json`, prints a concise table (ns/iter, items/s where available, selected ratios), and now tolerates benchmarks without `items_per_second`.
- `VecDeque` implements a power-of-two ring buffer with random-access iterators and an `erase` method so it can participate in all workloads without copying into a vector first.
