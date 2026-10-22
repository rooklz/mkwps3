/* ax_spu_job.h -- what the PPU and the SPU audio mixer (ax_spu.c) pass each other.
 *
 * One AX frame's voices per job. The PPU fills the job line and sends the SPU the job's
 * sequence number through its inbound mailbox; the SPU mixes into the bus buffers, writes
 * the PBs and buses back, and then posts the result line with the same number. Both lines
 * are 128 bytes and 128-aligned, so each is one DMA and the two sides never share one. */
#ifndef AX_SPU_JOB_H
#define AX_SPU_JOB_H

#include <stdint.h>

#define AX_SPU_NEW_FILTER   1u   /* the ucode runs the biquad (UCodeUsesNewFilter) */
#define AX_SPU_COEFFS_FIT32 2u   /* every polyphase phase sums to <= 0xffff: 32-bit taps */
#define AX_SPU_CHECK_VECTOR 4u   /* also run the scalar kernels and count differences */

#define AX_SPU_HELLO 0xA0D10001u /* result.voices before the first job: the SPU is up */

typedef struct {
    uint64_t mem1_ea;      /* host address of guest MEM1 (24 MB) */
    uint64_t mem2_ea;      /* host address of guest MEM2 (64 MB) */
    uint64_t bus_ea;       /* 12 regular buses of 96 int32, in AxBus order */
    uint64_t wmbus_ea;     /* 8 wiimote buses of 18 int32 */
    uint64_t coef_ea;      /* the 0x800 polyphase coefficients, or 0 for none */
    uint64_t result_ea;    /* the AxSpuResult line */
    uint32_t pb_addr;      /* guest address of the first PB */
    uint32_t seq;
    uint32_t flags;        /* AX_SPU_* */
    uint32_t nseg;         /* PB layout: runs of {guest offset, struct offset, bytes} */
    uint16_t seg[3][3];
    uint8_t pad[128 - 64 - 18];
} __attribute__((aligned(128))) AxSpuJob;

typedef struct {
    uint32_t seq;          /* the job this answers */
    uint32_t voices;       /* voices mixed */
    uint32_t pbs;          /* PBs walked */
    uint32_t ticks;        /* SPU decrementer ticks (79.8 MHz) the job took */
    uint32_t bad;          /* guest accesses outside MEM1/MEM2, skipped */
    uint32_t wait_pb;      /* of `ticks`, waiting for PB reads */
    uint32_t wait_smp;     /* of `ticks`, waiting for sample reads */
    uint32_t vector_mismatches; /* with AX_SPU_CHECK_VECTOR: vector kernel calls that differed */
    uint32_t pad[24];
} __attribute__((aligned(128))) AxSpuResult;

#endif
