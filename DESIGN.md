# DESIGN — every decision, and why

Written as questions and answers, because those are the questions this project
gets asked. If you can answer all of these without looking, you own the code.

---

### Q1. What does this thing actually do?

It replaces `malloc`, `free`, `calloc` and `realloc`.

It asks the kernel for memory in 1 MiB slabs with `mmap`, and then hands out
pieces of those slabs. Each piece — a *block* — carries its own size at both
ends, which is what lets a freed block find its neighbours and merge with them.
Free blocks are kept in 16 linked lists, sorted roughly by size, so finding one
big enough is quick.

That is the whole idea. Everything below is a detail of it.

```
  my_malloc(100)
        |
        v
  +------------------------------------------------------+
  |  is there a free block big enough?                    |
  |     yes -> take it, cut off what we don't need         |
  |     no  -> mmap another 1 MiB and take it from there   |
  +------------------------------------------------------+

  my_free(p)
        |
        v
  +------------------------------------------------------+
  |  mark it free                                         |
  |  is the block before it free?  merge                  |
  |  is the block after it free?   merge                  |
  |  put the result in a free list                        |
  +------------------------------------------------------+
```

---

### Q2. Why 16-byte alignment, and how do you guarantee it?

**Why 16:** `malloc` has to work for *any* type, so it has to satisfy the
strictest alignment any type needs. On x86-64 that is 16 bytes — what a
`long double` or an SSE register type requires. Returning an 8-aligned pointer
would work for most things and crash on some.

**How:** by arranging things so it can't go wrong, rather than by computing
anything. The header is 16 bytes. Every block size is a multiple of 16. So if
one block starts on a 16-byte boundary, the block after it does too, and so
does every payload (block start + 16). `mmap` gives us page-aligned memory to
start from, and the fixed stuff at the front of a chunk is 48 bytes — also a
multiple of 16.

There is no alignment arithmetic anywhere in `my_malloc`. There is a
`static_assert` that fails the build if the header size stops being a multiple
of 16.

---

### Q3. The header is 16 bytes but only 8 carry information. Isn't that wasteful?

Yes, 8 bytes per block are pure padding. It buys the answer to Q2: with a
16-byte header the payload alignment is automatic.

A production allocator does it the other way: an 8-byte header, with the block
*start* deliberately offset by 8 so the payload still lands aligned. That saves
8 bytes per allocation and costs you an alignment argument that is much harder
to keep straight. This project chose the simple version and pays 8 bytes.

If you wanted that word back for something useful, the obvious use is storing
the caller's original request size — that would give exact internal-
fragmentation numbers and let `realloc` copy only the bytes the caller actually
asked for instead of the whole old payload.

---

### Q4. Why are the size and the "in use" flag in the same word?

Because block sizes are always a multiple of 16, the bottom four bits of the
size are always zero. That is four free bits. We use one of them for "in use".

```
   size_and_flags:  ...0000 0000 1000 0000  =  128, free
                    ...0000 0000 1000 0001  =  128, in use
                                        ^
                                        the flag lives here

   block_size(b)  ->  word & ~0xF
   in_use(b)      ->  word &  0x1
```

One 8-byte word carries both, and reading either is one instruction.

---

### Q5. Why is there a footer? Nothing ever reads it on a block that's in use.

The footer is what makes merging **backwards** O(1).

```
        ... the block before ...            this block ...
   +--------+---------------+--------+--------+-------------+--------+
   | header |    payload    | footer | header |   payload   | footer |
   +--------+---------------+--------+--------+-------------+--------+
                                ^        ^
                                |        |
                     always at (b - 8)   b
```

When you free block `b`, you want to know whether the block physically before
it is also free, so you can merge them into one bigger block. Without a footer
you would have to walk the whole chunk from the start to find out who your
predecessor is. With a footer, it is at `b - 8`. Read it, and you have the
previous block's size and its in-use flag.

Merging forwards is easy either way — the next block is at `b + size`.

