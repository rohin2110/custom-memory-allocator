// src/allocator.cpp — the whole allocator: where memory comes from, the free
// lists, and the four public functions. Read it top to bottom.
//
// One rule holds for this entire file: nothing on the malloc/free path may
// call anything that allocates. No printf, no std::cout, no std::string, no
// vector. The reason isn't style — printf calls malloc, so calling printf from
// inside malloc is either infinite recursion or a deadlock on the lock we are
// already holding. That is why my_stats() returns a struct and lets the caller
// print it, instead of printing anything itself.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>  // memcpy and memset don't allocate

#include "internal.h"

namespace myalloc {

// ---------------------------------------------------------------------------
// State. All of it guarded by the_lock().
// ---------------------------------------------------------------------------

Chunk* g_chunks = nullptr;
FreeNode* g_lists[kNumClasses] = {};
Stats g_stats = {};

static std::size_t g_mmap_limit = 0;  // 0 = unlimited; test hook

std::mutex& the_lock() {
  static std::mutex m;
  return m;
}

std::size_t page_size() {
  // Cached: sysconf is a function call we don't want on the growth path, and
  // the page size can't change while the process runs.
  static const std::size_t ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  return ps;
}

// ---------------------------------------------------------------------------
// Free lists: one doubly-linked list per power-of-two size range.
//
//   class 0  -> [48, 64)          class 3  -> [256, 512)
//   class 1  -> [64, 128)         ...
//   class 2  -> [128, 256)        class 15 -> [1 MiB, up)
//
// Doubly linked because merging has to pull a block out of the MIDDLE of a
// list. With only next pointers that would mean searching for the predecessor,
// and free() would stop being O(1).
// ---------------------------------------------------------------------------

std::size_t class_of(std::size_t size) {
  if (size < kMinBlock) size = kMinBlock;  // floor(log2(48)) == 5, so no underflow
  // floor(log2(x)) == 63 - clz(x). One instruction, no loop.
  const unsigned log2 =
      63u - static_cast<unsigned>(__builtin_clzll(static_cast<unsigned long long>(size)));
  const std::size_t i = static_cast<std::size_t>(log2) - 5u;
  return (i >= kNumClasses) ? kNumClasses - 1 : i;
}

// Push at the head: O(1), and the block we just freed is the one most likely
// to still be in cache, so handing it straight back is also the fastest thing
// to do.
static void list_insert(void* b) {
  FreeNode* n = node_of(b);
  const std::size_t c = class_of(block_size(b));
  n->prev = nullptr;
  n->next = g_lists[c];
  if (g_lists[c]) g_lists[c]->prev = n;
  g_lists[c] = n;
}

static void list_remove(void* b) {
  FreeNode* n = node_of(b);
  // The class is recomputed from the block's CURRENT size. That is only right
  // because we always remove a block before changing its size — split and
  // merge both do it in that order. Reverse them and the block gets unlinked
  // from a list it was never on, which corrupts the list silently.
  const std::size_t c = class_of(block_size(b));
  if (n->prev) n->prev->next = n->next; else g_lists[c] = n->next;
  if (n->next) n->next->prev = n->prev;
  n->next = nullptr;
  n->prev = nullptr;
}

// First fit inside the request's own class; if nothing there is big enough,
// take the head of the first larger non-empty class.
//
// Taking that head without checking its size is safe, and it is worth knowing
// why: a request in class c is smaller than 2^(c+6), and every block in class
// c+1 or above is at least 2^(c+6). So anything up there fits.
//
// Returns the block still linked into its list; the caller unlinks it.
static void* find_fit(std::size_t need) {
  const std::size_t c = class_of(need);

  for (FreeNode* n = g_lists[c]; n; n = n->next) {
    void* b = block_of_node(n);
    if (block_size(b) >= need) return b;
  }
  for (std::size_t k = c + 1; k < kNumClasses; ++k) {
    if (g_lists[k]) return block_of_node(g_lists[k]);
  }
  return nullptr;  // caller has to grow the heap
}

// ---------------------------------------------------------------------------
// Getting memory from the kernel
// ---------------------------------------------------------------------------

// Returns the new chunk's single free block, not yet in any list.
// nullptr on failure; the caller sets errno.
static void* grow(std::size_t need) {
  // Overflow guard. Everything below adds constants to `need` and rounds up;
  // if that wrapped we would map a small region and then hand out a block
  // claiming to be enormous.
  if (need > SIZE_MAX - kChunkOverhead - page_size()) return nullptr;

  std::size_t total = kChunkSize;
  const std::size_t want = align_up(need + kChunkOverhead, page_size());
  if (want > total) total = want;  // one request bigger than a chunk gets a bigger chunk

  if (g_mmap_limit != 0 && g_stats.bytes_mapped + total > g_mmap_limit) {
    return nullptr;  // injected out-of-memory, for tests
  }

  // MAP_ANONYMOUS | MAP_PRIVATE: zero-filled pages backed by no file.
  // The kernel allocates NO physical memory here — it just records the range.
  // Pages arrive one at a time, on first touch, as minor faults. That is why
  // bytes_mapped can be huge while resident_bytes is near zero.
  void* raw = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) return nullptr;  // note: MAP_FAILED, not nullptr

