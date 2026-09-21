// src/heapcheck.cpp — walks the whole heap and checks that it is still sane.
//
// This exists because AddressSanitizer is no help here. ASan works by
// replacing malloc and free with versions that wrap every allocation in
// poisoned bytes. It has no idea we are carving blocks out of an mmap'd
// region, so to ASan the whole region is one big valid allocation: writing
// past the end of a block we handed out lands inside the same mapping and ASan
// sees nothing. The checks below are the replacement, and they are strong
// enough that a one-byte overflow shows up as "header and footer disagree".
//
// It is O(blocks + free_blocks^2), because checking whether a block is in a
// free list is a linear scan. That is fine — this is a test tool, never called
// from malloc.

#include "internal.h"

namespace myalloc {
namespace {

HeapCheck bad(const char* what, const void* where) { return HeapCheck{false, what, where}; }

// How many free lists contain this block? Should be exactly 1 for a free
// block and 0 for one in use.
std::size_t times_in_a_free_list(const void* b) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < kNumClasses; ++i) {
    for (const FreeNode* n = g_lists[i]; n; n = n->next) {
      if (block_of_node(n) == b) ++count;
    }
  }
  return count;
}

}  // namespace

HeapCheck my_heap_check() {
  std::lock_guard<std::mutex> g(the_lock());

  std::size_t free_blocks_found = 0;

  // ---- pass 1: walk every chunk, block by block --------------------------
  //
  // Each step moves forward by exactly the size the block claims for itself.
  // So if the walk lands precisely on the epilogue at the end, the blocks must
  // tile the chunk with no gaps and no overlaps — any block that lied about
  // its size would put us somewhere else.
  for (const Chunk* c = g_chunks; c; c = c->next) {
    byte* base = const_cast<byte*>(reinterpret_cast<const byte*>(c));
    byte* end = base + c->total;

    void* prologue = base + kChunkHeader;
    if (block_size(prologue) != kPrologue || !in_use(prologue)) {
      return bad("prologue sentinel is corrupt", prologue);
    }

    void* b = base + kChunkHeader + kPrologue;
    void* previous = nullptr;

    for (;;) {
      if (static_cast<byte*>(b) + kHeaderSize > end) {
        return bad("walk ran off the end of the chunk", b);
      }

      const std::size_t size = block_size(b);

      if (size == 0) {  // the epilogue
        if (static_cast<byte*>(b) != end - kEpilogue) {
          return bad("epilogue is not where it should be", b);
        }
        if (!in_use(b)) return bad("epilogue is not marked in use", b);
        break;
      }

      if (size < kMinBlock) return bad("block is smaller than the minimum", b);
      if (size % kAlignment != 0) return bad("block size is not a multiple of 16", b);
      if (static_cast<byte*>(b) + size > end - kEpilogue) {
        return bad("block runs past the end of the chunk", b);
      }

      // The workhorse. A one-byte overflow out of the previous block lands
      // here, and so does any bug that resizes a block through only one tag.
      if (footer(b)->size_and_flags != head(b)->size_and_flags) {
        return bad("header and footer disagree", b);
      }

      if (reinterpret_cast<std::uintptr_t>(payload(b)) % kAlignment != 0) {
        return bad("payload is not 16-byte aligned", b);
      }

      if (!in_use(b)) {
        ++free_blocks_found;

        // Two free blocks touching means a free() failed to merge them, which
        // is how a heap slowly turns into unusable confetti.
        if (previous && !in_use(previous)) {
          return bad("two free blocks next to each other (merge was missed)", b);
        }

        const std::size_t n = times_in_a_free_list(b);
        if (n == 0) return bad("free block is in no free list (lost)", b);
        if (n > 1) return bad("free block is in more than one free list", b);
      } else {
        // The double-free signature: the caller still holds this pointer and
        // it is also queued up to be handed out again.
        if (times_in_a_free_list(b) != 0) {
          return bad("block in use is also in a free list (double free?)", b);
        }
      }

      previous = b;
      b = next_block(b);
    }
  }

  // ---- pass 2: walk the free lists themselves ----------------------------
  //
  // Pass 1 proved every free block is on a list. This proves every list entry
  // is a real free block, in the right class, with links that agree.
  std::size_t entries = 0;
  for (std::size_t i = 0; i < kNumClasses; ++i) {
    const FreeNode* behind = nullptr;
    for (const FreeNode* n = g_lists[i]; n; n = n->next) {
      if (n->prev != behind) return bad("free list prev pointer is broken", n);

      const void* b = block_of_node(n);
      if (!inside_a_chunk(b)) return bad("free list entry points outside every chunk", n);
      if (in_use(b)) return bad("a block in use is linked into a free list", b);
      if (class_of(block_size(b)) != i) return bad("free block is in the wrong class", b);

      ++entries;
      behind = n;

      // A cycle would spin here forever. Pass 1 already told us exactly how
      // many free blocks exist, so going past that count proves something is
      // wrong before we hang.
      if (entries > free_blocks_found) return bad("free list is too long (cycle?)", n);
    }
  }
  if (entries != free_blocks_found) {
    return bad("free list count doesn't match the heap walk", nullptr);
  }

  return HeapCheck{true, "ok", nullptr};
}

}  // namespace myalloc
