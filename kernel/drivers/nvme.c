/* nvme.c — NVMe 1.4 controller driver (Phase 20)
 *
 * Init sequence:
 *   1. Find NVMe controller via pcie_find_device(0x01, 0x08, 0x02)
 *   2. Map BAR0 into kernel VA
 *   3. Disable controller (CC.EN=0), wait CSTS.RDY=0
 *   4. Allocate admin SQ+CQ (64 entries each, 1 page each)
 *   5. Set AQA/ASQ/ACQ, set CC.EN=1, wait CSTS.RDY=1
 *   6. Identify Controller (admin cmd 0x06, CNS=1)
 *   7. Identify Namespace (NSID=1, CNS=0)
 *   8. Create I/O CQ (admin cmd 0x05) + I/O SQ (admin cmd 0x01)
 *   9. Register blkdev_t "nvme0"
 *
 * All I/O: submit SQE -> ring doorbell -> poll CQE -> ring CQ head doorbell
 */
#include "nvme.h"
#include "arch.h"
#include "../arch/x86_64/pcie.h"
#include "../mm/vmm.h"
#include "../mm/kva.h"
#include "../mm/pmm.h"
#include "../fs/blkdev.h"
#include "../core/printk.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Static state
 * ---------------------------------------------------------------------- */

/* SAFETY: s_bar0 is set once in nvme_init() to a kernel VA for NVMe BAR0
 * MMIO registers. Declared volatile so MMIO reads are not cached by the
 * compiler and register updates are always visible. */
static volatile nvme_regs_t *s_bar0     = NULL;
static uint32_t              s_dstrd    = 0;
static uint16_t              s_cid      = 0;

/* Admin queue */
static nvme_sqe_t           *s_asq      = NULL;
/* SAFETY: s_acq is volatile so CQE phase-tag polls are not optimised away. */
static volatile nvme_cqe_t  *s_acq      = NULL;
static uint32_t              s_asq_tail = 0;
static uint32_t              s_acq_head = 0;
static uint8_t               s_acq_phase = 1;

/* I/O queue (queue ID=1) */
static nvme_sqe_t           *s_iosq      = NULL;
/* SAFETY: s_iocq is volatile so CQE phase-tag polls are not optimised away. */
static volatile nvme_cqe_t  *s_iocq      = NULL;
static uint32_t              s_iosq_tail = 0;
static uint32_t              s_iocq_head = 0;
static uint8_t               s_iocq_phase = 1;

/* Single-page bounce buffer for read/write I/O.
 * Allocated once in nvme_init; reused for all synchronous I/O.
 * Phase 20: synchronous only — no concurrent I/O. */
static void    *s_iobuf     = NULL;
static uint64_t s_iobuf_phys = 0;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Doorbell register for queue qid, tail (is_head=0) or head (is_head=1).
 * Doorbell stride: bits [55:52] of CAP register gives log2 of stride in
 * 4-byte units (NVMe 1.4 spec section 3.1.8).
 * Doorbell offset = 0x1000 + (2*qid + is_head) * (4 << s_dstrd). */
static volatile uint32_t *
doorbell(uint32_t qid, int is_head)
{
    uint32_t off = 0x1000u +
                   (2u * qid + (uint32_t)is_head) * (4u << s_dstrd);
    /* SAFETY: s_bar0 is a valid kernel VA for NVMe BAR0 MMIO, set in
     * nvme_init(). The offset arithmetic follows NVMe 1.4 spec §3.1.8. */
    return (volatile uint32_t *)((uint8_t *)s_bar0 + off);
}

/* Allocate one 4KB queue page: use kva_alloc_pages (which allocates a PMM
 * frame and maps it into kernel VA), then retrieve the physical address via
 * kva_page_phys.  The page is already mapped and accessible at the returned
 * pointer; *phys_out receives the physical address for the NVMe BAR registers.
 *
 * NOTE: these pages are never freed (no kva_free_pages call).  At Phase 20
 * scale (admin+IO queue + identify buffer = 5 pages) this is negligible. */
static void *
alloc_queue_page(uint64_t *phys_out)
{
    void *va = kva_alloc_pages(1);
    /* SAFETY: kva_alloc_pages returns a kernel VA for a PMM-allocated,
     * mapped page; zeroing via __builtin_memset is safe here. */
    __builtin_memset(va, 0, 4096);
    *phys_out = kva_page_phys(va);
    return va;
}