  byte* base = static_cast<byte*>(raw);

  Chunk* c = reinterpret_cast<Chunk*>(base);
  c->next = g_chunks;
  c->total = total;
  g_chunks = c;

  ++g_stats.chunks;
  g_stats.bytes_mapped += total;
  if (g_stats.bytes_mapped > g_stats.peak_bytes_mapped) {
    g_stats.peak_bytes_mapped = g_stats.bytes_mapped;
  }

  // Prologue: a fake in-use block whose footer stops backward merging.
  set_block(base + kChunkHeader, kPrologue, /*used=*/true);

  // Epilogue: a bare header, size 0, in use — stops forward merging and stops
  // the heap walk.
  reinterpret_cast<Header*>(base + total - kEpilogue)->size_and_flags = kInUse;

  // Everything in between starts life as one free block.
  void* b = base + kChunkHeader + kPrologue;
  set_block(b, total - kChunkOverhead, /*used=*/false);
  return b;
}

// ---------------------------------------------------------------------------
// Split and merge
// ---------------------------------------------------------------------------

// Cut an in-use block down to `need` and give the tail back to the free lists.
// Does nothing if the leftover would be too small to be a block of its own.
//
// Refusing to split below kMinBlock is where internal fragmentation comes
// from: those spare bytes stay stuck to the caller's block. The alternative —
// leaving a 16-byte scrap behind — is worse, because a block that can't hold
// its own two links can't be tracked at all.
static void split(void* b, std::size_t need) {
  const std::size_t total = block_size(b);
  if (total - need < kMinBlock) return;

  set_block(b, need, /*used=*/true);

  void* tail = static_cast<byte*>(b) + need;
  std::size_t tail_size = total - need;

  // The block after the tail may already be free — that happens when realloc
  // shrinks a block whose neighbour was free. Merge now, or we would leave two
  // free blocks touching, which the heap checker (rightly) calls a bug.
  //
  // Computed as tail + tail_size, not next_block(tail): the tail's header
  // hasn't been written yet, so reading a size out of it would be reading the
  // caller's old data.
  void* after = static_cast<byte*>(tail) + tail_size;
  if (!in_use(after)) {
    list_remove(after);
    tail_size += block_size(after);
  }

  set_block(tail, tail_size, /*used=*/false);
  list_insert(tail);
}

// Merge a just-freed block with whichever neighbours are also free.
// Returns the merged block, which may start earlier than b did.
static void* merge(void* b) {
  std::size_t size = block_size(b);

  // Look at both neighbours BEFORE writing anything: the set_block at the
  // bottom overwrites b's own header, and prev_block reads the footer at b-8.
  const bool take_prev = prev_is_free(b);
  void* nxt = next_block(b);
  const bool take_next = !in_use(nxt);

  if (take_next) {
    list_remove(nxt);      // remove before the size changes — see list_remove
    size += block_size(nxt);
  }
  if (take_prev) {
    void* prv = prev_block(b);
    list_remove(prv);
    size += block_size(prv);
    b = prv;               // the merged block starts at the predecessor
  }

  set_block(b, size, /*used=*/false);
  return b;
}

// ---------------------------------------------------------------------------
// The work, without the locking. my_realloc needs to call these while already
// holding the lock, and the lock isn't recursive.
// ---------------------------------------------------------------------------

// Smallest block that can serve n payload bytes. Caller checks overflow first.
static std::size_t block_for(std::size_t n) {
  const std::size_t need = align_up(n + kOverhead, kAlignment);
  return (need < kMinBlock) ? kMinBlock : need;
}

