// include/myalloc.h — the whole public API of the allocator.
//
// Four functions that mirror C's malloc family, plus two things for looking
// inside: a stats snapshot and a heap checker.

#pragma once

#include <cstddef>

namespace myalloc {

// --- allocation -------------------------------------------------------------

// Returns a 16-byte-aligned pointer to at least n writable bytes,
// or nullptr with errno == ENOMEM.
// my_malloc(0) returns a real, freeable, smallest-possible block. C allows
// either that or nullptr; returning a pointer means a caller can't confuse
// "I asked for zero bytes" with "the allocator failed".
void* my_malloc(std::size_t n);

// my_free(nullptr) does nothing. C requires that, and a lot of cleanup code
// relies on it.
void my_free(void* p);

// count * size bytes, zeroed. The multiplication is overflow-checked.
void* my_calloc(std::size_t count, std::size_t size);

// my_realloc(nullptr, n) == my_malloc(n).
// my_realloc(p, 0)       frees p and returns nullptr (what glibc does).
void* my_realloc(void* p, std::size_t n);

// How many bytes you can actually write behind p. Not part of the C API;
// handy in tests.
std::size_t my_usable_size(const void* p);

// --- looking inside ---------------------------------------------------------

struct Stats {
  // counters
  std::size_t alloc_calls;
  std::size_t free_calls;
  std::size_t realloc_calls;
  std::size_t chunks;             // how many times we had to mmap more memory

  // what the caller is holding right now
  std::size_t live_blocks;
  std::size_t live_bytes;         // total block size, so it includes the 24 B of
                                  // header+footer on each one
  std::size_t peak_live_bytes;

  // MAPPED: address space we asked the kernel for.
  std::size_t bytes_mapped;
  std::size_t peak_bytes_mapped;

  // RESIDENT: physical pages actually backing it, read from /proc/self/statm
  // when you take the snapshot. This is a different number from bytes_mapped
  // and the two are never added together — see DESIGN.md Q11.
  std::size_t resident_bytes;
  std::size_t minor_faults;       // getrusage: one per page, on first touch

  // fragmentation, from a walk of the heap
  std::size_t free_blocks;
  std::size_t free_bytes;
  std::size_t largest_free_block;
  double external_fragmentation;  // 1 - largest/total. 0 = one contiguous run.
};

Stats my_stats();

struct HeapCheck {
  bool ok;
  const char* what;    // which invariant broke
  const void* where;   // which block

  explicit operator bool() const { return ok; }
};

// Walks every block and every free list and checks that the heap is still
// sane. This is the substitute for AddressSanitizer, which cannot see inside
// a custom allocator — see README.
HeapCheck my_heap_check();

// --- test hooks (not for real use) ------------------------------------------

// Refuse to map more than this many bytes in total, so the out-of-memory path
// can be tested without exhausting the machine. 0 = no limit.
void my_set_mmap_limit(std::size_t bytes);

// Unmap everything and start clean. Invalidates every pointer we ever handed
// out. Tests call it between cases.
void my_reset();

}  // namespace myalloc
