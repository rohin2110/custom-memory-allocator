// src/internal.h — the state allocator.cpp and heapcheck.cpp share.
// Not installed; callers never see this.

#pragma once

#include <mutex>

#include "block.h"
#include "myalloc.h"

namespace myalloc {

// One mmap'd region. Chunks are linked into a list so we can unmap them all
// at the end. The link lives inside the mapping itself, which is why
// my_reset() reads c->next BEFORE unmapping c.
struct Chunk {
  Chunk* next;
  std::size_t total;  // exactly what we passed to mmap, so munmap can undo it
};

// Chunk layout:
//
//   +-------------+--------------+---------------------------+-----------+
//   | Chunk (16)  | prologue(32) |  real blocks tile this     | epilogue  |
//   |             | in use,      |  region exactly            | (16)      |
//   |             | never freed  |                            | size 0,   |
//   |             |              |                            | in use    |
//   +-------------+--------------+---------------------------+-----------+
//   0             16             48                    base + total - 16
//
// The prologue and epilogue are fake blocks, permanently marked "in use".
// They exist so that merging never needs a bounds check: merging backwards
// from the first real block hits the prologue's footer and stops; merging
// forwards from the last real block hits the epilogue's header and stops.
// 64 bytes per megabyte to delete two ifs from free(). See DESIGN.md Q7.
inline constexpr std::size_t kChunkHeader   = sizeof(Chunk);   // 16
inline constexpr std::size_t kPrologue      = 32;
inline constexpr std::size_t kEpilogue      = kHeaderSize;     // 16
inline constexpr std::size_t kChunkOverhead = kChunkHeader + kPrologue + kEpilogue;  // 64

static_assert(kChunkOverhead % kAlignment == 0,
              "otherwise the usable region isn't a multiple of 16 and block "
              "sizes drift off the alignment grid");

extern Chunk* g_chunks;
extern FreeNode* g_lists[kNumClasses];
extern Stats g_stats;

// The one lock. Wrapped in a function so there is no static-initialisation
// order problem between translation units.
std::mutex& the_lock();

std::size_t class_of(std::size_t size);
bool inside_a_chunk(const void* p);
std::size_t page_size();

}  // namespace myalloc
