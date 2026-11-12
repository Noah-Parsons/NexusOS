/* vmm.c
 *
 * x86-64 4-level paging:
 *
 *   virtual address bits     table
 *     47..39   PML4 index
 *     38..30   PDPT index
 *     29..21   PD   index
 *     20..12   PT   index
 *     11..0    offset within 4 KiB page
 *
 * Each table is 512 * 8 bytes = 4 KiB. Non-leaf entries store the
 * physical address of the next table in bits 51..12, with PRESENT and
 * WRITE flags. Leaf entries store the physical page in the same field.
 * If PTE_HUGE is set in a PDPT or PD entry the entry is itself the
 * leaf (1 GiB or 2 MiB page respectively).
 *
 */

#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "io.h"
#include "string.h"

#define ENTRIES_PER_TABLE   512
#define PAGE_MASK           0x000FFFFFFFFFF000ull   /* bits 51..12 */

/* Framebuffer handoff descriptor from the bootloader (see boot.c and fb.c).
 * vmm_init has to know about it: the on-screen console writes to the LFB, and
 * the instruction after write_cr3() had better still be able to reach it. */
#define FB_INFO_ADDR        0x9700

static uint64_t *kernel_pml4;   /* virtual == physical thanks to identity map */

static inline uint64_t pml4_index(uintptr_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t pdpt_index(uintptr_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t pd_index  (uintptr_t v) { return (v >> 21) & 0x1FF; }
static inline uint64_t pt_index  (uintptr_t v) { return (v >> 12) & 0x1FF; }

static uint64_t *alloc_table(void) {
    uintptr_t pa = pmm_alloc_frame();
    if (!pa) return NULL;
    uint64_t *t = (uint64_t *)pa;       /* identity-mapped, so PA == VA */
    memset(t, 0, 4096);
    return t;
}

static uint64_t *next_table(uint64_t entry) {
    if (!(entry & PTE_PRESENT)) return NULL;
    if (entry & PTE_HUGE)       return NULL;       /* leaf at this level */
    return (uint64_t *)(uintptr_t)(entry & PAGE_MASK);
}

/* Install one 2 MiB leaf at `addr`, creating the tables above it on demand. */
static bool map_2mb(uint64_t *pml4, uint64_t addr, uint64_t flags) {
    uint64_t i4 = pml4_index((uintptr_t)addr);
    uint64_t i3 = pdpt_index((uintptr_t)addr);
    uint64_t i2 = pd_index((uintptr_t)addr);

    uint64_t *pdpt;
    if (!(pml4[i4] & PTE_PRESENT)) {
        pdpt = alloc_table();
        if (!pdpt) return false;
        pml4[i4] = (uint64_t)(uintptr_t)pdpt | PTE_KERNEL_RW;
    } else {
        pdpt = (uint64_t *)(uintptr_t)(pml4[i4] & PAGE_MASK);
    }

    uint64_t *pd;
    if (!(pdpt[i3] & PTE_PRESENT)) {
        pd = alloc_table();
        if (!pd) return false;
        pdpt[i3] = (uint64_t)(uintptr_t)pd | PTE_KERNEL_RW;
    } else {
        pd = (uint64_t *)(uintptr_t)(pdpt[i3] & PAGE_MASK);
    }

    pd[i2] = addr | flags | PTE_HUGE;
    return true;
}

/* Build identity map of [0 .. ram_end) using 2 MiB pages. */
static void identity_map_2mb(uint64_t *pml4, uint64_t ram_end) {
    for (uint64_t addr = 0; addr < ram_end; addr += (1ull << 21))
        map_2mb(pml4, addr, PTE_KERNEL_RW);
}

/* Identity-map the GOP framebuffer into `pml4`.
 *
 * The LFB sits in the PCI MMIO hole, which lands on either side of the loop
 * above depending on how much RAM the machine has: above the top of usable RAM
 * on a small box (QEMU with -m 256M leaves the OVMF framebuffer at 0x80000000
 * untouched), but *below* it on anything with >= 4 GiB, where the usable E820
 * top runs past 4 GiB and identity_map_2mb therefore covers the whole hole.
 * Mapping the framebuffer explicitly here handles both, and doing it before the
 * CR3 switch keeps the on-screen console alive across the changeover.
 *
 * Left write-back on purpose: firmware puts a write-combining MTRR over the
 * aperture, and a WB page-table entry combines with that to WC. Forcing PCD
 * here would pin the whole framebuffer to uncached and make blits crawl. */
static void map_framebuffer(uint64_t *pml4) {
    uint64_t           lfb  = *(volatile uint64_t *)(uintptr_t)FB_INFO_ADDR;
    volatile uint32_t *info = (volatile uint32_t *)(uintptr_t)(FB_INFO_ADDR + 8);
    uint64_t pitch  = info[0];
    uint64_t height = info[2];
    uint32_t valid  = info[4];

    if (!valid || lfb == 0 || pitch == 0 || height == 0) return;

    uint64_t start = lfb & ~((1ull << 21) - 1);
    uint64_t end   = (lfb + pitch * height + (1ull << 21) - 1) & ~((1ull << 21) - 1);
    for (uint64_t addr = start; addr < end; addr += (1ull << 21))
        map_2mb(pml4, addr, PTE_KERNEL_RW);
}

/* Replace a 2 MiB leaf with a page table describing the same 512 frames, so a
 * single 4 KiB page inside it can then be remapped independently. */
static uint64_t *split_huge_pd(uint64_t *pd, uint64_t i2) {
    uint64_t old   = pd[i2];
    uint64_t base  = old & PAGE_MASK;
    uint64_t flags = old & ~(PAGE_MASK | PTE_HUGE);

    uint64_t *pt = alloc_table();
    if (!pt) return NULL;
    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++)
        pt[i] = (base + (i << 12)) | flags;

    pd[i2] = (uint64_t)(uintptr_t)pt | PTE_KERNEL_RW;

    /* Drop the 2 MiB TLB entry that covered this range: leaving a stale large
     * page cached alongside the new 4 KiB ones is architecturally undefined. */
    write_cr3((uint64_t)(uintptr_t)kernel_pml4);
    return pt;
}

void vmm_init(uint64_t ram_end) {
    /* Cap identity map to keep early page-table count sane. */
    if (ram_end > (4ull << 30)) ram_end = 4ull << 30;
    /* Round up to 2 MiB boundary. */
    ram_end = (ram_end + (1ull << 21) - 1) & ~((1ull << 21) - 1);

    kernel_pml4 = alloc_table();
    if (!kernel_pml4) {
        console_puts("[vmm] FATAL: out of memory building PML4\n");
        for (;;) { cli(); hlt(); }
    }

    identity_map_2mb(kernel_pml4, ram_end);
    map_framebuffer(kernel_pml4);

    write_cr3((uint64_t)(uintptr_t)kernel_pml4);

    console_puts("[vmm] mapped identity 0..");
    console_put_hex(ram_end);
    console_puts(" with 2 MiB pages\n");
}

bool vmm_map(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    uint64_t i4 = pml4_index(virt);
    uint64_t i3 = pdpt_index(virt);
    uint64_t i2 = pd_index(virt);
    uint64_t i1 = pt_index(virt);

    uint64_t *pdpt;
    if (!(kernel_pml4[i4] & PTE_PRESENT)) {
        pdpt = alloc_table();
        if (!pdpt) return false;
        kernel_pml4[i4] = (uint64_t)(uintptr_t)pdpt | PTE_KERNEL_RW;
    } else {
        pdpt = next_table(kernel_pml4[i4]);
    }

    uint64_t *pd;
    if (!(pdpt[i3] & PTE_PRESENT)) {
        pd = alloc_table();
        if (!pd) return false;
        pdpt[i3] = (uint64_t)(uintptr_t)pd | PTE_KERNEL_RW;
    } else {
        if (pdpt[i3] & PTE_HUGE) return false;   /* 1 GiB leaf -- refuse */
        pd = next_table(pdpt[i3]);
    }

    uint64_t *pt;
    if (!(pd[i2] & PTE_PRESENT)) {
        pt = alloc_table();
        if (!pt) return false;
        pd[i2] = (uint64_t)(uintptr_t)pt | PTE_KERNEL_RW;
    } else if (pd[i2] & PTE_HUGE) {
        /* Break the 2 MiB leaf up rather than refusing. Anything living inside
         * the identity map -- most importantly the framebuffer on a machine
         * with >= 4 GiB of RAM -- would otherwise be impossible to remap. */
        pt = split_huge_pd(pd, i2);
        if (!pt) return false;
    } else {
        pt = next_table(pd[i2]);
    }

    pt[i1] = (phys & PAGE_MASK) | (flags & ~PTE_HUGE) | PTE_PRESENT;
    invlpg(virt);
    return true;
}

void vmm_unmap(uintptr_t virt) {
    uint64_t i4 = pml4_index(virt);
    if (!(kernel_pml4[i4] & PTE_PRESENT)) return;
    uint64_t *pdpt = next_table(kernel_pml4[i4]);
    if (!pdpt) return;

    uint64_t i3 = pdpt_index(virt);
    if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) return;
    uint64_t *pd = next_table(pdpt[i3]);
    if (!pd) return;

    uint64_t i2 = pd_index(virt);
    if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) return;
    uint64_t *pt = next_table(pd[i2]);
    if (!pt) return;

    uint64_t i1 = pt_index(virt);
    pt[i1] = 0;
    invlpg(virt);
}

uintptr_t vmm_translate(uintptr_t virt) {
    if (!(kernel_pml4[pml4_index(virt)] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = next_table(kernel_pml4[pml4_index(virt)]);
    if (!pdpt) return 0;

    uint64_t e3 = pdpt[pdpt_index(virt)];
    if (!(e3 & PTE_PRESENT)) return 0;
    if (e3 & PTE_HUGE) return (e3 & PAGE_MASK) | (virt & ((1ull << 30) - 1));

    uint64_t *pd = (uint64_t *)(uintptr_t)(e3 & PAGE_MASK);
    uint64_t e2 = pd[pd_index(virt)];
    if (!(e2 & PTE_PRESENT)) return 0;
    if (e2 & PTE_HUGE) return (e2 & PAGE_MASK) | (virt & ((1ull << 21) - 1));

    uint64_t *pt = (uint64_t *)(uintptr_t)(e2 & PAGE_MASK);
    uint64_t e1 = pt[pt_index(virt)];
    if (!(e1 & PTE_PRESENT)) return 0;
    return (e1 & PAGE_MASK) | (virt & 0xFFF);
}
