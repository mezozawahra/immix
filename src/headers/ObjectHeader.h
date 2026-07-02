#ifndef IMMIX_OBJECTHEADER_H
#define IMMIX_OBJECTHEADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../GCTypes.h"
#include "../Constants.h"
#include "../Log.h"

typedef enum {
    object_standard = 0x1,
    object_large    = 0x2,
} ObjectType;

typedef enum {
    object_free      = 0x0,
    object_allocated = 0x1,
    object_marked    = 0x2,
} ObjectFlag;

/**
 * Sits exactly OBJ_ALIGN (8) bytes before the mutator-visible Any* pointer.
 * Layout in memory: [ObjectHeader][Any object payload...]
 * The GC only ever sees ObjectHeader*. The payload's real size and internal
 * layout are opaque - Object_Size() tells you the total allocation in bytes
 * (header + payload). Any* is your type; the GC never dereferences it.
 */
typedef struct {
    uint32_t size;  // stored as (byte_size >> OBJ_ALIGN_BITS); decode with Object_Size()
    uint8_t  type;  // ObjectType
    uint8_t  flag;  // ObjectFlag
    uint8_t  _pad;  // reserved, keep struct at 8 bytes (= OBJ_ALIGN)
} ObjectHeader;

/**
 * The GC's view of any heap allocation. `payload` is where Any* points;
 * everything before it is the ObjectHeader the GC owns. The payload itself
 * is opaque - the GC never reads it, only marks/sweeps the containing
 * lines/blocks.
 */
typedef struct {
    ObjectHeader header;
    // payload follows immediately: Any object lives here.
    // Access via Object_ToMutatorAddress()/Object_FromMutatorAddress().
} Object;

static inline bool Object_IsMarked(ObjectHeader *h)    { return h->flag == object_marked; }
static inline void Object_MarkObjectHeader(ObjectHeader *h) { h->flag = object_marked; }
static inline void Object_SetAllocated(ObjectHeader *h) { h->flag = object_allocated; }
static inline void Object_SetFree(ObjectHeader *h)      { h->flag = object_free; }
static inline bool Object_IsAllocated(ObjectHeader *h)  { return h->flag == object_allocated; }

static inline bool Object_IsStandardObject(ObjectHeader *h) { return h->type == object_standard; }
static inline bool Object_IsLargeObject(ObjectHeader *h)    { return h->type == object_large; }
static inline void Object_SetObjectType(ObjectHeader *h, ObjectType t) { h->type = t; }

static inline size_t Object_Size(ObjectHeader *h) {
    uint32_t size = h->size;
    assert((Object_IsStandardObject(h) && size < LARGE_BLOCK_SIZE) ||
           !Object_IsStandardObject(h));
    return (size_t)size << OBJ_ALIGN_BITS;
}

static inline void Object_SetSize(ObjectHeader *h, size_t size) {
    uint32_t s = (uint32_t)(size >> OBJ_ALIGN_BITS);
    assert(!Object_IsStandardObject(h) ||
           (s > 0 && s < LARGE_BLOCK_SIZE));
    h->size = s;
}

/**
 * Any* -> ObjectHeader*: step back exactly OBJ_ALIGN bytes.
 * `mutatorPtr` is the void* your runtime holds (= &payload = Any*).
 */
static inline Object *Object_FromMutatorAddress(void *mutatorPtr) {
    return (Object *)((ubyte_t *)mutatorPtr - OBJECT_HEADER_SIZE);
}

/**
 * ObjectHeader* -> Any* (the pointer your runtime stores and passes around).
 */
static inline void *Object_ToMutatorAddress(Object *object) {
    return (void *)((ubyte_t *)object + OBJECT_HEADER_SIZE);
}

#endif // IMMIX_OBJECTHEADER_H