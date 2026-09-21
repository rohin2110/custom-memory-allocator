// tests/test_allocator.cpp
//
// Every test starts from an empty heap (my_reset) and most of them finish by
// checking that every heap invariant still holds.
//
// All randomness is seeded with a fixed number, and values come from plain %
// arithmetic rather than std::uniform_int_distribution — the standard does not
// pin down how that maps engine output to results, so the same seed gives a
// different workload on a different standard library. A failing test has to
// reproduce on your machine and on mine.

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "block.h"     // kAlignment, kMinBlock, kOverhead, kChunkSize
#include "internal.h"  // g_lists and class_of, to stage deliberate corruption
#include "myalloc.h"

using namespace myalloc;

namespace {

#define EXPECT_HEAP_OK()                                              \
  do {                                                                \
    const HeapCheck r_ = my_heap_check();                             \
    EXPECT_TRUE(r_.ok) << "broken: " << r_.what << " at " << r_.where; \
  } while (0)

#define ASSERT_HEAP_OK()                                              \
  do {                                                                \
    const HeapCheck r_ = my_heap_check();                             \
    ASSERT_TRUE(r_.ok) << "broken: " << r_.what << " at " << r_.where; \
  } while (0)

#define ASSERT_HEAP_OK_AT(step)                                       \
  do {                                                                \
    const HeapCheck r_ = my_heap_check();                             \
    ASSERT_TRUE(r_.ok) << "broken after step " << (step) << ": "      \
                       << r_.what << " at " << r_.where;              \
  } while (0)

class Alloc : public ::testing::Test {
 protected:
  void SetUp() override { my_reset(); }
  void TearDown() override { my_reset(); }
};

// A pattern that depends on `tag`, so a mix-up shows as a specific mismatch
// rather than "some bytes moved".
void fill(void* p, std::size_t n, std::uint8_t tag) {
  auto* q = static_cast<std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) q[i] = static_cast<std::uint8_t>(tag + (i & 0x7F));
}

bool check(const void* p, std::size_t n, std::uint8_t tag) {
  const auto* q = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    if (q[i] != static_cast<std::uint8_t>(tag + (i & 0x7F))) return false;
  }
  return true;
}

}  // namespace

// --- alignment --------------------------------------------------------------

TEST_F(Alloc, EveryPointerIs16ByteAligned) {
  std::vector<std::size_t> sizes;
  for (std::size_t p = 0; p <= 20; ++p) {
    const std::size_t s = std::size_t(1) << p;
    if (s >= 2) sizes.push_back(s - 1);
    sizes.push_back(s);
    sizes.push_back(s + 1);
  }

  std::vector<void*> live;
  for (std::size_t s : sizes) {
    void* p = my_malloc(s);
    ASSERT_NE(p, nullptr) << "failed to allocate " << s << " bytes";
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % kAlignment, 0u)
        << "pointer for " << s << " bytes is misaligned";
    EXPECT_GE(my_usable_size(p), s);

    // Writing the whole usable range proves the block really is that big and
    // does not run into its neighbour's metadata. The heap check catches it if
    // it does.
    std::memset(p, 0xA5, my_usable_size(p));
    live.push_back(p);
  }
  ASSERT_HEAP_OK();

  for (void* p : live) my_free(p);
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, MallocZeroGivesBackARealBlock) {
  void* p = my_malloc(0);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % kAlignment, 0u);
  my_free(p);
  EXPECT_HEAP_OK();
}

// --- splitting --------------------------------------------------------------

TEST_F(Alloc, BlocksComeOutBackToBack) {
  // Three small allocations out of a fresh chunk must be adjacent, which is
  // only true if each one split the remainder off instead of swallowing it.
  void* a = my_malloc(100);
  void* b = my_malloc(100);
  void* c = my_malloc(100);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);

  // 100 asked for + 24 of header and footer = 124, rounded up to 128.
  EXPECT_EQ(static_cast<char*>(b) - static_cast<char*>(a), 128);
  EXPECT_EQ(static_cast<char*>(c) - static_cast<char*>(b), 128);

  EXPECT_EQ(my_stats().free_blocks, 1u) << "only the unsplit remainder should be free";
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, ALeftoverTooSmallToTrackStaysWithTheBlock) {
  void* a = my_malloc(100);  // 128-byte block
  void* b = my_malloc(100);
  void* c = my_malloc(100);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  my_free(b);                // a 128-byte hole between two blocks in use

  // 88 + 24 = 112. The leftover would be 128 - 112 = 16, below the 48-byte
  // minimum, so the whole 128-byte block has to be handed over as is.
  void* d = my_malloc(88);
  ASSERT_EQ(d, b) << "should have reused the exact hole";
  EXPECT_EQ(my_usable_size(d), 128u - kOverhead);
  EXPECT_HEAP_OK();
}