// The request is unservable if adding our constants to it would wrap.
static bool too_big(std::size_t n) {
  return n > SIZE_MAX - kOverhead - kAlignment - kChunkOverhead - page_size();
}

static void* alloc_locked(std::size_t n) {
  if (n == 0) n = 1;
  if (too_big(n)) {
    errno = ENOMEM;
    return nullptr;
  }

  const std::size_t need = block_for(n);

  void* b = find_fit(need);
  if (b) {
    list_remove(b);
  } else {
    b = grow(need);  // nothing usable anywhere: ask the kernel for more
    if (!b) {
      errno = ENOMEM;
      return nullptr;
    }
  }

  set_block(b, block_size(b), /*used=*/true);
  split(b, need);

  ++g_stats.alloc_calls;
  ++g_stats.live_blocks;
  g_stats.live_bytes += block_size(b);
  if (g_stats.live_bytes > g_stats.peak_live_bytes) {
    g_stats.peak_live_bytes = g_stats.live_bytes;
  }
  return payload(b);
}

static void free_locked(void* p) {
  if (!p) return;  // free(nullptr) does nothing, guaranteed by C

  void* b = block_of(p);

  ++g_stats.free_calls;
  --g_stats.live_blocks;
  g_stats.live_bytes -= block_size(b);

  set_block(b, block_size(b), /*used=*/false);
  list_insert(merge(b));
}

// ---------------------------------------------------------------------------
// Public API: take the lock, do the work, release it.
// ---------------------------------------------------------------------------

void* my_malloc(std::size_t n) {
  std::lock_guard<std::mutex> g(the_lock());
  return alloc_locked(n);
}

void my_free(void* p) {
  std::lock_guard<std::mutex> g(the_lock());
  free_locked(p);
}

void* my_calloc(std::size_t count, std::size_t size) {
  // The check that matters. count * size wrapping is the classic way two
  // attacker-controlled numbers turn into a heap overflow: the allocation
  // comes back tiny and the caller then writes count*size bytes into it.
  //
  // Written as a division so the check itself never does a wrapping multiply.
  if (count != 0 && size > SIZE_MAX / count) {
    errno = ENOMEM;
    return nullptr;
  }
  const std::size_t total = count * size;

  void* p;
  {
    std::lock_guard<std::mutex> g(the_lock());
    p = alloc_locked(total);
  }
  if (!p) return nullptr;

  // Zeroed outside the lock: the block is ours alone now, and zeroing a big
  // buffer while holding the global lock would stall every other thread.
  std::memset(p, 0, total);
  return p;
}

void* my_realloc(void* p, std::size_t n) {
  if (!p) return my_malloc(n);  // realloc(NULL, n) == malloc(n)

  if (n == 0) {
    // C17 calls this implementation-defined and C23 calls it undefined, so we
    // pick glibc's answer and write it down: free and return nullptr.
    my_free(p);
    return nullptr;
  }

  std::lock_guard<std::mutex> g(the_lock());
  ++g_stats.realloc_calls;

  if (too_big(n)) {
    errno = ENOMEM;
    return nullptr;
  }

  void* b = block_of(p);
  const std::size_t was = block_size(b);
  const std::size_t need = block_for(n);

  // Shrinking, or growing into padding we already own: nothing moves.
  if (need <= was) {
    split(b, need);
    g_stats.live_bytes = g_stats.live_bytes - was + block_size(b);
    return p;
  }

  // Growing: if the block physically after us is free and the two together are
  // big enough, swallow it. This is the case that makes "realloc in a loop"
  // O(n) instead of O(n^2) — a buffer at the top of the heap keeps eating the
  // free space above it without ever moving.
  void* nxt = next_block(b);
  if (!in_use(nxt) && was + block_size(nxt) >= need) {
    list_remove(nxt);
    set_block(b, was + block_size(nxt), /*used=*/true);
    split(b, need);
    g_stats.live_bytes = g_stats.live_bytes - was + block_size(b);
    if (g_stats.live_bytes > g_stats.peak_live_bytes) {
      g_stats.peak_live_bytes = g_stats.live_bytes;
    }
    return p;
  }

  // Otherwise: allocate, copy, free. Note the _locked calls — we are already
  // holding the lock.
  const std::size_t old_capacity = capacity(b);
  void* np = alloc_locked(n);
  if (!np) return nullptr;  // C requires the old block to survive a failure

  // We don't record the caller's original request size, only the block size,
  // so we copy the whole old payload. Slightly more than necessary; the price
  // of not carrying a second number in every header.
  std::memcpy(np, p, (old_capacity < n) ? old_capacity : n);
  free_locked(p);
  return np;
}