You are right that it is dead weight on a block that is in use: only free
blocks get merged into. Real allocators exploit that by keeping a "the block
before me is free" bit in the header and writing a footer only on free blocks,
which saves 8 bytes per allocation. This project keeps the footer everywhere
because "the header and the footer always match" is the invariant the heap
checker uses to catch buffer overflows (Q15), and that is worth more here than
8 bytes.

---

### Q6. Why 16 separate free lists instead of one?

One list means every allocation walks it looking for something big enough. With
thousands of free blocks that is slow, and it gets slower as the program runs.

Splitting by size range fixes that. List `i` holds blocks of size
`[2^(i+5), 2^(i+6))`:

```
  class 0   [48, 64)         --> [56] <-> [48] <-> null
  class 1   [64, 128)        --> null
  class 2   [128, 256)       --> [224] <-> [128] <-> [192] <-> null
  class 3   [256, 512)       --> [300] <-> null
  ...
  class 15  [1 MiB, up)      --> [1044432] <-> null      (the rest of a chunk)
```

Finding the right list is one instruction: `floor(log2(size)) - 5`, and
`floor(log2(x))` is `63 - __builtin_clzll(x)`, a single count-leading-zeros.

**Why powers of two** rather than, say, one list per 16 bytes: 16 lists cover
everything from 48 bytes to a megabyte and beyond, the index is free to
compute, and program allocation sizes really do cluster around powers of two.
The cost is that one list spans a 2x size range, so the first block you find in
it might be nearly twice what you needed — which is fine, because you split off
the excess anyway (Q9).

---

### Q7. Why doubly linked, and why push new blocks at the head?

**Doubly linked** because merging has to pull a block out of the *middle* of a
list. If I free block `b` and the block after it is free, that neighbour is
sitting somewhere in a list and has to come out. With only `next` pointers I
would have to search the list for its predecessor first — O(n) — and `free`
would stop being O(1). With `prev` pointers it is four assignments.

**Push at the head** because it is O(1) with no traversal, and because the
block you just freed is the one most likely to still be in the CPU cache. If
the next allocation is the same size, handing that exact block straight back is
the cheapest possible thing to do.

The cost is that the lists end up in no particular order, which is why the
search is first-fit and not best-fit (Q10).

---

### Q8. What are the prologue and epilogue for?

They are fake blocks at each end of a chunk, permanently marked "in use". They
exist so that merging never needs a bounds check.

```
  +-------------+--------------+---------------------------+-----------+
  | Chunk (16)  | prologue(32) |   real blocks             | epilogue  |
  |  next,total |  in use      |                           |  size 0,  |
  |             |              |                           |  in use   |
  +-------------+--------------+---------------------------+-----------+
  0             16             48                    base + total - 16
```

Without them, `free` would need two extra tests: *am I the first block in this
chunk?* and *am I the last?* With them:

- merging backwards from the first real block reads the prologue's footer, sees
  "in use", and stops;
- merging forwards from the last real block reads the epilogue's header, sees
  "in use", and stops.

64 bytes per megabyte to delete two `if`s from the hot path. It is also the
standard arrangement from the CSAPP textbook, so it is what an interviewer who
knows this material expects to see.

---

### Q9. Why is the minimum block 48 bytes? A 16-byte request only needs 16.

Because a **free** block has to store its own two list pointers, and it stores
them in the payload:

```
  header (16) + next (8) + prev (8) + footer (8) = 40, rounded up to 48
```

A block smaller than that could not be tracked once it was freed. So when a
split would leave a leftover under 48 bytes, we don't split — the spare bytes
stay attached to the caller's block.

That is exactly where **internal fragmentation** comes from: bytes inside a
block that the caller paid for and cannot use. A 16-byte request costs 48
bytes. It is the biggest single weakness of this layout, and Q3 explains what
you would change first to shrink it.

---

### Q10. Why first fit and not best fit?

First fit stops as soon as it finds something big enough. Best fit scans the
whole list every time to find the *smallest* block that fits, hoping to waste
fewer bytes.

The reason not to bother: on real programs the two produce almost the same
amount of fragmentation. Free lists in practice hold a handful of repeated
sizes, not a smooth spread, so "the first one that fits" is usually also "the
best one that fits". You would be paying for a full scan on every allocation to
buy a difference that mostly isn't there. (This is the well-known result from
Johnstone and Wilson's fragmentation survey.)

