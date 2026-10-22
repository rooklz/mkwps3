/* gxf_shared.h -- what the GPU thread and the SPU front end (gxf_spu.c) pass each other.
 *
 * The SPU parses the GX command stream the game writes into the ring, display lists
 * included, keeps its own copy of the CP, BP and XF registers, and drops every write
 * that would leave a register as it was (most of them: titles re-send whole material
 * blocks per draw). What is left goes to the GPU thread as records in a second ring,
 * in stream order, and the GPU thread replays them through the same sink the PPU
 * parser feeds. Indexed XF loads arrive already resolved into XF writes.
 *
 * Records are 32-bit words: a header with the operation in the top byte, then its
 * operands. They never straddle the end of the record ring; GXF_WRAP sends the reader
 * back to the start. */
#ifndef GXF_SHARED_H
#define GXF_SHARED_H

#include <stdint.h>

#define GXF_NOP      0x00u   /* one word of padding */
#define GXF_CP       0x01u   /* aux reg; value */
#define GXF_XF       0x02u   /* aux addr | (count-1) << 16; count values */
#define GXF_BP       0x03u   /* word (register in its top byte, as in the stream) */
#define GXF_DRAW     0x04u   /* aux prim << 8 | vat; count, guest address of the vertices, vertex size */
#define GXF_RUN      0x05u   /* aux vat; guest address, bytes, prims, vertices, indices, vertex size */
#define GXF_POS      0x06u   /* ring position hi, lo: everything before it is parsed */
#define GXF_WRAPCOPY 0x07u   /* aux bytes: copy the ring's last `aux` bytes to just below its start */
#define GXF_INDEX    0x08u   /* aux op; index << 16 | xf address, count: an indexed load the SPU couldn't read */
#define GXF_WRAP     0xFFu   /* the rest of the ring is unused: carry on from its start */

#define GXF_HDR(op, aux) (((uint32_t)(op) << 24) | ((uint32_t)(aux) & 0x00FFFFFFu))

#define GXF_HELLO 0x67786601u

/* Three 128-byte lines: what the GPU thread sets up, what the game thread updates, and
 * what the SPU reports. No line has two writers. */
typedef struct {
    /* set once before the SPU starts */
    uint64_t ring_ea;       /* host address of ring byte 0 */
    uint64_t arena_ea;      /* host address of guest fold offset 0 */
    uint64_t out_ea;        /* the record ring */
    uint64_t ppe_ea;        /* the ppe line below, for the SPU to poll */
    uint64_t spu_ea;        /* the spu line below */
    uint32_t ring_bytes;
    uint32_t ring_guest;    /* guest address of ring byte 0 */
    uint32_t out_bytes;     /* a multiple of 16 */
    uint32_t flags;
    uint8_t pad0[128 - 56];

    /* the PPE's line: the game thread's publish position, the GPU thread's read position */
    volatile uint64_t written;
    volatile uint32_t out_tail;   /* bytes, wrapping at 2^32 */
    volatile uint32_t quit;
    uint8_t pad1[128 - 16];

    /* the SPU's line */
    volatile uint64_t seen;       /* the publish position the SPU has parsed up to */
    volatile uint32_t out_head;   /* bytes, wrapping at 2^32 */
    volatile uint32_t hello;
    volatile uint32_t unknown;    /* unknown opcodes */
    volatile uint32_t lists;      /* display lists called */
    volatile uint32_t dropped;    /* writes that changed nothing */
    volatile uint32_t kept;       /* register writes passed on */
    volatile uint32_t idx_loads;  /* indexed loads resolved */
    volatile uint32_t idx_misses; /* of those, needing a DMA */
    volatile uint32_t busy;       /* decrementer ticks spent parsing */
    volatile uint32_t stall;      /* decrementer ticks waiting for room in the record ring */
    uint8_t pad2[128 - 48];
} __attribute__((aligned(128))) GxfControl;

#endif