// --- merging ----------------------------------------------------------------

TEST_F(Alloc, FreeingMergesBothNeighbours) {
  // First find out what "the whole chunk, free" looks like.
  void* warm = my_malloc(16);
  ASSERT_NE(warm, nullptr);
  my_free(warm);
  const std::size_t whole_chunk = my_stats().largest_free_block;
  ASSERT_GT(whole_chunk, 0u);

  void* a = my_malloc(100);
  void* b = my_malloc(100);
  void* c = my_malloc(100);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);

  my_free(a);  // neighbours are the prologue and b: nothing to merge with
  EXPECT_EQ(my_stats().free_blocks, 2u);
  ASSERT_HEAP_OK();

  my_free(c);  // merges forwards, with the rest of the chunk
  EXPECT_EQ(my_stats().free_blocks, 2u);
  ASSERT_HEAP_OK();

  my_free(b);  // merges BOTH ways: with a behind it and with c in front
  const Stats s = my_stats();
  EXPECT_EQ(s.free_blocks, 1u) << "it should all be one block again";
  EXPECT_EQ(s.largest_free_block, whole_chunk)
      << "the chunk should look exactly as it did before any allocation";
  EXPECT_HEAP_OK();
}

// --- realloc ----------------------------------------------------------------

TEST_F(Alloc, ReallocGrowsInPlaceWhenTheNextBlockIsFree) {
  void* a = my_malloc(100);
  ASSERT_NE(a, nullptr);
  fill(a, 100, 0x11);

  void* r = my_realloc(a, 4000);  // the block after `a` is the free remainder
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r, a) << "growing into a free neighbour must not move the block";
  EXPECT_TRUE(check(r, 100, 0x11));
  EXPECT_GE(my_usable_size(r), 4000u);
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, ReallocMovesWhenSomethingIsInTheWay) {
  void* a = my_malloc(100);
  void* wall = my_malloc(100);  // blocks in-place growth
  ASSERT_NE(a, nullptr);
  ASSERT_NE(wall, nullptr);
  fill(a, 100, 0x22);

  void* r = my_realloc(a, 4000);
  ASSERT_NE(r, nullptr);
  EXPECT_NE(r, a) << "with a neighbour in use the block has to move";
  EXPECT_TRUE(check(r, 100, 0x22)) << "realloc must keep the old contents";
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, ReallocShrinksInPlaceAndGivesTheTailBack) {
  void* a = my_malloc(4000);
  void* wall = my_malloc(100);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(wall, nullptr);
  fill(a, 4000, 0x33);

  const std::size_t free_before = my_stats().free_bytes;
  void* r = my_realloc(a, 64);
  ASSERT_EQ(r, a) << "shrinking never needs to move";
  EXPECT_TRUE(check(r, 64, 0x33));
  EXPECT_GT(my_stats().free_bytes, free_before) << "the tail should be back in a free list";
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, ShrinkingNextToAFreeBlockStillMerges) {
  // The case a naive shrink gets wrong: the block after `a` is already free,
  // so the tail we cut off has to merge with it instead of sitting beside it.
  void* a = my_malloc(4000);
  void* b = my_malloc(100);
  void* wall = my_malloc(100);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(wall, nullptr);
  my_free(b);

  ASSERT_EQ(my_realloc(a, 64), a);
  EXPECT_HEAP_OK();  // "two free blocks next to each other" fires here if not
}

TEST_F(Alloc, ReallocFollowsCRulesForNullAndZero) {
  void* p = my_realloc(nullptr, 128);  // same as malloc
  ASSERT_NE(p, nullptr);
  EXPECT_GE(my_usable_size(p), 128u);

  const std::size_t before = my_stats().live_blocks;
  EXPECT_EQ(my_realloc(p, 0), nullptr);  // frees and returns null
  EXPECT_EQ(my_stats().live_blocks, before - 1);
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, FreeingNullDoesNothing) {
  const Stats before = my_stats();
  my_free(nullptr);
  const Stats after = my_stats();
  EXPECT_EQ(after.free_calls, before.free_calls);
  EXPECT_EQ(after.live_blocks, before.live_blocks);
  EXPECT_HEAP_OK();
}