std::size_t my_usable_size(const void* p) {
  if (!p) return 0;
  // No lock: this reads one word from a block the caller owns, and nothing
  // else can be resizing it while they hold it.
  return capacity(block_of(p));
}

void my_set_mmap_limit(std::size_t bytes) {
  std::lock_guard<std::mutex> g(the_lock());
  g_mmap_limit = bytes;
}

void my_reset() {
  std::lock_guard<std::mutex> g(the_lock());
  while (g_chunks) {
    Chunk* c = g_chunks;
    const std::size_t total = c->total;
    g_chunks = c->next;  // read the link before unmapping the memory it lives in
    ::munmap(c, total);
  }
  for (std::size_t i = 0; i < kNumClasses; ++i) g_lists[i] = nullptr;
  g_stats = Stats{};
  g_mmap_limit = 0;
}

bool inside_a_chunk(const void* p) {
  const byte* q = static_cast<const byte*>(p);
  for (const Chunk* c = g_chunks; c; c = c->next) {
    const byte* base = reinterpret_cast<const byte*>(c);
    if (q >= base + kChunkHeader + kPrologue && q < base + c->total) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

// Resident set size in bytes, from /proc/self/statm. The file is
//   "size resident shared text lib data dt", all counted in PAGES.
// We want field 2.
//
// Done with open/read and a hand-rolled parser rather than fopen/fscanf
// because stdio mallocs a buffer the first time you touch a stream, and this
// library makes a point of never doing that. Returns 0 if anything goes wrong.
static std::size_t read_rss() {
  const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;

  char buf[128];
  ssize_t n;
  do {
    n = ::read(fd, buf, sizeof(buf) - 1);
  } while (n < 0 && errno == EINTR);
  ::close(fd);
  if (n <= 0) return 0;
  buf[n] = '\0';

  const char* s = buf;
  while (*s && *s != ' ') ++s;  // skip field 1
  while (*s == ' ') ++s;

  std::size_t pages = 0;
  bool any = false;
  for (; *s >= '0' && *s <= '9'; ++s) {
    pages = pages * 10 + static_cast<std::size_t>(*s - '0');
    any = true;
  }
  return any ? pages * page_size() : 0;
}

Stats my_stats() {
  Stats s;
  {
    std::lock_guard<std::mutex> g(the_lock());
    s = g_stats;

    // Walk every block to measure the free space. O(number of blocks) — fine
    // between phases, not something to call in a loop.
    std::size_t free_bytes = 0, free_blocks = 0, largest = 0;
    for (const Chunk* c = g_chunks; c; c = c->next) {
      byte* base = const_cast<byte*>(reinterpret_cast<const byte*>(c));
      void* b = base + kChunkHeader + kPrologue;
      while (block_size(b) != 0) {  // size 0 is the epilogue
        if (!in_use(b)) {
          free_bytes += block_size(b);
          ++free_blocks;
          if (block_size(b) > largest) largest = block_size(b);
        }
        b = next_block(b);
      }
    }
    s.free_bytes = free_bytes;
    s.free_blocks = free_blocks;
    s.largest_free_block = largest;

    // How chopped up the free space is. 0 means it is all one run; close to 1
    // means the same total scattered across many small holes.
    s.external_fragmentation =
        free_bytes ? 1.0 - static_cast<double>(largest) / static_cast<double>(free_bytes)
                   : 0.0;
  }

  // Sampled outside the lock: these are process-wide numbers that have nothing
  // to do with our data structures.
  s.resident_bytes = read_rss();

  struct rusage ru;
  if (::getrusage(RUSAGE_SELF, &ru) == 0) {
    // Minor = served without disk I/O. Touching a fresh anonymous page costs
    // exactly one of these, so this counter is what moves when a program
    // starts writing into memory it has mapped but never used.
    s.minor_faults = static_cast<std::size_t>(ru.ru_minflt);
  }
  return s;
}

}  // namespace myalloc
