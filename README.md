# myalloc — a memory allocator in C++17

A working replacement for `malloc`, `free`, `calloc` and `realloc`, built on
`mmap`. It has boundary tags, segregated free lists, O(1) merging in both
directions, a heap checker, and instrumentation that keeps *mapped* and
*resident* memory as the separate things they are.

About 1,000 lines of code. Every design decision is written down as a question
and an answer in **[DESIGN.md](DESIGN.md)**.

```
  my_malloc(100)
        |
        v
  +----------------------------------------------------+
  |  a free block big enough?                           |
  |     yes -> take it, cut off what we don't need      |
  |     no  -> mmap another 1 MiB, take it from there   |
  +----------------------------------------------------+

  my_free(p)  ->  mark free, merge with either neighbour
                  that is also free, put it in a free list
```

---

## Files

| Path | Lines | What it is |
|---|---|---|
| `include/myalloc.h` | ~90 | The whole public API |
| `src/block.h` | ~140 | What a block looks like, and the accessors for it |
| `src/internal.h` | ~60 | Chunk layout and the shared state |
| `src/allocator.cpp` | ~420 | Free lists, growth, split, merge, the four functions |
| `src/heapcheck.cpp` | ~150 | Every invariant, checked |
| `tests/test_allocator.cpp` | ~420 | 23 tests |
| `cli/main.cpp` | ~290 | Workloads, stats, and timings against system `malloc` |

Read them in that order and the whole thing makes sense in one sitting.

---

## Build and run

Needs a C++17 compiler, CMake 3.16+, and Linux (`mmap`, `/proc/self/statm`,
`getrusage`).

