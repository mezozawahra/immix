# Immix GC — internal documentation

This folder documents the existing single-mutator Immix implementation, file
by file. Each doc covers one logical module (a `.h`/`.c` pair, since in this
codebase the header rarely makes sense without the implementation next to
it). The folder structure mirrors `src/`.

| File | Covers | What it's responsible for |
|---|---|---|
| [Constants.md](Constants.md) | `Constants.h` | Every size constant and the arithmetic that derives `LINE_COUNT` etc. |
| [GCTypes.md](GCTypes.md) | `GCTypes.h` | The two base types (`word_t`, `ubyte_t`) and the `INLINE` macro |
| [headers/ObjectHeader.md](headers/ObjectHeader.md) | `headers/ObjectHeader.h` | The 8-byte per-object header, the `Rtti` type-metadata struct, the `Object` layout |
| [headers/BlockHeader.md](headers/BlockHeader.md) | `headers/BlockHeader.h` | The 32KB block's own header + line-header table, pointer-masking tricks |
| [headers/LineHeader.md](headers/LineHeader.md) | `headers/LineHeader.h` | The 1-byte-per-256-byte-line metadata and the in-place free-list node it doubles as |
| [Line.md](Line.md) | `Line.h` | Recording/looking up where the first object in a line starts |
| [datastructures/BlockList.md](datastructures/BlockList.md) | `datastructures/BlockList.{h,c}` | The singly-linked list of blocks (free / recycled), with relative-index links |
| [datastructures/Stack.md](datastructures/Stack.md) | `datastructures/Stack.{h,c}` | The GC's mark-phase worklist (**not** the native call stack) |
| [datastructures/Bitmap.md](datastructures/Bitmap.md) | `datastructures/Bitmap.{h,c}` | Bit-per-chunk tracking used only by the large-object allocator |
| [utils/MathUtils.md](utils/MathUtils.md) | `utils/MathUtils.h` | `log2_floor`, ceil-log2, round-up-to-multiple |
| [Allocator.md](Allocator.md) | `Allocator.{h,c}` | The bump allocator: normal path, **overflow allocation**, line/block acquisition |
| [Block.md](Block.md) | `Block.{h,c}` | Turning a just-marked block back into free lines/blocks (`Block_Recycle`) |
| [LargeAllocator.md](LargeAllocator.md) | `LargeAllocator.{h,c}` | The separate buddy-style heap for objects ≥ 8KB |
| [Heap.md](Heap.md) | `Heap.{h,c}` | Top-level dispatch (small vs. large), growth, the collection cycle |
| [Marker.md](Marker.md) | `Marker.{h,c}` | Root discovery (conservative stack scan + precise module scan) and tracing |
| [StackoverflowHandler.md](StackoverflowHandler.md) | `StackoverflowHandler.{h,c}` | Recovery when the mark worklist fills up mid-trace |
| [State.md](State.md) | `State.{h,c}` | The global singletons everything above shares |
| [Object.md](Object.md) | `Object.{h,c}` | Resolving a raw address to its containing object (conservative pointer fix-up), marking |
| [ImmixGC.md](ImmixGC.md) | `ImmixGC.c` | The actual C ABI: `scalanative_init/alloc/collect` |

## The allocation path, top to bottom

1. Mutator code calls `scalanative_alloc(info, size, isObjectArray)` ([ImmixGC.md](ImmixGC.md)).
2. `Heap_Alloc` ([Heap.md](Heap.md)) looks at `size` and routes to either:
   - `Heap_AllocSmall` → `Allocator_Alloc` ([Allocator.md](Allocator.md)) — the bump-pointer fast path, which itself may fall into **overflow allocation** for objects bigger than one line; or
   - `Heap_AllocLarge` → `LargeAllocator_GetBlock` ([LargeAllocator.md](LargeAllocator.md)) — a power-of-two buddy-style allocator, for objects ≥ `LARGE_BLOCK_SIZE` (8KB).
3. If either fails (no space), `Heap_Collect` runs:
   - `Marker_MarkRoots` ([Marker.md](Marker.md)) conservatively scans the native call stack, then precisely scans Scala Native's global "modules", then drains the mark worklist (a `Stack`, see [datastructures/Stack.md](datastructures/Stack.md)) — tracing every reachable object via its `Rtti` pointer-map ([headers/ObjectHeader.md](headers/ObjectHeader.md)).
   - If the worklist fills up mid-trace, [StackoverflowHandler.md](StackoverflowHandler.md) repairs the damage afterwards with a full heap re-scan.
   - `Heap_Recycle` walks every block and calls `Block_Recycle` ([Block.md](Block.md)) to turn dead lines back into holes (and dead blocks back into free blocks), then sweeps the large heap, then decides whether to grow.
4. Allocation is retried; if it still fails, the heap grows and it's retried once more.

## Two naming collisions worth knowing before you read the rest

- **There are two unrelated things called "large" in `Allocator.h`.** `Allocator->largeBlock/largeCursor/largeLimit` is the **overflow** bump-pointer pair used for medium-sized objects (bigger than one line, but still well under 8KB) — it has nothing to do with `LargeAllocator`, which is a completely separate heap/struct for objects ≥ 8KB. See [Allocator.md](Allocator.md)'s note on this.
- **There are two unrelated things called "stack."** `State.h`'s global `Stack *stack` is the GC's mark-phase worklist (a flat array of `Object*`, see [datastructures/Stack.md](datastructures/Stack.md)) — it is *not* the native call stack that `Marker_markProgramStack` scans for conservative roots.