/* Poll for a CQE whose phase tag matches *phase.  On success, advances the
 * head pointer, flips the phase on wrap, rings the CQ head doorbell, and
 * returns 0 if the status code is 0 (success) or -1 on error.
 * Returns -1 on timeout.
 * submitted_cid: the CID embedded in the SQE cdw0[31:16]; verified against
 * the CQE cid field to detect out-of-order or spurious completions. */
static int
poll_cqe(volatile nvme_cqe_t *cq, uint32_t cq_depth,
         uint32_t *cq_head, uint8_t *phase,
         volatile uint32_t *cq_head_db,
         uint32_t qid, uint16_t submitted_cid)
{
    volatile nvme_cqe_t *entry;
    uint32_t timeout = 1000000u;
    (void)qid;

    while (timeout--) {
        entry = &cq[*cq_head];
        /* SAFETY: entry points into a kva-mapped page; volatile ensures each
         * read goes to memory so the hardware-updated phase tag is visible. */
        if (((entry->status) & 1u) == *phase) {
            /* Verify completion belongs to our submission (NVMe 1.4 §4.6). */
            if (entry->cid != submitted_cid) {
                /* Wrong CID — advance head and phase, treat as error. */
                (*cq_head)++;
                if (*cq_head >= cq_depth) {
                    *cq_head = 0;
                    *phase ^= 1u;
                }
                arch_wmb();
                *cq_head_db = *cq_head;
                return -1;
            }
            uint16_t sc = (uint16_t)((entry->status >> 1) & 0x7FFu);
            (*cq_head)++;
            if (*cq_head >= cq_depth) {
                *cq_head = 0;
                *phase ^= 1u;
            }
            /* sfence: ensure our head-pointer update is visible before
             * the doorbell write that tells the controller we consumed
             * this entry. */
            arch_wmb();
            *cq_head_db = *cq_head;
            return (sc == 0) ? 0 : -1;
        }
    }
    return -1;   /* timeout */
}

/* -------------------------------------------------------------------------
 * Admin commands
 * ---------------------------------------------------------------------- */

/* Issue an Identify command.
 *   cns=1 : Identify Controller
 *   cns=0 : Identify Namespace (nsid must be 1) */
static int
nvme_identify(uint32_t nsid, uint8_t cns, void *buf, uint64_t buf_phys)
{
    nvme_sqe_t *sqe = &s_asq[s_asq_tail];
    uint16_t    cid = s_cid++;
    (void)buf;
    __builtin_memset(sqe, 0, sizeof(*sqe));
    sqe->cdw0  = NVME_ADMIN_IDENTIFY | ((uint32_t)cid << 16);
    sqe->nsid  = nsid;
    sqe->prp1  = buf_phys;
    sqe->prp2  = 0;
    sqe->cdw10 = cns;

    s_asq_tail++;
    if (s_asq_tail >= NVME_ADMIN_QUEUE_DEPTH)
        s_asq_tail = 0;

    /* sfence: SQE must be fully written before the doorbell write. */
    arch_wmb();
    *doorbell(0, 0) = s_asq_tail;   /* admin SQ tail doorbell */

    return poll_cqe(s_acq, NVME_ADMIN_QUEUE_DEPTH,
                    &s_acq_head, &s_acq_phase,
                    doorbell(0, 1), 0, cid);
}

static int
nvme_create_io_cq(uint16_t qid, uint64_t cq_phys, uint16_t depth)
{
    nvme_sqe_t *sqe = &s_asq[s_asq_tail];
    uint16_t    cid = s_cid++;
    __builtin_memset(sqe, 0, sizeof(*sqe));
    sqe->cdw0  = NVME_ADMIN_CREATE_IO_CQ | ((uint32_t)cid << 16);
    sqe->prp1  = cq_phys;
    sqe->cdw10 = ((uint32_t)(depth - 1u) << 16) | qid;
    sqe->cdw11 = 1u;   /* physically contiguous */

    s_asq_tail++;
    if (s_asq_tail >= NVME_ADMIN_QUEUE_DEPTH)
        s_asq_tail = 0;

    arch_wmb();
    *doorbell(0, 0) = s_asq_tail;
    return poll_cqe(s_acq, NVME_ADMIN_QUEUE_DEPTH,
                    &s_acq_head, &s_acq_phase, doorbell(0, 1), 0, cid);
}