Also, the size classes already do most of the work best fit would do: by the
time we're scanning, every block in the list is within 2x of what was asked
for.

Two other policies considered and rejected:

- **Next fit** (carry on scanning from where the last search stopped). Sounds
  like it would spread wear evenly; in practice it scatters related
  allocations across the heap and measurably *increases* fragmentation.
- **Address-ordered lists** (keep each list sorted by address, so allocations
  cluster at low addresses). Genuinely better for fragmentation, but inserting
  becomes O(n), which would make `free` the slowest thing in the allocator.

---

### Q11. Walk me through `free`. Why is it O(1)?

```
  1. mark the block free                          (write header + footer)
  2. is the block before it free?                 (read the footer at b - 8)
  3. is the block after it free?                  (read the header at b + size)
  4. unlink whichever of those are free           (doubly linked: O(1) each)
  5. write one merged header and footer
  6. push the result onto the right free list     (at the head: O(1))
```

Every step is a fixed number of memory accesses. No searching anywhere.

There are four cases and the code handles them uniformly:

```
  (a) neither neighbour free      (b) the one after is free
      +-----+=====+-----+             +-----+=====+-----+
      |used |FREED|used |             |used |FREED|free |
      +-----+=====+-----+             +-----+=====+-----+
             stays                           |<-merged->|

  (c) the one before is free      (d) both are free
      +-----+=====+-----+             +-----+=====+-----+
      |free |FREED|used |             |free |FREED|free |
      +-----+=====+-----+             +-----+=====+-----+
      |<-merged->|                    |<------merged--->|
```

**Two ordering rules make this work, and both are easy to get wrong:**

1. Look at both neighbours *before* writing anything. The final write
   overwrites this block's own header, and finding the previous block means
   reading the footer at `b - 8`.
2. Unlink a neighbour from its list *before* changing any size. `list_remove`
   works out which list a block is in by recomputing its size class. Change the
   size first and it gets unlinked from a list it was never on — which
   corrupts that list silently, and you find out much later, somewhere else.

---

### Q12. What is the difference between "mapped" and "resident"?

This is the part worth being able to explain without stumbling.

When you call `mmap` for 256 MB, the kernel allocates **zero** physical memory.
It writes down that the range exists and returns. Every page in it is marked
"not present" in the page table.

The first time you *write* to an address in that range, the CPU takes a page
fault. The kernel sees the range is valid and anonymous, grabs one zeroed
physical page, wires it into the page table, and restarts your instruction.
Nothing was read from disk, so this is a **minor** fault. One per 4 KB page,
on first touch only.

So there are two completely different numbers:

```
  mapped   = 256 MB    address space; costs one kernel bookkeeping record
  resident =   0 MB    physical pages actually behind it

           ... touch every page ...

  mapped   = 256 MB    unchanged
  resident = 256 MB    now it is real memory
  faults   = +65536    256 MB / 4 KB
```

This allocator reports them separately and never adds them together:

- `bytes_mapped` is our own running total of `mmap` lengths.
- `resident_bytes` comes from field 2 of `/proc/self/statm` (in pages).
- `minor_faults` comes from `getrusage(RUSAGE_SELF)`.

`./myalloc_cli paging` walks through the whole sequence and prints it.

---

### Q13. Why `mmap` and not `brk`/`sbrk`?

`brk` moves a single pointer at the end of one contiguous region. That means
memory can only be given back to the OS from the very top: if you free
something in the middle, the region can't shrink past it, and one long-lived
allocation near the top pins everything below it.

`mmap` gives independent regions you can create and destroy in any order. It is
also what glibc itself uses above a size threshold, for exactly this reason.

---

### Q14. Why doesn't `free` lower RSS in the paging demo?

Because freeing puts the block back in *our* free list. The memory is still
mapped and the physical pages are still backing it — we just consider it
available again. From the kernel's point of view nothing happened.

To actually lower RSS you have to tell the kernel, and there are two ways:

- `munmap` the whole region. Only possible if the region is entirely free,
  which a chunk usually isn't.
