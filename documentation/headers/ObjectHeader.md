# `headers/ObjectHeader.h`

This is the file most directly relevant to the existing "polymorphic `Any`
with a `type_meta*`" design — `Rtti` here *is* the type-metadata pointer,
and `Object` here *is* the generic object shape every concrete type's
instance is laid out as an extension of.

## `ObjectType` — which heap owns this object

```c
typedef enum {
    object_standard = 0x1,
    object_large = 0x2,
} ObjectType;
```

`object_standard` means "lives inside a normal 32KB Immix block, addressed
line-wise." `object_large` means "lives in the separate `LargeAllocator`
heap as its own buddy-style chunk" (see [LargeAllocator.md](../LargeAllocator.md)).
This is a per-object tag, not a per-allocation-site decision baked into the
type — `Heap_Alloc` decides which heap to use purely from the requested
*size*, then stamps the resulting object with the matching tag.

## `ObjectFlag` — lifecycle state

```c
typedef enum {
    object_free = 0x0,
    object_allocated = 0x1,
    object_marked = 0x2,
} ObjectFlag;
```

One byte, three states, reused every collection cycle — there's no separate
"this object existed before the last GC" bit. The state machine across one
collection is:

```
allocated  --[Marker_Mark finds it reachable]-->  marked
allocated  --[never visited during marking]-->     (stays allocated, but
                                                     is actually garbage —
                                                     resolved during sweep)
marked     --[Block_Recycle / LargeAllocator_Sweep]--> allocated   (survivor)
allocated* --[Block_Recycle / LargeAllocator_Sweep]--> free        (garbage)
```

So `object_allocated` is ambiguous *during* the mark phase (it means
"either still alive from before, or dead and just hasn't been swept yet")
and only becomes unambiguous again once the sweep (`Block_Recycle`,
`LargeAllocator_Sweep`) has run. See [Block.md](../Block.md) for exactly
where that resolution happens.

## `ObjectHeader` — the 8 bytes that sit before every object

```c
typedef struct {
    uint32_t size;
    uint8_t type;
    uint8_t flag;
    uint8_t isObjectArray;
} ObjectHeader;
```

This is exactly `OBJECT_HEADER_SIZE` (8 bytes, see
[Constants.md](../Constants.md)) — `4 + 1 + 1 + 1 = 7` bytes of fields plus
1 byte of padding to keep the struct word-aligned.

- **`size`** — not stored in bytes. `Object_SetSize`/`Object_Size` shift by
  `WORD_SIZE_BITS` (3) on the way in/out, so this field actually holds
  "size in 8-byte words." That's deliberate compression: it lets a 32-bit
  field represent object sizes up to `4G * 8 bytes = 32GB` instead of 4GB,
  at the cost of objects always being a whole number of words (which they
  already have to be, since every field is word-sized). For `object_standard`
  objects there's an additional assert that the *byte* size stays under
  `LARGE_BLOCK_SIZE` (8KB) — consistent with the small/large heap split.
- **`type`** — an `ObjectType`.
- **`flag`** — an `ObjectFlag`.
- **`isObjectArray`** — `0` or `1`. This is the single bit that decides
  which of the two scanning strategies `Marker_Mark` uses for this object
  (see below, and [Marker.md](../Marker.md)): scan every field as a
  reference (object arrays — every slot is guaranteed to be a GC pointer,
  no per-field type information needed), or consult `Rtti.refMapStruct` to
  find out which specific fields are references (everything else, where
  fields can be a mix of references and raw data like ints/doubles).

## `Rtti` — the type metadata (`type_meta*`)

```c
typedef struct {
    struct { int32_t id; word_t *name; int8_t kind; } rt;
    int64_t size;
    struct { int32_t from; int32_t to; } range;
    struct {
        int32_t dyn_method_count;
        word_t *dyn_method_salt;
        word_t *dyn_method_keys;
        word_t *dyn_methods;
    } dynDispatchTable;
    int64_t *refMapStruct;
} Rtti;
```