static int
nvme_create_io_sq(uint16_t qid, uint64_t sq_phys, uint16_t depth,
                  uint16_t cqid)
{
    nvme_sqe_t *sqe = &s_asq[s_asq_tail];
    uint16_t    cid = s_cid++;
    __builtin_memset(sqe, 0, sizeof(*sqe));
    sqe->cdw0  = NVME_ADMIN_CREATE_IO_SQ | ((uint32_t)cid << 16);
    sqe->prp1  = sq_phys;
    sqe->cdw10 = ((uint32_t)(depth - 1u) << 16) | qid;
    sqe->cdw11 = ((uint32_t)cqid << 16) | 1u;  /* cqid | physically contiguous */

    s_asq_tail++;
    if (s_asq_tail >= NVME_ADMIN_QUEUE_DEPTH)
        s_asq_tail = 0;

    arch_wmb();
    *doorbell(0, 0) = s_asq_tail;
    return poll_cqe(s_acq, NVME_ADMIN_QUEUE_DEPTH,
                    &s_acq_head, &s_acq_phase, doorbell(0, 1), 0, cid);
}

/* -------------------------------------------------------------------------
 * blkdev read/write callbacks — forward declarations
 * ---------------------------------------------------------------------- */
static int nvme_blkdev_read(struct blkdev *dev, uint64_t lba, uint32_t count,
                            void *buf);
static int nvme_blkdev_write(struct blkdev *dev, uint64_t lba, uint32_t count,
                             const void *buf);

/* -------------------------------------------------------------------------
 * nvme_init
 * ---------------------------------------------------------------------- */