// --- calloc -----------------------------------------------------------------

TEST_F(Alloc, CallocZeroesEvenAReusedDirtyBlock) {
  // Dirty a block, free it, then calloc the same size. The block gets
  // recycled, so if calloc skipped the memset we would see the old bytes.
  void* dirty = my_malloc(512);
  ASSERT_NE(dirty, nullptr);
  std::memset(dirty, 0xFF, 512);
  my_free(dirty);

  auto* p = static_cast<unsigned char*>(my_calloc(64, 8));
  ASSERT_NE(p, nullptr);
  for (std::size_t i = 0; i < 512; ++i) ASSERT_EQ(p[i], 0u) << "byte " << i << " not zero";
  my_free(p);
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, CallocRefusesToOverflow) {
  errno = 0;
  // 2^33 * 2^33 is 2^66, which wraps to 0 in 64 bits. Unchecked, this returns
  // a zero-sized block and the caller then writes gigabytes into it.
  EXPECT_EQ(my_calloc(std::size_t(1) << 33, std::size_t(1) << 33), nullptr);
  EXPECT_EQ(errno, ENOMEM);

  errno = 0;
  EXPECT_EQ(my_calloc(SIZE_MAX, 2), nullptr);
  EXPECT_EQ(errno, ENOMEM);

  void* z = my_calloc(0, 16);  // legal, must still work
  EXPECT_NE(z, nullptr);
  my_free(z);
  EXPECT_HEAP_OK();
}

// --- running out of memory --------------------------------------------------

TEST_F(Alloc, OutOfMemoryReturnsNullAndLeavesTheHeapUsable) {
  // Cap the allocator at two chunks and allocate until it refuses. Using the
  // cap instead of really exhausting memory keeps this fast and safe in CI.
  my_set_mmap_limit(2 * kChunkSize);

  std::vector<void*> live;
  void* p;
  errno = 0;
  while ((p = my_malloc(4096)) != nullptr) {
    live.push_back(p);
    ASSERT_LT(live.size(), 10000u) << "the limit was ignored";
  }
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_GT(live.size(), 100u);

  // A failed allocation must leave everything consistent.
  ASSERT_HEAP_OK();

  // And the heap must work again once memory comes back.
  my_free(live.back());
  live.pop_back();
  EXPECT_NE(my_malloc(4096), nullptr);
}

TEST_F(Alloc, AbsurdRequestIsRefusedInsteadOfWrapping) {
  errno = 0;
  EXPECT_EQ(my_malloc(SIZE_MAX), nullptr);
  EXPECT_EQ(errno, ENOMEM);
  errno = 0;
  EXPECT_EQ(my_malloc(SIZE_MAX - 32), nullptr);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, ARequestBiggerThanAChunkGetsABiggerChunk) {
  const std::size_t big = 4 * kChunkSize;
  void* p = my_malloc(big);
  ASSERT_NE(p, nullptr);
  EXPECT_GE(my_usable_size(p), big);
  EXPECT_GE(my_stats().bytes_mapped, big);
  std::memset(p, 0x5A, big);  // make sure it is all really writable
  EXPECT_HEAP_OK();
  my_free(p);
  EXPECT_HEAP_OK();
}

// --- the heap checker itself ------------------------------------------------

TEST_F(Alloc, CheckerCatchesAOneByteOverflow) {
  // This is exactly the bug AddressSanitizer cannot see in a custom
  // allocator: one byte past the usable region, which lands in this block's
  // own footer.
  void* p = my_malloc(64);
  ASSERT_NE(p, nullptr);
  ASSERT_HEAP_OK();

  *(static_cast<unsigned char*>(p) + my_usable_size(p)) ^= 0xFF;

  const HeapCheck r = my_heap_check();
  EXPECT_FALSE(r.ok);
  EXPECT_STREQ(r.what, "header and footer disagree");
  // The heap is deliberately broken now; TearDown unmaps it wholesale.
}

