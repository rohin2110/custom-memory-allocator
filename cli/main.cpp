// cli/main.cpp — run a workload and print what happened.
//
//   ./myalloc_cli                  # mixed workload
//   ./myalloc_cli small 200000
//   ./myalloc_cli mixed 200000
//   ./myalloc_cli frag  20000
//   ./myalloc_cli paging           # mapped vs resident
//   ./myalloc_cli bench            # timing, next to the system malloc
//
// All randomness is seeded, so the same command always does the same thing.
//
// iostream is fine HERE, in main. It is not fine inside the allocator, which
// is why my_stats() hands back a struct and this file does the printing.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "myalloc.h"

using namespace myalloc;

namespace {

double mib(std::size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

void print_stats(const char* title) {
  const Stats s = my_stats();
  std::cout << "\n--- " << title << " ---\n" << std::fixed << std::setprecision(2);

  std::cout << "  calls              alloc " << s.alloc_calls
            << ", free " << s.free_calls
            << ", realloc " << s.realloc_calls
            << ", grew the heap " << s.chunks << " time(s)\n";

  std::cout << "  live               " << s.live_blocks << " blocks, "
            << mib(s.live_bytes) << " MiB (peak " << mib(s.peak_live_bytes) << " MiB)\n";

  // These two are different numbers and are never added together.
  std::cout << "  mapped  (mmap)     " << mib(s.bytes_mapped) << " MiB"
            << "   (peak " << mib(s.peak_bytes_mapped) << " MiB)\n";
  std::cout << "  resident (RSS)     " << mib(s.resident_bytes) << " MiB"
            << "   minor faults " << s.minor_faults << "\n";

  std::cout << "  free space         " << s.free_blocks << " blocks, "
            << mib(s.free_bytes) << " MiB, largest "
            << mib(s.largest_free_block) << " MiB\n";
  std::cout << "  external frag      " << (s.external_fragmentation * 100.0) << " %\n";
}

// Exits non-zero if the heap is broken, so CI can run these as real tests.
void check_heap() {
  const HeapCheck r = my_heap_check();
  std::cout << "  heap check         " << (r.ok ? "OK" : "FAILED: ") << (r.ok ? "" : r.what)
            << "\n";
  if (!r.ok) std::exit(1);
}

// --- workloads --------------------------------------------------------------

// Many equal-sized blocks, all alive at once. The shape of a program building
// one big linked structure.
void workload_small(std::size_t n) {
  std::vector<void*> live;
  for (std::size_t i = 0; i < n; ++i) {
    void* p = my_malloc(64);
    if (!p) break;
    std::memset(p, 1, 64);
    live.push_back(p);
  }
  print_stats("small: 64-byte blocks, all live");
  check_heap();
  for (void* p : live) my_free(p);
}

// Random sizes, random free order, a few reallocs. Roughly 45/45/10 so the
// live set stays steady instead of just growing.
void workload_mixed(std::size_t n) {
  std::mt19937 rng(20260921u);
  std::vector<void*> live;

  for (std::size_t i = 0; i < n; ++i) {
    const unsigned roll = rng() % 100u;
    if (roll < 45u || live.empty()) {
      const std::size_t sz = 1 + rng() % 4096u;
      void* p = my_malloc(sz);
      if (!p) break;
      std::memset(p, 2, sz > 64 ? 64 : sz);
      live.push_back(p);
    } else if (roll < 90u) {
      const std::size_t k = rng() % live.size();
      my_free(live[k]);
      live[k] = live.back();
      live.pop_back();
    } else {
      const std::size_t k = rng() % live.size();
      void* p = my_realloc(live[k], 1 + rng() % 8192u);
      if (p) live[k] = p;
    }
  }
  print_stats("mixed: random sizes, random free order");
  check_heap();
  for (void* p : live) my_free(p);
}

// Deliberately hostile: fill with equal blocks, free every other one, then ask
// for blocks too big to fit in the holes.
void workload_frag(std::size_t n) {
  std::vector<void*> live;
  for (std::size_t i = 0; i < n; ++i) {
    void* p = my_malloc(128);
    if (!p) break;
    live.push_back(p);
  }
  print_stats("packed with 128-byte blocks");

  for (std::size_t i = 0; i < live.size(); i += 2) {
    my_free(live[i]);
    live[i] = nullptr;
  }
  print_stats("after freeing every other one");

  // 256 does not fit in a 128-byte hole, so these force the heap to grow even
  // though there is plenty of free memory. The gap between "free bytes" and
  // "usable free bytes" is what external fragmentation measures.
  std::vector<void*> more;
  for (std::size_t i = 0; i < n / 4; ++i) {
    void* p = my_malloc(256);
    if (!p) break;
    more.push_back(p);
  }
  print_stats("after asking for blocks too big for the holes");
  check_heap();

  for (void* p : live) my_free(p);
  for (void* p : more) my_free(p);
}

// The demonstration that mapped and resident are not the same number.
void workload_paging() {
  constexpr std::size_t kSize = 256u * 1024u * 1024u;  // 256 MiB

  auto line = [](const char* stage) {
    const Stats s = my_stats();
    std::cout << "  " << std::left << std::setw(38) << stage << std::right
              << " mapped " << std::setw(8) << mib(s.bytes_mapped) << " MiB"
              << " | RSS " << std::setw(8) << mib(s.resident_bytes) << " MiB"
              << " | minor faults " << s.minor_faults << "\n";
  };

  std::cout << "\n--- demand paging ---\n" << std::fixed << std::setprecision(1);
  line("before allocating anything");

  auto* p = static_cast<volatile unsigned char*>(my_malloc(kSize));
  if (!p) {
    std::cout << "  could not map 256 MiB\n";
    return;
  }
  line("after my_malloc(256 MiB), untouched");

  // Each first touch is a minor page fault: the kernel finds nothing behind
  // that address, grabs a zeroed page, wires it in, and restarts the
  // instruction. Nothing is read from disk, hence "minor".
  for (std::size_t off = 0; off < kSize / 4; off += 4096) p[off] = 1;
  line("after touching 25% of the pages");
  for (std::size_t off = kSize / 4; off < kSize / 2; off += 4096) p[off] = 1;
  line("after touching 50%");
  for (std::size_t off = kSize / 2; off < kSize; off += 4096) p[off] = 1;
  line("after touching 100%");

  my_free(const_cast<unsigned char*>(p));
  line("after my_free");
  std::cout << "\n  Note: free() does not lower RSS here. The memory goes back to our\n"
               "  free list, not to the kernel — see DESIGN.md Q14.\n";
  check_heap();
}

// --- timing -----------------------------------------------------------------

// This is the one line that makes the timings below mean anything.
//
// GCC and Clang both know exactly what malloc and free do. If the pointer
// never escapes the loop, they delete the allocate/free pair outright, the
// loop takes zero time, and you cheerfully report "0.0 ns per call". I hit
// this while writing these benchmarks: the system-malloc rows came out roughly
// 300,000x faster than ours, which is not a result, it is a deleted loop.
//
// The empty asm block with a "memory" clobber is an optimisation barrier: the
// compiler has to assume some unknown code read p and could have touched any
// memory, so it can't reason about the allocation any more. (Storing p to a
// volatile is not enough once everything is inlined — I tried that first.)
// Touching the first byte is what a real program would do anyway.
inline void use(void* p) {
  asm volatile("" : : "r,m"(p) : "memory");
  if (p) *static_cast<char*>(p) = 1;
}

template <typename F>
double time_ns_per_op(F&& f, std::size_t ops) {
  const auto t0 = std::chrono::steady_clock::now();
  f();
  const auto t1 = std::chrono::steady_clock::now();
  const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  return ns / static_cast<double>(ops);
}

void bench_row(const char* name, double mine, double theirs) {
  std::cout << "  " << std::left << std::setw(34) << name << std::right << std::fixed
            << std::setprecision(1) << std::setw(9) << mine << " ns"
            << std::setw(11) << theirs << " ns"
            << std::setw(9) << std::setprecision(2) << (mine / theirs) << "x\n";
}

void workload_bench() {
  constexpr std::size_t kOps = 300000;

  std::cout << "\n--- timing, against the system malloc ---\n";
  std::cout << "  " << std::left << std::setw(34) << "" << std::right
            << std::setw(12) << "myalloc" << std::setw(14) << "system"
            << std::setw(9) << "ratio\n";

  // 1. small fixed size, allocate and free straight away
  my_reset();
  const double a1 = time_ns_per_op([&] {
    for (std::size_t i = 0; i < kOps; ++i) { void* p = my_malloc(64); use(p); my_free(p); }
  }, kOps);
  const double b1 = time_ns_per_op([&] {
    for (std::size_t i = 0; i < kOps; ++i) { void* p = std::malloc(64); use(p); std::free(p); }
  }, kOps);
  bench_row("small fixed 64 B, alloc+free", a1, b1);

  // 2. random sizes
  std::mt19937 rng(7u);
  std::vector<std::size_t> sizes;
  for (std::size_t i = 0; i < 4096; ++i) sizes.push_back(1 + rng() % 4096u);

  my_reset();
  const double a2 = time_ns_per_op([&] {
    for (std::size_t i = 0; i < kOps; ++i) { void* p = my_malloc(sizes[i & 4095]); use(p); my_free(p); }
  }, kOps);
  const double b2 = time_ns_per_op([&] {
    for (std::size_t i = 0; i < kOps; ++i) { void* p = std::malloc(sizes[i & 4095]); use(p); std::free(p); }
  }, kOps);
  bench_row("random sizes 1-4096 B", a2, b2);

  // 3. allocate everything, then free everything
  constexpr std::size_t kN = 100000;
  std::vector<void*> v(kN);
  my_reset();
  const double a3 = time_ns_per_op([&] {
    for (std::size_t i = 0; i < kN; ++i) { v[i] = my_malloc(128); use(v[i]); }
    for (std::size_t i = 0; i < kN; ++i) my_free(v[i]);
  }, kN * 2);
  const double b3 = time_ns_per_op([&] {
    for (std::size_t i = 0; i < kN; ++i) { v[i] = std::malloc(128); use(v[i]); }
    for (std::size_t i = 0; i < kN; ++i) std::free(v[i]);
  }, kN * 2);
  bench_row("alloc all, then free all", a3, b3);

  // 4. free in the opposite order
  my_reset();
  const double a4 = time_ns_per_op([&] {
    for (std::size_t i = 0; i < kN; ++i) { v[i] = my_malloc(128); use(v[i]); }
    for (std::size_t i = kN; i-- > 0;) my_free(v[i]);
  }, kN * 2);
  const double b4 = time_ns_per_op([&] {
    for (std::size_t i = 0; i < kN; ++i) { v[i] = std::malloc(128); use(v[i]); }
    for (std::size_t i = kN; i-- > 0;) std::free(v[i]);
  }, kN * 2);
  bench_row("alloc all, free newest first", a4, b4);

  std::cout << "\n  Expect to lose on the first two. glibc keeps a small per-thread\n"
               "  cache that serves those with no lock at all; we take a mutex on\n"
               "  every single call. See README, \"where this loses\".\n";
  my_reset();
}

}  // namespace

int main(int argc, char** argv) {
  const char* which = (argc > 1) ? argv[1] : "mixed";
  const std::size_t n =
      (argc > 2) ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 0;

  if (std::strcmp(which, "small") == 0) {
    workload_small(n ? n : 200000);
  } else if (std::strcmp(which, "mixed") == 0) {
    workload_mixed(n ? n : 200000);
  } else if (std::strcmp(which, "frag") == 0) {
    workload_frag(n ? n : 20000);
  } else if (std::strcmp(which, "paging") == 0) {
    workload_paging();
  } else if (std::strcmp(which, "bench") == 0) {
    workload_bench();
    return 0;
  } else {
    std::cout << "usage: myalloc_cli [small|mixed|frag|paging|bench] [count]\n";
    return 2;
  }

  print_stats("final");
  check_heap();
  return 0;
}