void
nvme_init(void)
{
    uint32_t i;
    uint32_t timeout;

    /* Step 1: Find NVMe controller via PCIe
     * class=0x01 (storage), subclass=0x08 (NVM), progif=0x02 (NVMe) */
    const pcie_device_t *dev = pcie_find_device(0x01, 0x08, 0x02);
    if (dev == NULL) {
        /* No NVMe controller present — silent return, no boot.txt line */
        return;
    }

    /* Step 2: Map BAR0 into kernel VA.
     * NVMe BAR0 is at minimum 16KB; map 4 pages (16KB) to be safe.
     * kva_alloc_pages allocates PMM frames and maps them — we then overwrite
     * those mappings to point at the actual MMIO physical address range with
     * no-cache flags (PWT+PCD). The PMM frames allocated by kva_alloc_pages
     * are leaked; at one NVMe controller this is an acceptable Phase 20 cost. */
    {
        uint64_t  bar0_phys  = dev->bar[0];
        uint32_t  bar0_pages = 4u;
        uintptr_t bar0_va    = (uintptr_t)kva_alloc_pages(bar0_pages);
        for (i = 0; i < bar0_pages; i++) {
            uintptr_t va = bar0_va + (uintptr_t)i * 4096u;
            /* kva_alloc_pages mapped each page to a PMM frame; unmap first
             * so vmm_map_page does not panic on a double-map.
             * SAFETY: va is a kva-allocated page that is present in the PT
             * (kva_alloc_pages guarantees this); vmm_unmap_page succeeds. */
            vmm_unmap_page(va);
            /* SAFETY: BAR0 is MMIO — map uncached (Present|Write|PWT|PCD = 0x1B).
             * vmm_map_page installs the MMIO physical address at this VA;
             * the old PMM frame is leaked (see above). */
            vmm_map_page(va, bar0_phys + (uint64_t)i * 4096u, 0x1Bu);
        }
        /* SAFETY: bar0_va is a kernel VA mapped to NVMe BAR0 MMIO registers;
         * volatile cast prevents the compiler caching register reads/writes. */
        s_bar0 = (volatile nvme_regs_t *)bar0_va;
    }

    /* Extract doorbell stride from CAP[55:52] */
    s_dstrd = (uint32_t)((s_bar0->cap >> 32) & 0xFu);

    /* Step 3: Disable controller — write CC.EN=0, wait CSTS.RDY=0 */
    s_bar0->cc = 0u;
    timeout = 500000u;
    while ((s_bar0->csts & NVME_CSTS_RDY) && timeout--)
        ;
    if (s_bar0->csts & NVME_CSTS_RDY) {
        printk("[NVME] FAIL: controller did not disable\n");
        return;
    }

    /* Step 4: Allocate admin SQ and CQ (1 page each = 64 entries) */
    {
        uint64_t asq_phys, acq_phys;
        s_asq = (nvme_sqe_t *)alloc_queue_page(&asq_phys);
        s_acq = (volatile nvme_cqe_t *)alloc_queue_page(&acq_phys);
        s_asq_tail  = 0;
        s_acq_head  = 0;
        s_acq_phase = 1u;   /* initial expected phase tag */
        s_cid       = 0;

        /* Step 5: Program AQA, ASQ, ACQ then enable the controller */
        s_bar0->aqa = ((uint32_t)(NVME_ADMIN_QUEUE_DEPTH - 1u) << 16) |
                      (uint32_t)(NVME_ADMIN_QUEUE_DEPTH - 1u);
        s_bar0->asq = asq_phys;
        s_bar0->acq = acq_phys;
        s_bar0->cc  = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
                      NVME_CC_IOSQES | NVME_CC_IOCQES;
    }

    /* Wait for CSTS.RDY=1 */
    timeout = 500000u;
    while (!(s_bar0->csts & NVME_CSTS_RDY) && timeout--)
        ;
    if (!(s_bar0->csts & NVME_CSTS_RDY)) {
        printk("[NVME] FAIL: controller did not become ready\n");
        return;
    }

    /* Step 6: Identify Controller (CNS=1, NSID=0) */
    {
        uint64_t id_phys;
        uint8_t *id_buf = (uint8_t *)alloc_queue_page(&id_phys);
        if (nvme_identify(0u, 1u, id_buf, id_phys) != 0) {
            printk("[NVME] FAIL: Identify Controller failed\n");
            return;
        }
        /* Model name at bytes [24,63] — space-padded; not printed for now */
    }

    /* Step 7: Identify Namespace NSID=1 (CNS=0) */
    {
        uint64_t  id_phys;
        uint8_t  *id_buf = (uint8_t *)alloc_queue_page(&id_phys);
        uint64_t  nsze;
        uint8_t   flbas;
        uint32_t  lbaf_entry;
        uint32_t  lbads;
        uint32_t  lba_size;

        if (nvme_identify(1u, 0u, id_buf, id_phys) != 0) {
            printk("[NVME] FAIL: Identify Namespace failed\n");
            return;
        }

        /* NSZE (offset 0): namespace size in logical blocks */
        __builtin_memcpy(&nsze, id_buf + 0, 8u);
        /* FLBAS (offset 26): lower 4 bits = active LBA format index */
        flbas = id_buf[26] & 0x0Fu;
        /* LBAF[flbas] (offset 128 + flbas*4): bits[23:16] = log2(LBA size) */
        __builtin_memcpy(&lbaf_entry, id_buf + 128u + (uint32_t)flbas * 4u, 4u);
        lbads = (lbaf_entry >> 16) & 0xFFu;
        lba_size = (lbads >= 9u) ? (1u << lbads) : 512u;

        /* Step 8: Create I/O CQ (queue ID=1) then I/O SQ (queue ID=1) */
        {
            uint64_t iosq_phys, iocq_phys;
            s_iosq = (nvme_sqe_t *)alloc_queue_page(&iosq_phys);
            s_iocq = (volatile nvme_cqe_t *)alloc_queue_page(&iocq_phys);
            s_iosq_tail  = 0;
            s_iocq_head  = 0;
            s_iocq_phase = 1u;

            if (nvme_create_io_cq(1u, iocq_phys,
                                  (uint16_t)NVME_IO_QUEUE_DEPTH) != 0) {
                printk("[NVME] FAIL: Create I/O CQ failed\n");
                return;
            }
            if (nvme_create_io_sq(1u, iosq_phys,
                                  (uint16_t)NVME_IO_QUEUE_DEPTH, 1u) != 0) {
                printk("[NVME] FAIL: Create I/O SQ failed\n");
                return;
            }
        }

        /* Allocate bounce buffer for synchronous read/write I/O */
        s_iobuf = alloc_queue_page(&s_iobuf_phys);

        /* Step 9: Register blkdev */
        {
            static blkdev_t s_nvme0;
            static const char s_nvme0_name[] = "nvme0";
            __builtin_memcpy(s_nvme0.name, s_nvme0_name,
                             sizeof(s_nvme0_name));
            s_nvme0.block_count = nsze;
            s_nvme0.block_size  = lba_size;
            s_nvme0.lba_offset  = 0;
            s_nvme0.read        = nvme_blkdev_read;
            s_nvme0.write       = nvme_blkdev_write;
            s_nvme0.priv        = NULL;
            blkdev_register(&s_nvme0);
        }

        printk("[NVME] OK: nvme0 %lu sectors x %u bytes\n",
               (unsigned long)nsze, (unsigned)lba_size);
    }
}