```sh
sudo apt-get install -y libgtest-dev      # optional; CMake fetches it otherwise

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Tests:

```sh
ctest --test-dir build --output-on-failure
```

Try it out:

```sh
./build/myalloc_cli mixed 200000   # random sizes, random free order
./build/myalloc_cli frag  20000    # deliberately fragmenting workload
./build/myalloc_cli paging         # mapped vs resident memory
./build/myalloc_cli bench          # timings, next to the system malloc
```

With UndefinedBehaviorSanitizer (what CI does):

```sh
cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug -DMYALLOC_ENABLE_UBSAN=ON
cmake --build build-ubsan -j && ./build-ubsan/myalloc_tests
```

The project is built under `-Wall -Wextra -Wpedantic`; add
`-DMYALLOC_WERROR=ON` to make warnings fatal.

---

## AddressSanitizer can't test this, and that's expected

ASan works by replacing `malloc` and `free` with instrumented versions that
wrap every allocation in poisoned bytes. It has no idea this library carves
blocks out of an `mmap`'d region — to ASan that whole region is one big valid
allocation, so writing past the end of a block we handed out lands inside the
same mapping and ASan sees nothing wrong.

`my_heap_check()` is the replacement. It walks every block and every free list
and checks that the tags agree, the blocks tile each chunk with no gaps or
overlaps, no two free blocks are touching, and every free block is in exactly
one list. A one-byte overflow shows up as *"header and footer disagree"* — and
there is a test that causes exactly that and asserts the message.

UBSan *is* useful here (alignment, integer overflow) so CI runs it.

---

## Results

Every number below is a placeholder. Run it on your own machine and fill them
in — allocator numbers mean nothing without the hardware and libc they came
from.

Machine: `[MEASURE]` · Kernel: `[MEASURE]` · glibc: `[MEASURE]` ·
Compiler: `[MEASURE]` · Build: Release

### Speed — `./build/myalloc_cli bench`

| Workload | myalloc | system malloc | ratio |
|---|---|---|---|
| Small fixed 64 B, alloc + free | `[MEASURE]` ns | `[MEASURE]` ns | `[MEASURE]`x |
| Random sizes 1–4096 B | `[MEASURE]` ns | `[MEASURE]` ns | `[MEASURE]`x |
| Allocate 100k, then free them all | `[MEASURE]` ns | `[MEASURE]` ns | `[MEASURE]`x |
| Allocate 100k, free newest first | `[MEASURE]` ns | `[MEASURE]` ns | `[MEASURE]`x |

### Fragmentation — `./build/myalloc_cli frag 20000`

| Stage | free blocks | largest free block | external frag |
|---|---|---|---|
| Packed with 128 B blocks | `[MEASURE]` | `[MEASURE]` | `[MEASURE]` % |
| After freeing every other one | `[MEASURE]` | `[MEASURE]` | `[MEASURE]` % |
| After asking for 256 B blocks | `[MEASURE]` | `[MEASURE]` | `[MEASURE]` % |

### Mapped vs resident — `./build/myalloc_cli paging`

| Stage | mapped | resident (RSS) | minor faults |
|---|---|---|---|
| Before allocating | 0 | `[MEASURE]` | `[MEASURE]` |
| After `my_malloc(256 MiB)`, untouched | `[MEASURE]` | `[MEASURE]` | `[MEASURE]` |
| After touching 25 % | `[MEASURE]` | `[MEASURE]` | `[MEASURE]` |
| After touching 50 % | `[MEASURE]` | `[MEASURE]` | `[MEASURE]` |
| After touching 100 % | `[MEASURE]` | `[MEASURE]` | `[MEASURE]` |

The second row is the interesting one: the address space is fully mapped and
almost none of it is real memory yet. Pages arrive one page fault at a time, on
first touch. DESIGN.md Q12 explains it properly.

### Overhead

| | |
|---|---|
| Metadata per block | 24 B (16 B header + 8 B footer) |
| Smallest block | 48 B |
| Alignment of every pointer returned | 16 B |
| Fixed overhead per 1 MiB chunk | 64 B |

---

## Where this loses to glibc, and why

Worth being precise about rather than vague.

**Small allocations, single-threaded.** glibc keeps a small per-thread cache
(`tcache`). A hit is: index an array, pop a pointer, return — no lock, not even
an atomic. Our fast path is: take a mutex, compute a size class, walk a list,
split, write two tags, release the mutex. The uncontended mutex alone costs
more than glibc's entire fast path.

**Multi-threaded, by a lot.** One global mutex means threads serialise
completely, and they also bounce the mutex's cache line between cores. glibc
scales; this doesn't. DESIGN.md Q19 describes what a per-thread cache would
look like here and why it isn't built.

**Memory overhead on small objects.** 24 bytes of metadata and a 48-byte
minimum, so a 16-byte request costs 48 bytes. glibc spends 8 bytes of metadata
with a 32-byte minimum.

**Memory is never returned to the OS.** The heap only grows — no `munmap` of
empty chunks, no `madvise`. glibc returns large blocks on `free`. DESIGN.md Q14
explains what it would take.

**Fragmentation under awkward workloads.** First fit inside a power-of-two
class is coarse. glibc combines exact-size bins for small sizes with a
best-fit search over sorted large bins.

**Where it's competitive:** bulk allocate-then-free patterns, where the cost is
dominated by touching memory rather than by bookkeeping. The `bench` output
shows the gap closing on those rows.

---

## Behaviour notes

- `my_free(nullptr)` does nothing, as C requires.
- `my_realloc(p, 0)` frees `p` and returns `nullptr` — glibc's behaviour. C17
  calls this implementation-defined and C23 calls it undefined, so this library
  picks one answer and writes it down.
- `my_malloc(0)` returns a real, freeable, smallest-possible block, so callers
  can't confuse "I asked for zero bytes" with "the allocator failed".
- `my_calloc` checks `count * size` for overflow using a division, so the check
  itself can't wrap.
- Freeing a foreign pointer, or freeing twice, is undefined behaviour and is
  not caught at the point of the mistake. `my_heap_check()` catches the damage.

## Licence

A learning project. Use it for anything; don't put it in production.