- `madvise(addr, len, MADV_DONTNEED)` on the whole pages inside a big free
  block. The address space stays mapped (so `bytes_mapped` doesn't move) but
  the physical pages go back and RSS falls. The catch is that those pages come
  back **zeroed** on the next touch, so the range you hand to `madvise` must
  exclude the block's own header, footer and list pointers — trimming those
  would wipe the allocator's own bookkeeping.

Neither is implemented here. This is the single most interesting thing you
could add next, and knowing precisely why it is tricky is most of the value.

---

### Q15. Why can't AddressSanitizer find bugs in this?

ASan works by replacing `malloc` and `free` with instrumented versions that
surround each allocation with poisoned "redzone" bytes, and checking every
memory access against a shadow map.

It has no idea this library carves blocks out of an `mmap`'d region. To ASan,
that whole region is one enormous valid allocation. Write one byte past the end
of a block we handed out and it lands inside the same mapping — ASan sees a
perfectly legal write and says nothing.

So the substitute is `my_heap_check()`, which walks everything and verifies:

- every block's header matches its footer;
- sizes are at least 48 and a multiple of 16; payloads are 16-aligned;
- the walk lands exactly on the epilogue — **which is the proof that blocks
  tile the chunk with no gaps and no overlaps**, since each step advances by
  the size the block claims for itself;
- no two free blocks are physically adjacent (that would mean a merge was
  missed);
- every free block is in exactly one free list, in the right class;
- no in-use block is in any free list (the double-free signature);
- free list `next`/`prev` agree, and the number of list entries matches the
  number of free blocks the walk found (catches both leaks and cycles).

A one-byte overflow lands in the block's own footer, so it shows up as
"header and footer disagree". There is a test that does exactly that.

UBSan *is* useful here, because alignment and integer-overflow checks don't
depend on knowing where the heap is. CI runs it.

---

### Q16. Why is the overflow check in `calloc` written as a division?

`calloc(count, size)` has to allocate `count * size` bytes. If that
multiplication wraps, the allocation comes back tiny and the caller then writes
`count * size` bytes into it — a heap overflow driven by two numbers that, in
real code, often come from a file or a network packet.

The check is:

```cpp
if (count != 0 && size > SIZE_MAX / count) { errno = ENOMEM; return nullptr; }
```

Written as a division on purpose. The tempting version —
`count * size / size == count` — already does the wrapping multiply you were
trying to avoid.

---

### Q17. What does `realloc` do, and when does it avoid copying?

Three cases:

1. **Shrinking** (or growing into padding we already own): change the size in
   place and give the tail back to the free lists. Nothing moves.
2. **Growing, and the block physically after us is free and big enough**:
   swallow that neighbour. Nothing moves. This is the case that makes the
   common "grow a buffer in a loop" pattern O(n) instead of O(n²) — a buffer at
   the top of the heap keeps eating the free space above it without ever being
   copied.
3. **Otherwise**: allocate, `memcpy`, free.

Two details worth knowing:

- In case 1, the tail we cut off might end up next to a block that was *already*
  free. It has to merge with it immediately, or we break the "no two adjacent
  free blocks" invariant. There is a test named after exactly this case.
- `my_realloc` calls the internal `alloc_locked`/`free_locked`, not the public
  `my_malloc`/`my_free`, because it is already holding the lock and the mutex
  is not recursive. Calling the public versions would deadlock. Splitting the
  functions is better than reaching for `std::recursive_mutex`, which hides
  re-entrancy problems instead of preventing them.

---

### Q18. What is the complexity of each operation?

| Operation | Cost | Why |
|---|---|---|
| `my_malloc`, block found in its own class | O(k) | k = blocks scanned before one fits; O(1) if the first one does |
| `my_malloc`, falls through to a bigger class | O(1) | take that list's head, no scan needed |
| `my_malloc`, needs more memory | O(1) + one `mmap` | |
| split | O(1) | two writes and a list push |
| `my_free` | O(1) | see Q11 |
| merge | O(1) | at most two unlinks and one insert |
| `my_realloc`, shrink or in-place grow | O(1) | no copy |
| `my_realloc`, has to move | O(n) | one `memcpy` |
| `my_calloc` | O(n) | the `memset` |
| `my_stats` | O(blocks) | walks the heap |
| `my_heap_check` | O(blocks + free²) | test tool only, never called from malloc |

The worst case for `my_malloc` is a long scan in one class where every block is
slightly too small. It is bounded by the length of that one list, never by the
size of the whole heap.

---

### Q19. What did you deliberately leave out?

Each of these is a real feature of a production allocator, left out to keep
this small enough to understand completely. Being able to say what each one
would do is worth more than having half-built it.

**Per-thread caches.** Every call here takes one global mutex, so threads
serialise completely. glibc gives each thread a small array of free lists
(`tcache`) — a hit is "pop a pointer and return", with no lock and no atomic at
all, and it only takes the real lock on a miss, refilling a batch of blocks at
a time. It would be the single biggest speedup. It is not here because cached
blocks are invisible to everyone else (memory cost), they don't get merged
(fragmentation cost), and the heap checker's "every free block is in exactly
one list" invariant stops being checkable — a thread can't safely read another
thread's cache.

**Giving memory back to the OS** (`madvise(MADV_DONTNEED)`, and `munmap`-ing a
chunk that has become entirely free). See Q14 for how it works and why it is
fiddly.

**A separate path for large allocations.** glibc gives any request over
~128 KB its own `mmap`, so that `free` can `munmap` it and genuinely return the
memory. Here everything comes out of the arena, which is simpler but means a
one-off 50 MB buffer stays with us forever. Adding it is maybe 40 lines: a flag
bit saying "this block is its own mapping", and a list of them.

**`mremap` in realloc.** Growing a large mapping currently means map, copy,
unmap. `mremap` re-points the page tables and copies nothing.

**`posix_memalign`.** Returning a pointer aligned to something bigger than 16
(a page, say, for direct I/O). The technique: over-allocate by
`alignment + minimum block`, find the first correctly-aligned spot far enough
in, split the block there, and return the front piece to the free lists.

**Huge pages.** Backing the arena with 2 MB pages so one TLB entry covers 512x
more memory. Needs 2 MB-aligned chunks, and it interacts badly with trimming.

**`LD_PRELOAD` interposition.** Exporting `malloc`/`free` with C linkage from a
shared library so any program can be run against this allocator. The hard part
is bootstrap: the dynamic linker calls `calloc` before your constructors run.

---

### Q20. What are the known limitations?

1. **One global mutex.** Correct, and a hard ceiling on multi-threaded speed.
2. **24 bytes of metadata per block and a 48-byte minimum.** Roughly 3x glibc's
   overhead on small objects.
3. **Memory is never returned to the OS.** The heap only grows.
4. **Invalid frees are not detected.** Passing a foreign pointer or freeing
   twice is undefined behaviour and is not caught at the point of the mistake;
   `my_heap_check()` catches the damage afterwards.
5. **A free list can get long** under a pathological workload, and first fit
   then scans it.
6. **Linux only.** `mmap`, `/proc/self/statm` and `getrusage` are all assumed.
7. **`my_reset()` is a loaded gun.** It unmaps everything and invalidates every
   outstanding pointer. It exists for test isolation and has no place in real
   code.

---

### Q21. Two bugs this design is prone to

Both of these actually occurred while building it, and both are worth keeping
in mind because neither produces an obvious crash at the scene.

**Writing a header before reading the thing that follows it.** In `split`, the
code needs to know whether the block after the new tail is free. The obvious
way is `next_block(tail)` — but `tail`'s header hasn't been written yet at that
point, so that reads a size out of the caller's old data and jumps to a random
address. The fix is to compute the address arithmetically (`tail + tail_size`)
rather than by reading a size that doesn't exist yet. The general rule: when
you're in the middle of rewriting the block structure, don't read it.

**A loop cursor that never advances.** The free-list walk in the heap checker
compares each node's `prev` against the previous node — and originally never
updated "the previous node". Every list with two or more entries reported as
corrupt. A checker that reports false failures is worse than no checker,
because you start distrusting it. Worth testing the checker itself: there are
two tests here that deliberately break the heap and assert the exact message
that comes back.
