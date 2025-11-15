#!/usr/bin/env python3

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def parse_args():
  parser = argparse.ArgumentParser(description="Run binary_search_bench and summarize results.")
  parser.add_argument("--binary",
                      default="build/binary_search_bench",
                      help="Path to the compiled benchmark binary.")
  parser.add_argument("--min-time",
                      default="0.01s",
                      help="Value for Google Benchmark --benchmark_min_time flag (e.g. 0.05s).")
  parser.add_argument("--keep-json",
                      action="store_true",
                      help="Do not delete the JSON output file (printed at the end).")
  return parser.parse_args()


def run_benchmark(binary: Path, min_time: str, json_path: Path):
  cmd = [
      str(binary),
      f"--benchmark_min_time={min_time}",
      f"--benchmark_out={json_path}",
      "--benchmark_out_format=json",
  ]
  subprocess.run(cmd, check=True)


def load_results(json_path: Path):
  with json_path.open() as fh:
    return json.load(fh)


def summarize(benchmarks):
  rows = []
  skip_suffixes = ("_mean", "_median", "_stddev", "_cv")
  for bench in benchmarks:
    name = bench["name"]
    if name.endswith(skip_suffixes):
      continue
    parts = name.split("/")
    if len(parts) < 3:
      continue
    container, algo, size = parts[:3]
    try:
      size_value = int(size)
    except ValueError:
      size_value = size
    total_ns = bench["real_time"]
    items_per_second = bench.get("items_per_second")
    per_item_ns = (1e9 / items_per_second) if items_per_second else float("nan")
    rows.append((container, algo, size_value, total_ns, per_item_ns, items_per_second))
  rows.sort(key=lambda r: (r[0], r[1], r[2]))
  return rows


def print_summary(rows):
  print(f"{'Container':<10} {'Algorithm':<18} {'Size':>8} {'ns/iter':>12} {'ns/query':>12} {'items/s':>15}")
  for container, algo, size, total_ns, per_item_ns, ips in rows:
    print(f"{container:<10} {algo:<18} {size:>8} {total_ns:12.2f} {per_item_ns:12.4f} {ips:15.2f}")


def main():
  args = parse_args()
  binary = Path(args.binary)
  if not binary.exists():
    raise SystemExit(f"Benchmark binary not found: {binary}")

  with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
    json_path = Path(tmp.name)

  try:
    run_benchmark(binary, args.min_time, json_path)
    data = load_results(json_path)
  finally:
    if not args.keep_json and json_path.exists():
      json_path.unlink()

  rows = summarize(data["benchmarks"])
  print_summary(rows)
  if args.keep_json:
    print(f"\nJSON results kept at: {json_path}")


if __name__ == "__main__":
  main()
