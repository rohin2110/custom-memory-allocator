// src/block.h — the shape of one block, and the accessors that read it.
//
// Every block looks the same whether it is in use or free:
//
//     +----------------------------+  <- block start, always 16-byte aligned
//     | size + flags        (8 B)  |  header
//     | padding             (8 B)  |
//     +----------------------------+  <- payload, therefore also 16-byte aligned
//     | in use: the caller's bytes |
//     | free:   next, prev links   |
//     +----------------------------+
//     | size + flags        (8 B)  |  footer: a copy of the header word
//     +----------------------------+  <- the next block starts here
//
// `size` is the WHOLE block: header + payload + footer. It is always a
// multiple of 16, so the bottom 4 bits of the word are always zero and we use
// one of them as the "in use" flag. One word carries both.
//
// Two things fall out of this and they are worth knowing cold:
//
//   * Alignment is free. The header is 16 bytes and every block size is a
//     multiple of 16, so if one block starts aligned, every block after it
//     does, and so does every payload. There is no alignment arithmetic
//     anywhere in malloc.
//
//   * The footer makes merging backwards O(1). From a block's start, the
//     previous block's footer is always at (start - 8). Read it and you know
//     the previous block's size and whether it is free, without searching.
//     Forwards is easy anyway: the next block is at (start + size).

#pragma once

#include <cstddef>
#include <cstdint>

namespace myalloc {

using byte = unsigned char;

// ---- tunables --------------------------------------------------------------

// 16 because that is what the x86-64 ABI requires for the widest types
// (long double, SSE). malloc has to work for any type, so it has to satisfy
// the strictest one.
inline constexpr std::size_t kAlignment = 16;

// How much we mmap each time we run out. Fixed rather than doubling: it keeps
// the "bytes mapped" number easy to reason about.
inline constexpr std::size_t kChunkSize = std::size_t(1) << 20;  // 1 MiB

// One free list per power-of-two size range. See DESIGN.md Q5.
inline constexpr std::size_t kNumClasses = 16;

// ---- layout ----------------------------------------------------------------

struct Header {
  std::size_t size_and_flags;
  std::size_t padding;  // unused; exists only so the payload lands 16-aligned
};

struct Footer {
  std::size_t size_and_flags;
};

// Lives inside a free block's payload, so the free lists cost no extra memory:
// a block is either holding the caller's data or holding its own links.
struct FreeNode {
  FreeNode* next;
  FreeNode* prev;
};

inline constexpr std::size_t kHeaderSize = sizeof(Header);               // 16
inline constexpr std::size_t kFooterSize = sizeof(Footer);               // 8
inline constexpr std::size_t kOverhead   = kHeaderSize + kFooterSize;    // 24

// A free block has to store its own two links: 16 + 16 + 8 = 40, rounded up
// to the 16-byte grid.
inline constexpr std::size_t kMinBlock = 48;

inline constexpr std::size_t kInUse    = 0x1;
inline constexpr std::size_t kSizeMask = ~static_cast<std::size_t>(0xF);

// If any of these stop being true the code below silently corrupts memory,
// so fail the build instead.
static_assert(sizeof(std::size_t) == 8, "layout assumes 64-bit size_t");
static_assert(kHeaderSize % kAlignment == 0,
              "payload = block + header, so the header must be a whole number "
              "of alignment units");
static_assert(sizeof(FreeNode) <= kMinBlock - kOverhead,
              "a free block must be able to hold its own links");

// ---- helpers ---------------------------------------------------------------

// `a` must be a power of two. Callers check for overflow before calling.
inline std::size_t align_up(std::size_t n, std::size_t a) {
  return (n + a - 1) & ~(a - 1);
}

// `b` always points at the HEADER of a block, never at the payload.

inline Header* head(void* b) { return static_cast<Header*>(b); }
inline const Header* head(const void* b) { return static_cast<const Header*>(b); }

inline std::size_t block_size(const void* b) {
  return head(b)->size_and_flags & kSizeMask;
}
inline bool in_use(const void* b) {
  return (head(b)->size_and_flags & kInUse) != 0;
}

inline void* payload(void* b) { return static_cast<byte*>(b) + kHeaderSize; }
inline void* block_of(void* p) { return static_cast<byte*>(p) - kHeaderSize; }
inline const void* block_of(const void* p) {
  return static_cast<const byte*>(p) - kHeaderSize;
}

// Bytes the caller may legally touch.
inline std::size_t capacity(const void* b) { return block_size(b) - kOverhead; }

inline FreeNode* node_of(void* b) { return static_cast<FreeNode*>(payload(b)); }
inline void* block_of_node(const FreeNode* n) {
  return const_cast<byte*>(reinterpret_cast<const byte*>(n)) - kHeaderSize;
}

// Writes the header and the footer together. This is the ONLY place that
// writes either of them, which is what makes "header always equals footer"
// true by construction instead of by being careful.
inline void set_block(void* b, std::size_t size, bool used) {
  const std::size_t word = size | (used ? kInUse : 0u);
  head(b)->size_and_flags = word;
  // Locate the footer from the NEW size, not from the size we are overwriting.
  reinterpret_cast<Footer*>(static_cast<byte*>(b) + size - kFooterSize)
      ->size_and_flags = word;
}

inline const Footer* footer(const void* b) {
  return reinterpret_cast<const Footer*>(
      static_cast<const byte*>(b) + block_size(b) - kFooterSize);
}

// The two neighbours. Both O(1) — that is the entire point of the footer.
inline void* next_block(void* b) { return static_cast<byte*>(b) + block_size(b); }

inline bool prev_is_free(const void* b) {
  const auto* f = reinterpret_cast<const Footer*>(static_cast<const byte*>(b) - kFooterSize);
  return (f->size_and_flags & kInUse) == 0;
}
inline void* prev_block(void* b) {
  const auto* f = reinterpret_cast<const Footer*>(static_cast<const byte*>(b) - kFooterSize);
  return static_cast<byte*>(b) - (f->size_and_flags & kSizeMask);
}

}  // namespace myalloc