TEST_F(Alloc, CheckerCatchesABlockThatIsBothInUseAndFree) {
  // The state a double free leaves behind. We stage it directly rather than
  // calling my_free twice, because a real double free is undefined behaviour
  // and could break the lists in ways that make the checker unsafe to run.
  void* p = my_malloc(64);
  ASSERT_NE(p, nullptr);
  ASSERT_HEAP_OK();

  void* b = block_of(p);
  FreeNode* n = node_of(b);
  const std::size_t c = class_of(block_size(b));
  n->prev = nullptr;
  n->next = g_lists[c];
  if (g_lists[c]) g_lists[c]->prev = n;
  g_lists[c] = n;

  const HeapCheck r = my_heap_check();
  EXPECT_FALSE(r.ok);
  EXPECT_STREQ(r.what, "block in use is also in a free list (double free?)");
}

// --- the big one ------------------------------------------------------------

TEST_F(Alloc, RandomSequenceNeverBreaksAnInvariant) {
  std::mt19937 rng(20260921u);  // fixed seed: a failure here reproduces exactly

  struct Live {
    void* p;
    std::size_t n;
    std::uint8_t tag;
  };
  std::vector<Live> live;

  constexpr int kSteps = 1500;
  for (int step = 0; step < kSteps; ++step) {
    const unsigned roll = rng() % 100u;

    if (roll < 55u || live.empty()) {
      // Mostly small, sometimes page-sized — roughly how real programs behave.
      const std::size_t n = (rng() % 100u < 75u) ? 1 + rng() % 256u : 1 + rng() % 16384u;
      void* p = my_malloc(n);
      ASSERT_NE(p, nullptr) << "allocation failed at step " << step;
      ASSERT_EQ(reinterpret_cast<std::uintptr_t>(p) % kAlignment, 0u);
      const auto tag = static_cast<std::uint8_t>(step & 0xFF);
      fill(p, n, tag);
      live.push_back({p, n, tag});

    } else if (roll < 85u) {
      // Free a RANDOM live block, not the newest. That is what produces the
      // interleaved in-use/free pattern that stresses merging both ways.
      const std::size_t i = rng() % live.size();
      ASSERT_TRUE(check(live[i].p, live[i].n, live[i].tag))
          << "contents changed under us at step " << step;
      my_free(live[i].p);
      live[i] = live.back();
      live.pop_back();

    } else {
      const std::size_t i = rng() % live.size();
      ASSERT_TRUE(check(live[i].p, live[i].n, live[i].tag));
      const std::size_t old_n = live[i].n;
      const std::size_t new_n = 1 + rng() % 16384u;

      void* r = my_realloc(live[i].p, new_n);
      ASSERT_NE(r, nullptr) << "realloc failed at step " << step;
      const std::size_t kept = (old_n < new_n) ? old_n : new_n;
      ASSERT_TRUE(check(r, kept, live[i].tag)) << "realloc lost data at step " << step;
      live[i].p = r;
      live[i].n = new_n;
      fill(r, new_n, live[i].tag);
    }

    // The point of the test: after EVERY step, every tag agrees, every free
    // block is on exactly one list, and no two free blocks are touching.
    ASSERT_HEAP_OK_AT(step);
  }

  for (const Live& l : live) {
    EXPECT_TRUE(check(l.p, l.n, l.tag));
    my_free(l.p);
  }
  EXPECT_HEAP_OK();

  const Stats s = my_stats();
  EXPECT_EQ(s.live_blocks, 0u);
  EXPECT_EQ(s.live_bytes, 0u);
  EXPECT_EQ(s.free_blocks, s.chunks) << "each chunk should be one whole free block again";
}

// --- threads ----------------------------------------------------------------

