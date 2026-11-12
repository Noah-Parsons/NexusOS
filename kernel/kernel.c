/* kernel.c -- NexusOS top-level entry.
 *
 * Boot order:
 *   0. framebuffer  (so "print" means something on a machine with no serial
 *                    port and no VGA text buffer -- i.e. any UEFI laptop)
 *   1. console      (so everything else can print)
 *   2. idt          (install handlers BEFORE enabling interrupts)
 *   3. pic          (remap IRQs to 0x20..0x2F)
 *   4. pit          (100 Hz tick on IRQ0)
 *   5. keyboard     (IRQ1)
 *   6. sti          (interrupts on)
 *   7. pmm          (uses E820 from bootloader at 0x9000)
 *   8. vmm          (rebuilds page tables with 4 KiB API)
 *   9. kmalloc      (heap, demand-paged from PMM via VMM)
 */

#include "console.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "sched.h"
#include "sync.h"
#include "ata.h"
#include "fat.h"
#include "fb.h"
#include "fbcon.h"
#include "mouse.h"
#include "wm.h"
#include "io.h"

extern uint8_t __kernel_end;        /* from linker.ld */

/* ---------- Demo 1: sleep + mutex ----------------------------------------
 * Two tasks each wake 20 times/sec, increment their own counter, then
 * lock a shared counter and bump it. If the mutex works, the invariant
 * shared_counter == counter_a + counter_b holds at every observation
 * point (or is off by at most one, if one task is mid-update). The
 * `tasks` shell command checks this.                                       */

static volatile uint64_t counter_a, counter_b;
static volatile uint64_t shared_counter;
static mutex_t           shared_lock;

static void task_a_main(void) {
    for (;;) {
        counter_a++;
        mutex_lock(&shared_lock);
        shared_counter++;
        mutex_unlock(&shared_lock);
        sched_sleep_ms(50);
    }
}

static void task_b_main(void) {
    for (;;) {
        counter_b++;
        mutex_lock(&shared_lock);
        shared_counter++;
        mutex_unlock(&shared_lock);
        sched_sleep_ms(50);
    }
}

/* ---------- Demo 2: semaphore-backed bounded queue -----------------------
 * Producer fills slots, consumer drains them; semaphores enforce the
 * "don't write to full queue / don't read from empty queue" constraints.
 * Producer is faster than consumer, so the queue tends to fill and the
 * producer blocks waiting for empty slots. The `prod` shell command
 * shows produced/consumed counts and current queue depth.                  */

#define Q_SIZE 8
static int          queue_buf[Q_SIZE];
static uint32_t     q_head, q_tail;
static volatile uint64_t produced, consumed;
static semaphore_t  empty_slots;     /* initial Q_SIZE */
static semaphore_t  full_slots;      /* initial 0      */
static mutex_t      queue_lock;

static void producer_main(void) {
    int n = 0;
    for (;;) {
        sem_wait(&empty_slots);
        mutex_lock(&queue_lock);
        queue_buf[q_head] = n++;
        q_head = (q_head + 1) % Q_SIZE;
        produced++;
        mutex_unlock(&queue_lock);
        sem_post(&full_slots);
        sched_sleep_ms(40);          /* faster than consumer */
    }
}

static void consumer_main(void) {
    for (;;) {
        sem_wait(&full_slots);
        mutex_lock(&queue_lock);
        (void)queue_buf[q_tail];
        q_tail = (q_tail + 1) % Q_SIZE;
        consumed++;
        mutex_unlock(&queue_lock);
        sem_post(&empty_slots);
        sched_sleep_ms(100);         /* slower than producer */
    }
}

static uint64_t e820_ram_end(void) {
    uint32_t      count   = *(volatile uint32_t *)E820_COUNT_ADDR;
    e820_entry_t *entries = (e820_entry_t *)E820_ENTRIES_ADDR;
    uint64_t      end     = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].type != 1) continue;
        uint64_t top = entries[i].base + entries[i].length;
        if (top > end) end = top;
    }
    /* Match pmm_init's fallback when firmware reports no usable RAM. */
    if (end == 0) end = PMM_SYNTH_BASE + PMM_SYNTH_LEN;
    return end;
}

static void banner(void) {
    console_puts("\n");
    console_puts("==========================================\n");
    console_puts("           NexusOS  -  x86_64\n");
    console_puts("==========================================\n");
}