This is shared, read-only, one-per-*type* metadata (not per-instance) —
every instance of the same concrete type points at the same `Rtti`.

- **`rt.{id,name,kind}`** — type identity: a numeric id, a pointer to the
  type's name, and a `kind` discriminator (this codebase doesn't define
  what the `kind` values mean — that's owned by whatever generates these
  tables; the GC itself never reads `rt` at all, it's bookkeeping for the
  embedding language's reflection/runtime-type-checking, just carried
  alongside).
- **`size`** — the type's *instance* size. (Not used by the GC directly
  either — by the time an object exists, its own `ObjectHeader.size` is
  already set; this is presumably read by allocation call sites before
  they know the instance's `ObjectHeader` even exists yet.)
- **`range`** — from/to markers into some larger external table (e.g. a
  global class hierarchy or type-id range for `isInstanceOf`-style checks).
  Not read by the GC.
- **`dynDispatchTable`** — virtual/dynamic method dispatch metadata (a
  salted hash table mapping method keys to method pointers, by the look of
  the four fields). Entirely a language-runtime concern; the GC never
  touches it.
- **`refMapStruct`** — **this is the one field the GC actually reads.** It's
  a pointer to an `int64_t` array: a list of *field offsets* (in
  `Field_t`/word units, indexing into `Object.fields[]`) that hold GC
  references, terminated by the sentinel `LAST_FIELD_OFFSET` (`-1`, defined
  locally in `Marker.c`/`StackoverflowHandler.c`). `Marker_Mark` walks this
  array exactly once per non-array object visited, instead of having to
  know anything about the type's actual field layout beyond "trust this
  list."

## `Object` — the generic instance layout

```c
typedef word_t *Field_t;

typedef struct {
    ObjectHeader header;
    Rtti *rtti;
    Field_t fields[0];
} Object;
```

This is the struct that every concrete type's instance is laid out as a
prefix of: 8-byte header, then the `Rtti*` (the `type_meta*` pointer),
then a flexible array of fields whose actual length depends on the
concrete type and is never known to this struct definition itself — `fields[0]`
is the classic C "variable-length trailing array" idiom. A concrete type
with, say, three reference fields and an int field is really `ObjectHeader
+ Rtti* + 4 words`, but the GC only ever needs to know about it through
this generic `Object*` view plus `rtti->refMapStruct` telling it which of
those trailing words are references.

## Mutator-visible address vs. real object address

```c
static inline Object *Object_FromMutatorAddress(word_t *address) {
    return (Object *)(address - WORDS_IN_OBJECT_HEADER);
}
static inline word_t *Object_ToMutatorAddress(Object *object) {
    return (word_t *)&object->rtti;
}
```

The pointer that allocation returns to the mutator (and that the mutator
stores in locals/registers/other objects' fields) is **not** the address of
the `Object` struct — it's `&object->rtti`, i.e. the header is hidden one
word *before* what the mutator sees. `WORDS_IN_OBJECT_HEADER` is `1` (8
bytes / 8-byte words). Anywhere the GC is handed a mutator-visible pointer
and needs the real object/header, it goes through
`Object_FromMutatorAddress` first; anywhere it has the real `Object*` and
needs to hand an address back out to mutator code, it goes through
`Object_ToMutatorAddress`. Getting this backwards is the single easiest way
to corrupt a header, so every accessor in this codebase is consistent about
which "kind" of pointer it expects.

## Everything else in the file

The rest are small accessor functions for the bit/byte fields above
(`Object_IsMarked`, `Object_MarkObjectHeader`, `Object_SetAllocated`,
`Object_SetFree`, `Object_IsAllocated`, `Object_IsStandardObject`,
`Object_IsLargeObject`, `Object_SetObjectType`, `Object_IsObjectArray`,
`Object_SetObjectArray`) plus `Object_Size`/`Object_SetSize` (covered
above). None of them have logic beyond a direct field read/write/compare —
they exist so that every other file talks to the header through named
accessors instead of poking `.flag`/`.type` directly.