/* -------------------------------------------------------------------------
 * blkdev read/write callbacks
 * ---------------------------------------------------------------------- */

/* nvme_blkdev_read — read `count` 512-byte sectors from LBA into buf.
 * Uses a shared bounce buffer allocated in nvme_init().
 * Only supports transfers that fit in one 4KB page (count * 512 <= 4096). */
static int
nvme_blkdev_read(struct blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    uint32_t   bytes = count * 512u;   /* assume 512-byte sectors */
    void      *tmp;
    nvme_sqe_t *sqe;
    int         rc;

    (void)dev;
    if (bytes > 4096u)
        return -1;   /* multi-page transfers not supported in Phase 20 */

    /* Bounce buffer: shared single page allocated in nvme_init() */
    tmp = s_iobuf;
    uint64_t buf_phys = s_iobuf_phys;

    {
    uint16_t cid = s_cid++;
    sqe = &s_iosq[s_iosq_tail];
    __builtin_memset(sqe, 0, sizeof(*sqe));
    sqe->cdw0  = NVME_IO_READ | ((uint32_t)cid << 16);
    sqe->nsid  = 1u;
    sqe->prp1  = buf_phys;
    sqe->prp2  = 0u;
    sqe->cdw10 = (uint32_t)(lba & 0xFFFFFFFFu);
    sqe->cdw11 = (uint32_t)(lba >> 32);
    sqe->cdw12 = count - 1u;   /* NLB: 0-based count */

    s_iosq_tail++;
    if (s_iosq_tail >= NVME_IO_QUEUE_DEPTH)
        s_iosq_tail = 0;

    /* sfence: SQE must be fully written to memory before doorbell write. */
    arch_wmb();
    *doorbell(1u, 0) = s_iosq_tail;

    rc = poll_cqe(s_iocq, NVME_IO_QUEUE_DEPTH,
                  &s_iocq_head, &s_iocq_phase,
                  doorbell(1u, 1), 1u, cid);
    }
    if (rc == 0)
        __builtin_memcpy(buf, tmp, bytes);
    return rc;
}

/* nvme_blkdev_write — write `count` 512-byte sectors from buf to LBA.
 * Uses a shared bounce buffer allocated in nvme_init().
 * Only supports transfers that fit in one 4KB page (count * 512 <= 4096). */
static int
nvme_blkdev_write(struct blkdev *dev, uint64_t lba, uint32_t count,
                  const void *buf)
{
    uint32_t   bytes = count * 512u;
    void      *tmp;
    nvme_sqe_t *sqe;

    (void)dev;
    if (bytes > 4096u)
        return -1;

    /* Bounce buffer: shared single page allocated in nvme_init() */
    tmp = s_iobuf;
    uint64_t buf_phys = s_iobuf_phys;
    __builtin_memcpy(tmp, buf, bytes);

    {
    uint16_t cid = s_cid++;
    sqe = &s_iosq[s_iosq_tail];
    __builtin_memset(sqe, 0, sizeof(*sqe));
    sqe->cdw0  = NVME_IO_WRITE | ((uint32_t)cid << 16);
    sqe->nsid  = 1u;
    sqe->prp1  = buf_phys;
    sqe->prp2  = 0u;
    sqe->cdw10 = (uint32_t)(lba & 0xFFFFFFFFu);
    sqe->cdw11 = (uint32_t)(lba >> 32);
    sqe->cdw12 = count - 1u;   /* NLB: 0-based count */

    s_iosq_tail++;
    if (s_iosq_tail >= NVME_IO_QUEUE_DEPTH)
        s_iosq_tail = 0;

    /* sfence: SQE must be fully written to memory before doorbell write. */
    arch_wmb();
    *doorbell(1u, 0) = s_iosq_tail;

    return poll_cqe(s_iocq, NVME_IO_QUEUE_DEPTH,
                    &s_iocq_head, &s_iocq_phase,
                    doorbell(1u, 1), 1u, cid);
    }
}