TEST_F(Alloc, ManyThreadsCannotCorruptTheHeap) {
  constexpr int kThreads = 4;
  constexpr int kStepsEach = 3000;
  std::atomic<int> failures{0};

  auto worker = [&failures](unsigned seed) {
    std::mt19937 rng(seed);
    struct Live {
      void* p;
      std::size_t n;
      std::uint8_t tag;
    };
    std::vector<Live> live;

    for (int step = 0; step < kStepsEach; ++step) {
      if (live.empty() || (rng() % 100u) < 60u) {
        const std::size_t n = 1 + rng() % 4096u;
        void* p = my_malloc(n);
        if (!p || reinterpret_cast<std::uintptr_t>(p) % kAlignment != 0) {
          ++failures;
          continue;
        }
        const auto tag = static_cast<std::uint8_t>(seed + step);
        fill(p, n, tag);
        live.push_back({p, n, tag});
      } else {
        const std::size_t i = rng() % live.size();
        // The real assertion: a block one thread is holding must never be
        // handed to another. Without the lock, two threads eventually get the
        // same block and this check fails.
        if (!check(live[i].p, live[i].n, live[i].tag)) ++failures;
        my_free(live[i].p);
        live[i] = live.back();
        live.pop_back();
      }
    }
    for (const Live& l : live) {
      if (!check(l.p, l.n, l.tag)) ++failures;
      my_free(l.p);
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, 1000u + 7u * unsigned(t));
  for (auto& th : threads) th.join();

  EXPECT_EQ(failures.load(), 0) << "a block was handed to two threads at once";
  EXPECT_HEAP_OK();
  EXPECT_EQ(my_stats().live_blocks, 0u);
}

// --- stats, and mapped vs resident ------------------------------------------

TEST_F(Alloc, LiveByteAccountingAddsUp) {
  std::vector<void*> live;
  std::size_t expected = 0;
  for (std::size_t i = 1; i <= 200; ++i) {
    void* p = my_malloc(i * 7);
    ASSERT_NE(p, nullptr);
    expected += my_usable_size(p) + kOverhead;  // live_bytes counts whole blocks
    live.push_back(p);
  }
  const Stats s = my_stats();
  EXPECT_EQ(s.live_blocks, live.size());
  EXPECT_EQ(s.live_bytes, expected);
  EXPECT_EQ(s.peak_live_bytes, expected);

  for (void* p : live) my_free(p);
  EXPECT_EQ(my_stats().live_bytes, 0u);
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, MappedMemoryIsNotResidentUntilYouTouchIt) {
  constexpr std::size_t kBig = 64u * 1024u * 1024u;

  const Stats base = my_stats();
  if (base.resident_bytes == 0) GTEST_SKIP() << "/proc/self/statm not readable here";

  void* p = my_malloc(kBig);
  ASSERT_NE(p, nullptr);

  const Stats untouched = my_stats();
  EXPECT_GE(untouched.bytes_mapped, kBig) << "the address space really was mapped";
  // The kernel hasn't backed any of it yet, so RSS should barely move. Plenty
  // of slack for whatever else the test binary is doing.
  EXPECT_LT(untouched.resident_bytes, base.resident_bytes + kBig / 4)
      << "memory that is mapped but untouched should not be resident";

  auto* q = static_cast<volatile unsigned char*>(p);
  for (std::size_t i = 0; i < kBig; i += 4096) q[i] = 1;  // one byte per page

  const Stats touched = my_stats();
  EXPECT_GT(touched.resident_bytes, untouched.resident_bytes + kBig / 2)
      << "touching every page should have made it resident";
  EXPECT_GT(touched.minor_faults, untouched.minor_faults) << "each first touch is a fault";
  EXPECT_EQ(touched.bytes_mapped, untouched.bytes_mapped)
      << "mapped bytes don't change when pages become resident";

  my_free(p);
  EXPECT_HEAP_OK();
}

TEST_F(Alloc, PunchingHolesRaisesExternalFragmentation) {
  std::vector<void*> live;
  for (int i = 0; i < 400; ++i) {
    void* p = my_malloc(200);
    ASSERT_NE(p, nullptr);
    live.push_back(p);
  }
  const Stats packed = my_stats();

  for (std::size_t i = 0; i < live.size(); i += 2) {
    my_free(live[i]);
    live[i] = nullptr;
  }
  const Stats holey = my_stats();

  EXPECT_GT(holey.free_blocks, packed.free_blocks);
  EXPECT_GT(holey.external_fragmentation, packed.external_fragmentation);
  EXPECT_HEAP_OK();

  for (void* p : live) my_free(p);  // free(nullptr) is a no-op, so this is fine

  // Freeing the rest merges it all back and fragmentation collapses.
  const Stats done = my_stats();
  EXPECT_EQ(done.free_blocks, 1u);
  EXPECT_DOUBLE_EQ(done.external_fragmentation, 0.0);
  EXPECT_HEAP_OK();
}