static void shell(void) {
    console_puts("\nnexus> ");
    char line[128];
    uint32_t n = 0;
    for (;;) {
        char c = keyboard_getc();
        if (c == '\n') {
            console_putc('\n');
            line[n] = '\0';
            if (n == 0) { /* nothing */ }
            else if (n == 4 && line[0]=='h' && line[1]=='e' && line[2]=='l' && line[3]=='p') {
                console_puts("commands: help, ticks, mem, tasks, prod, ls, cat <file>, halt\n");
            } else if (n == 5 && line[0]=='t' && line[1]=='i' && line[2]=='c' && line[3]=='k' && line[4]=='s') {
                console_puts("ticks: "); console_put_dec(pit_ticks()); console_putc('\n');
            } else if (n == 3 && line[0]=='m' && line[1]=='e' && line[2]=='m') {
                uint64_t used, freeb;
                kmalloc_stats(&used, &freeb);
                console_puts("frames used: "); console_put_dec(pmm_used_frames());
                console_puts(" / ");           console_put_dec(pmm_total_frames());
                console_puts("\nheap used:  "); console_put_dec(used);
                console_puts(" B, free: ");    console_put_dec(freeb); console_puts(" B\n");
            } else if (n == 5 && line[0]=='t' && line[1]=='a' && line[2]=='s' && line[3]=='k' && line[4]=='s') {
                /* Sample everything once, atomically with respect to ourselves,
                 * by reading without locks -- the sum/diff might be off by one
                 * if a task is mid-update, which is acceptable for a stats peek. */
                uint64_t a = counter_a, b = counter_b, sh = shared_counter;
                uint64_t expected = a + b;
                console_puts("switches:        "); console_put_dec(sched_switches()); console_putc('\n');
                console_puts("counter_a:       "); console_put_dec(a);                console_putc('\n');
                console_puts("counter_b:       "); console_put_dec(b);                console_putc('\n');
                console_puts("shared:          "); console_put_dec(sh);               console_putc('\n');
                console_puts("a+b - shared:    "); console_put_dec(expected - sh);    console_putc('\n');
                console_puts("                 (should be 0 or 1; never larger if mutex holds)\n");
            } else if (n == 4 && line[0]=='p' && line[1]=='r' && line[2]=='o' && line[3]=='d') {
                uint64_t p = produced, c = consumed;
                console_puts("produced: "); console_put_dec(p); console_putc('\n');
                console_puts("consumed: "); console_put_dec(c); console_putc('\n');
                console_puts("in queue: "); console_put_dec(p - c); console_putc('\n');
            } else if (n == 2 && line[0]=='l' && line[1]=='s') {
                fat_ls_root();
            } else if (n >= 5 && line[0]=='c' && line[1]=='a' && line[2]=='t' && line[3]==' ') {
                static uint8_t filebuf[8192];
                const char *name = &line[4];
                uint32_t got = fat_read_file(name, filebuf, sizeof(filebuf));
                if (got == 0) {
                    console_puts("file not found or empty\n");
                } else {
                    for (uint32_t i = 0; i < got; i++) console_putc((char)filebuf[i]);
                    if (filebuf[got - 1] != '\n') console_putc('\n');
                }
            } else if (n == 4 && line[0]=='h' && line[1]=='a' && line[2]=='l' && line[3]=='t') {
                console_puts("halting.\n");
                for (;;) { cli(); hlt(); }
            } else {
                console_puts("unknown command. try: help\n");
            }
            n = 0;
            console_puts("nexus> ");
        } else if (c == '\b') {
            if (n > 0) { n--; console_puts("\b \b"); }
        } else if (n + 1 < sizeof(line)) {
            line[n++] = c;
            console_putc(c);
        }
    }
}

void kernel_main(void) {
    console_init();

    /* Graphics before anything else. fb_init needs no page tables of its own
     * -- the boot identity map (the firmware's under UEFI, kernel_entry_bios.asm's
     * under BIOS) is still live here and covers the LFB -- so bringing it up
     * first costs nothing and means every line below actually reaches a screen.
     * Without it the whole boot is invisible until wm_init on the last line,
     * and any failure in between is indistinguishable from a hang. */
    if (fb_init()) fb_early_init();

    banner();
    console_puts("[boot] console ready\n");

    idt_init();
    console_puts("[boot] idt installed\n");

    pic_init();
    console_puts("[boot] pic remapped to 0x20\n");

    pit_init(100);
    keyboard_init();
    sti();
    console_puts("[boot] interrupts enabled\n");

    uintptr_t kend = (uintptr_t)&__kernel_end;
    console_puts("[boot] kernel ends at "); console_put_hex(kend); console_putc('\n');

    pmm_init(kend);

    uint64_t ram_end = e820_ram_end();
    vmm_init(ram_end);

    kmalloc_init();

    if (fb_active()) {
        const framebuffer_t *f = fb_get();
        console_puts("[fb] ");
        console_put_dec(f->width); console_putc('x');
        console_put_dec(f->height); console_putc('x');
        console_put_dec(f->bpp);
        console_puts(" @ "); console_put_hex((uintptr_t)f->addr);
        console_puts(" pitch="); console_put_dec(f->pitch); console_putc('\n');
        /* The on-screen console comes up later, inside the window manager's
         * terminal window (see wm_init). Until then output goes to serial. */
    } else {
        console_puts("[fb] no framebuffer; staying on text/serial\n");
    }

    /* Sanity-check the heap. */
    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kmalloc(4096);
    console_puts("[test] kmalloc -> ");
    console_put_hex((uintptr_t)a); console_putc(' ');
    console_put_hex((uintptr_t)b); console_putc(' ');
    console_put_hex((uintptr_t)c); console_putc('\n');
    kfree(b); kfree(a); kfree(c);

    console_puts("\nNexusOS ready. Type 'help' for commands.\n");

    sched_init();

    /* sleep + mutex demo */
    mutex_init(&shared_lock);
    task_create("counter_a", task_a_main);
    task_create("counter_b", task_b_main);

    /* semaphore-backed bounded queue demo */
    mutex_init(&queue_lock);
    sem_init(&empty_slots, Q_SIZE);
    sem_init(&full_slots, 0);
    task_create("producer", producer_main);
    task_create("consumer", consumer_main);

    /* Disk + filesystem. ATA reads block on a semaphore so this must
     * happen after the scheduler is up. */
    if (ata_init()) {
        fat_mount();
    }

    mouse_init();
    wm_init();      /* back buffer + demo windows + compositor task */

    shell();    /* runs forever as the idle task */
}
