/*
 * RISCV machine
 * 
 * Copyright (c) 2016-2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include "cutils.h"
#include "iomem.h"
#include "riscv_cpu.h"
#include "virtio.h"
#include "machine.h"
#include "snapshot.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* RISCV machine */

#define MAX_VIRTIO_DEVICES 31 /* one PLIC interrupt each */

typedef struct RISCVMachine {
    VirtMachine common;
    PhysMemoryMap *mem_map;
    int max_xlen;
    RISCVCPUState *cpu_state;
    uint64_t ram_size;
    /* RTC */
    BOOL rtc_real_time;
    uint64_t rtc_start_time;
    uint64_t timecmp;
    /* PLIC */
    uint32_t plic_pending_irq, plic_served_irq;
    IRQSignal plic_irq[32]; /* IRQ 0 is not used */
    /* HTIF */
    uint64_t htif_tohost, htif_fromhost;
    BOOL htif_kbd_wanted; /* guest asked for a keyboard character */

    VIRTIODevice *keyboard_dev;
    VIRTIODevice *mouse_dev;

    int virtio_count;
    VIRTIODevice *virtio_devs[MAX_VIRTIO_DEVICES];
    int blk_count;
    VIRTIODevice *blk_devs[MAX_DRIVE_DEVICE];

    /* what copy_bios() needs again on a machine reset */
    uint8_t *bios_buf, *kernel_buf, *initrd_buf;
    int bios_len, kernel_len, initrd_len;
    char *cmdline;
    BOOL reset_pending; /* the guest asked for a reboot */
    BOOL halted; /* the guest powered the machine off */
} RISCVMachine;

#define LOW_RAM_SIZE   0x00010000 /* 64KB */
#define RAM_BASE_ADDR  0x80000000
#define CLINT_BASE_ADDR 0x02000000
#define CLINT_SIZE      0x000c0000
#define SYSCON_BASE_ADDR 0x00100000 /* "sifive,test0" finisher, as on QEMU virt */
#define SYSCON_SIZE      0x1000
#define HTIF_BASE_ADDR 0x40008000
#define IDE_BASE_ADDR  0x40009000
#define VIRTIO_BASE_ADDR 0x40010000
#define VIRTIO_SIZE      0x1000
#define VIRTIO_IRQ       1
#define PLIC_BASE_ADDR 0x40100000
#define PLIC_SIZE      0x00400000
#define FRAMEBUFFER_BASE_ADDR 0x41000000

#define RTC_FREQ 10000000
#define RTC_FREQ_DIV 16 /* arbitrary, relative to CPU freq to have a
                           10 MHz frequency */

static uint64_t rtc_get_real_time(RISCVMachine *s)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * RTC_FREQ +
        (ts.tv_nsec / (1000000000 / RTC_FREQ));
}

static uint64_t rtc_get_time(RISCVMachine *m)
{
    uint64_t val;
    if (m->rtc_real_time) {
        val = rtc_get_real_time(m) - m->rtc_start_time;
    } else {
        val = riscv_cpu_get_cycles(m->cpu_state) / RTC_FREQ_DIV;
    }
    //    printf("rtc_time=%" PRId64 "\n", val);
    return val;
}

static uint64_t riscv_machine_get_time(void *opaque)
{
    return rtc_get_time(opaque);
}

static uint32_t htif_read(void *opaque, uint32_t offset,
                          int size_log2)
{
    RISCVMachine *s = opaque;
    uint32_t val;

    assert(size_log2 == 2);
    switch(offset) {
    case 0:
        val = s->htif_tohost;
        break;
    case 4:
        val = s->htif_tohost >> 32;
        break;
    case 8:
        val = s->htif_fromhost;
        break;
    case 12:
        val = s->htif_fromhost >> 32;
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

static uint8_t *memdup(const uint8_t *buf, int len)
{
    uint8_t *p;
    if (!buf || len <= 0)
        return NULL;
    p = malloc(len);
    memcpy(p, buf, len);
    return p;
}

static void riscv_machine_power_off(RISCVMachine *s, int status)
{
#ifdef __EMSCRIPTEN__
    /* exit() cannot stop the browser's event loop: stop running the
       CPU and let the page say so */
    s->halted = TRUE;
    EM_ASM({ if (Module.onPowerOff) Module.onPowerOff($0); }, status);
#else
    printf("\nPower off.\n");
    exit(status);
#endif
}

/* The QEMU virt "test" device: OpenSBI's syscon-reboot and syscon-poweroff
   drivers (see the reboot and poweroff nodes in the device tree) write
   0x7777 to reset the machine and 0x5555 to power it off. 0x3333 is a
   power off with an exit status in the upper 16 bits. */
#define SYSCON_FINISHER_FAIL  0x3333
#define SYSCON_FINISHER_PASS  0x5555
#define SYSCON_FINISHER_RESET 0x7777

static uint32_t syscon_read(void *opaque, uint32_t offset, int size_log2)
{
    return 0;
}

static void syscon_write(void *opaque, uint32_t offset, uint32_t val,
                         int size_log2)
{
    RISCVMachine *s = opaque;

    if (offset != 0)
        return;
    switch (val & 0xffff) {
    case SYSCON_FINISHER_RESET:
        s->reset_pending = TRUE;
        break;
    case SYSCON_FINISHER_PASS:
        riscv_machine_power_off(s, 0);
        break;
    case SYSCON_FINISHER_FAIL:
        riscv_machine_power_off(s, val >> 16);
        break;
    default:
        break;
    }
}

static void htif_handle_cmd(RISCVMachine *s)
{
    uint32_t device, cmd;

    device = s->htif_tohost >> 56;
    cmd = (s->htif_tohost >> 48) & 0xff;
    if (s->htif_tohost == 1) {
        /* shuthost */
        s->htif_tohost = 0;
        riscv_machine_power_off(s, 0);
    } else if (device == 1 && cmd == 1) {
        uint8_t buf[1];
        buf[0] = s->htif_tohost & 0xff;
        s->common.console->write_data(s->common.console->opaque, buf, 1);
        s->htif_tohost = 0;
        s->htif_fromhost = ((uint64_t)device << 56) | ((uint64_t)cmd << 48);
    } else if (device == 1 && cmd == 0) {
        /* request keyboard interrupt: the next console input byte goes
           to HTIF instead of the virtio console */
        s->htif_tohost = 0;
        s->htif_kbd_wanted = TRUE;
    } else {
        printf("HTIF: unsupported tohost=0x%016" PRIx64 "\n", s->htif_tohost);
    }
}

static void htif_write(void *opaque, uint32_t offset, uint32_t val,
                       int size_log2)
{
    RISCVMachine *s = opaque;

    assert(size_log2 == 2);
    switch(offset) {
    case 0:
        s->htif_tohost = (s->htif_tohost & ~0xffffffff) | val;
        break;
    case 4:
        s->htif_tohost = (s->htif_tohost & 0xffffffff) | ((uint64_t)val << 32);
        htif_handle_cmd(s);
        break;
    case 8:
        s->htif_fromhost = (s->htif_fromhost & ~0xffffffff) | val;
        break;
    case 12:
        s->htif_fromhost = (s->htif_fromhost & 0xffffffff) |
            (uint64_t)val << 32;
        break;
    default:
        break;
    }
}

/* console input goes to HTIF only while the guest (e.g. U-Boot) has an
   outstanding getc request, so that Linux's virtio console is not robbed
   of keystrokes */
static BOOL htif_console_input_wanted(VirtMachine *m)
{
    RISCVMachine *s = (RISCVMachine *)m;

    /* once the guest runs a virtio console (Linux), all input goes there */
    if (m->console_dev && virtio_console_can_write_data(m->console_dev))
        return FALSE;
    return s->htif_kbd_wanted && s->htif_fromhost == 0;
}

static BOOL htif_console_input(VirtMachine *m, uint8_t c)
{
    if (!htif_console_input_wanted(m))
        return FALSE;
    {
        RISCVMachine *s = (RISCVMachine *)m;
        s->htif_fromhost = ((uint64_t)1 << 56) | ((uint64_t)0 << 48) | c;
        s->htif_kbd_wanted = FALSE;
    }
    return TRUE;
}

#if 0
static void htif_poll(RISCVMachine *s)
{
    uint8_t buf[1];
    int ret;

    if (s->htif_fromhost == 0) {
        ret = s->console->read_data(s->console->opaque, buf, 1);
        if (ret == 1) {
            s->htif_fromhost = ((uint64_t)1 << 56) | ((uint64_t)0 << 48) |
                buf[0];
        }
    }
}
#endif

static uint32_t clint_read(void *opaque, uint32_t offset, int size_log2)
{
    RISCVMachine *m = opaque;
    uint32_t val;

    assert(size_log2 == 2);
    switch(offset) {
    case 0xbff8:
        val = rtc_get_time(m);
        break;
    case 0xbffc:
        val = rtc_get_time(m) >> 32;
        break;
    case 0x4000:
        val = m->timecmp;
        break;
    case 0x4004:
        val = m->timecmp >> 32;
        break;
    default:
        val = 0;
        break;
    }
    return val;
}
 
static void clint_write(void *opaque, uint32_t offset, uint32_t val,
                      int size_log2)
{
    RISCVMachine *m = opaque;

    assert(size_log2 == 2);
    switch(offset) {
    case 0x4000:
        m->timecmp = (m->timecmp & ~0xffffffff) | val;
        riscv_cpu_reset_mip(m->cpu_state, MIP_MTIP);
        break;
    case 0x4004:
        m->timecmp = (m->timecmp & 0xffffffff) | ((uint64_t)val << 32);
        riscv_cpu_reset_mip(m->cpu_state, MIP_MTIP);
        break;
    default:
        break;
    }
}

static void plic_update_mip(RISCVMachine *s)
{
    RISCVCPUState *cpu = s->cpu_state;
    uint32_t mask;
    mask = s->plic_pending_irq & ~s->plic_served_irq;
    if (mask) {
        riscv_cpu_set_mip(cpu, MIP_MEIP | MIP_SEIP);
    } else {
        riscv_cpu_reset_mip(cpu, MIP_MEIP | MIP_SEIP);
    }
}

#define PLIC_HART_BASE 0x200000
#define PLIC_HART_SIZE 0x1000

static uint32_t plic_read(void *opaque, uint32_t offset, int size_log2)
{
    RISCVMachine *s = opaque;
    uint32_t val, mask;
    int i;
    assert(size_log2 == 2);
    switch(offset) {
    case PLIC_HART_BASE:
        val = 0;
        break;
    case PLIC_HART_BASE + 4:
        mask = s->plic_pending_irq & ~s->plic_served_irq;
        if (mask != 0) {
            i = ctz32(mask);
            s->plic_served_irq |= 1 << i;
            plic_update_mip(s);
            val = i + 1;
        } else {
            val = 0;
        }
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

static void plic_write(void *opaque, uint32_t offset, uint32_t val,
                       int size_log2)
{
    RISCVMachine *s = opaque;
    
    assert(size_log2 == 2);
    switch(offset) {
    case PLIC_HART_BASE + 4:
        val--;
        if (val < 32) {
            s->plic_served_irq &= ~(1 << val);
            plic_update_mip(s);
        }
        break;
    default:
        break;
    }
}

static void plic_set_irq(void *opaque, int irq_num, int state)
{
    RISCVMachine *s = opaque;
    uint32_t mask;

    mask = 1 << (irq_num - 1);
    if (state) 
        s->plic_pending_irq |= mask;
    else
        s->plic_pending_irq &= ~mask;
    plic_update_mip(s);
}

static uint8_t *get_ram_ptr(RISCVMachine *s, uint64_t paddr, BOOL is_rw)
{
    return phys_mem_get_ram_ptr(s->mem_map, paddr, is_rw);
}

/* FDT machine description */

#define FDT_MAGIC	0xd00dfeed
#define FDT_VERSION	17

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version; /* <= 17 */
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct fdt_reserve_entry {
       uint64_t address;
       uint64_t size;
};

#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

typedef struct {
    uint32_t *tab;
    int tab_len;
    int tab_size;
    int open_node_count;
    
    char *string_table;
    int string_table_len;
    int string_table_size;
} FDTState;

static FDTState *fdt_init(void)
{
    FDTState *s;
    s = mallocz(sizeof(*s));
    return s;
}

static void fdt_alloc_len(FDTState *s, int len)
{
    int new_size;
    if (unlikely(len > s->tab_size)) {
        new_size = max_int(len, s->tab_size * 3 / 2);
        s->tab = realloc(s->tab, new_size * sizeof(uint32_t));
        s->tab_size = new_size;
    }
}

static void fdt_put32(FDTState *s, int v)
{
    fdt_alloc_len(s, s->tab_len + 1);
    s->tab[s->tab_len++] = cpu_to_be32(v);
}

/* the data is zero padded */
static void fdt_put_data(FDTState *s, const uint8_t *data, int len)
{
    int len1;
    
    len1 = (len + 3) / 4;
    fdt_alloc_len(s, s->tab_len + len1);
    memcpy(s->tab + s->tab_len, data, len);
    memset((uint8_t *)(s->tab + s->tab_len) + len, 0, -len & 3);
    s->tab_len += len1;
}

static void fdt_begin_node(FDTState *s, const char *name)
{
    fdt_put32(s, FDT_BEGIN_NODE);
    fdt_put_data(s, (uint8_t *)name, strlen(name) + 1);
    s->open_node_count++;
}

static void fdt_begin_node_num(FDTState *s, const char *name, uint64_t n)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s@%" PRIx64, name, n);
    fdt_begin_node(s, buf);
}

static void fdt_end_node(FDTState *s)
{
    fdt_put32(s, FDT_END_NODE);
    s->open_node_count--;
}

static int fdt_get_string_offset(FDTState *s, const char *name)
{
    int pos, new_size, name_size, new_len;

    pos = 0;
    while (pos < s->string_table_len) {
        if (!strcmp(s->string_table + pos, name))
            return pos;
        pos += strlen(s->string_table + pos) + 1;
    }
    /* add a new string */
    name_size = strlen(name) + 1;
    new_len = s->string_table_len + name_size;
    if (new_len > s->string_table_size) {
        new_size = max_int(new_len, s->string_table_size * 3 / 2);
        s->string_table = realloc(s->string_table, new_size);
        s->string_table_size = new_size;
    }
    pos = s->string_table_len;
    memcpy(s->string_table + pos, name, name_size);
    s->string_table_len = new_len;
    return pos;
}

static void fdt_prop(FDTState *s, const char *prop_name,
                     const void *data, int data_len)
{
    fdt_put32(s, FDT_PROP);
    fdt_put32(s, data_len);
    fdt_put32(s, fdt_get_string_offset(s, prop_name));
    fdt_put_data(s, data, data_len);
}

static void fdt_prop_tab_u32(FDTState *s, const char *prop_name,
                             uint32_t *tab, int tab_len)
{
    int i;
    fdt_put32(s, FDT_PROP);
    fdt_put32(s, tab_len * sizeof(uint32_t));
    fdt_put32(s, fdt_get_string_offset(s, prop_name));
    for(i = 0; i < tab_len; i++)
        fdt_put32(s, tab[i]);
}

static void fdt_prop_u32(FDTState *s, const char *prop_name, uint32_t val)
{
    fdt_prop_tab_u32(s, prop_name, &val, 1);
}

static void fdt_prop_tab_u64(FDTState *s, const char *prop_name,
                             uint64_t v0)
{
    uint32_t tab[2];
    tab[0] = v0 >> 32;
    tab[1] = v0;
    fdt_prop_tab_u32(s, prop_name, tab, 2);
}

static void fdt_prop_tab_u64_2(FDTState *s, const char *prop_name,
                               uint64_t v0, uint64_t v1)
{
    uint32_t tab[4];
    tab[0] = v0 >> 32;
    tab[1] = v0;
    tab[2] = v1 >> 32;
    tab[3] = v1;
    fdt_prop_tab_u32(s, prop_name, tab, 4);
}

static void fdt_prop_str(FDTState *s, const char *prop_name,
                         const char *str)
{
    fdt_prop(s, prop_name, str, strlen(str) + 1);
}

/* NULL terminated string list */
static void fdt_prop_tab_str(FDTState *s, const char *prop_name,
                             ...)
{
    va_list ap;
    int size, str_size;
    char *ptr, *tab;

    va_start(ap, prop_name);
    size = 0;
    for(;;) {
        ptr = va_arg(ap, char *);
        if (!ptr)
            break;
        str_size = strlen(ptr) + 1;
        size += str_size;
    }
    va_end(ap);
    
    tab = malloc(size);
    va_start(ap, prop_name);
    size = 0;
    for(;;) {
        ptr = va_arg(ap, char *);
        if (!ptr)
            break;
        str_size = strlen(ptr) + 1;
        memcpy(tab + size, ptr, str_size);
        size += str_size;
    }
    va_end(ap);
    
    fdt_prop(s, prop_name, tab, size);
    free(tab);
}

/* write the FDT to 'dst1'. return the FDT size in bytes */
int fdt_output(FDTState *s, uint8_t *dst)
{
    struct fdt_header *h;
    struct fdt_reserve_entry *re;
    int dt_struct_size;
    int dt_strings_size;
    int pos;

    assert(s->open_node_count == 0);
    
    fdt_put32(s, FDT_END);
    
    dt_struct_size = s->tab_len * sizeof(uint32_t);
    dt_strings_size = s->string_table_len;

    h = (struct fdt_header *)dst;
    h->magic = cpu_to_be32(FDT_MAGIC);
    h->version = cpu_to_be32(FDT_VERSION);
    h->last_comp_version = cpu_to_be32(16);
    h->boot_cpuid_phys = cpu_to_be32(0);
    h->size_dt_strings = cpu_to_be32(dt_strings_size);
    h->size_dt_struct = cpu_to_be32(dt_struct_size);

    pos = sizeof(struct fdt_header);

    h->off_dt_struct = cpu_to_be32(pos);
    memcpy(dst + pos, s->tab, dt_struct_size);
    pos += dt_struct_size;

    /* align to 8 */
    while ((pos & 7) != 0) {
        dst[pos++] = 0;
    }
    h->off_mem_rsvmap = cpu_to_be32(pos);
    re = (struct fdt_reserve_entry *)(dst + pos);
    re->address = 0; /* no reserved entry */
    re->size = 0;
    pos += sizeof(struct fdt_reserve_entry);

    h->off_dt_strings = cpu_to_be32(pos);
    memcpy(dst + pos, s->string_table, dt_strings_size);
    pos += dt_strings_size;

    /* align to 8, just in case */
    while ((pos & 7) != 0) {
        dst[pos++] = 0;
    }

    h->totalsize = cpu_to_be32(pos);
    return pos;
}

void fdt_end(FDTState *s)
{
    free(s->tab);
    free(s->string_table);
    free(s);
}

static int riscv_build_fdt(RISCVMachine *m, uint8_t *dst,
                           uint64_t kernel_start, uint64_t kernel_size,
                           uint64_t initrd_start, uint64_t initrd_size,
                           const char *cmd_line)
{
    FDTState *s;
    int size, max_xlen, i, cur_phandle, intc_phandle, plic_phandle;
    int syscon_phandle;
    char isa_string[128], *q;
    uint32_t misa;
    uint32_t tab[8];
    FBDevice *fb_dev;
    
    s = fdt_init();

    cur_phandle = 1;
    
    fdt_begin_node(s, "");
    fdt_prop_u32(s, "#address-cells", 2);
    fdt_prop_u32(s, "#size-cells", 2);
    fdt_prop_str(s, "compatible", "ucbbar,riscvemu-bar_dev");
    fdt_prop_str(s, "model", "ucbbar,riscvemu-bare");

    /* CPU list */
    fdt_begin_node(s, "cpus");
    fdt_prop_u32(s, "#address-cells", 1);
    fdt_prop_u32(s, "#size-cells", 0);
    fdt_prop_u32(s, "timebase-frequency", RTC_FREQ);

    /* cpu */
    fdt_begin_node_num(s, "cpu", 0);
    fdt_prop_str(s, "device_type", "cpu");
    fdt_prop_u32(s, "reg", 0);
    fdt_prop_str(s, "status", "okay");
    fdt_prop_str(s, "compatible", "riscv");

    max_xlen = m->max_xlen;
    misa = riscv_cpu_get_misa(m->cpu_state);
    q = isa_string;
    q += snprintf(isa_string, sizeof(isa_string), "rv%d", max_xlen);
    for(i = 0; i < 26; i++) {
        if (misa & (1 << i))
            *q++ = 'a' + i;
    }
    *q = '\0';
    fdt_prop_str(s, "riscv,isa", isa_string);
    
    fdt_prop_str(s, "mmu-type", max_xlen <= 32 ? "riscv,sv32" : "riscv,sv48");
    fdt_prop_u32(s, "clock-frequency", 2000000000);

    fdt_begin_node(s, "interrupt-controller");
    fdt_prop_u32(s, "#interrupt-cells", 1);
    fdt_prop(s, "interrupt-controller", NULL, 0);
    fdt_prop_str(s, "compatible", "riscv,cpu-intc");
    intc_phandle = cur_phandle++;
    fdt_prop_u32(s, "phandle", intc_phandle);
    fdt_end_node(s); /* interrupt-controller */
    
    fdt_end_node(s); /* cpu */
    
    fdt_end_node(s); /* cpus */

    fdt_begin_node_num(s, "memory", RAM_BASE_ADDR);
    fdt_prop_str(s, "device_type", "memory");
    tab[0] = (uint64_t)RAM_BASE_ADDR >> 32;
    tab[1] = RAM_BASE_ADDR;
    tab[2] = m->ram_size >> 32;
    tab[3] = m->ram_size;
    fdt_prop_tab_u32(s, "reg", tab, 4);
    
    fdt_end_node(s); /* memory */

    /* Reserve the firmware region below the kernel. BBL stays resident at
       RAM_BASE_ADDR and handles every SBI call and rdtime emulation, but
       nothing told Linux about it; Linux 6.x allocates RAM below the kernel
       image, overwrites BBL and then dies in an M-mode trap loop as soon
       as userspace gets busy. OpenSBI publishes the same kind of node. */
    if (kernel_size > 0 && kernel_start > RAM_BASE_ADDR) {
        fdt_begin_node(s, "reserved-memory");
        fdt_prop_u32(s, "#address-cells", 2);
        fdt_prop_u32(s, "#size-cells", 2);
        fdt_prop(s, "ranges", NULL, 0);
        fdt_begin_node_num(s, "mmode_resv0", RAM_BASE_ADDR);
        fdt_prop_tab_u64_2(s, "reg", RAM_BASE_ADDR,
                           kernel_start - RAM_BASE_ADDR);
        fdt_prop(s, "no-map", NULL, 0);
        fdt_end_node(s); /* mmode_resv0 */
        fdt_end_node(s); /* reserved-memory */
    }

    fdt_begin_node(s, "htif");
    fdt_prop_str(s, "compatible", "ucb,htif0");
    /* fromhost then tohost, as OpenSBI's fdt_serial_htif expects; the
       registers are MMIO at HTIF_BASE_ADDR (tohost @0, fromhost @8) */
    tab[0] = 0;
    tab[1] = HTIF_BASE_ADDR + 8;
    tab[2] = 0;
    tab[3] = 8;
    tab[4] = 0;
    tab[5] = HTIF_BASE_ADDR;
    tab[6] = 0;
    tab[7] = 8;
    fdt_prop_tab_u32(s, "reg", tab, 8);
    /* U-Boot only binds pre-relocation console drivers to nodes tagged
       this way (see doc/develop/devicetree/dt_qemu.rst in U-Boot) */
    fdt_prop(s, "bootph-all", NULL, 0);
    fdt_end_node(s); /* htif */

    fdt_begin_node(s, "soc");
    fdt_prop_u32(s, "#address-cells", 2);
    fdt_prop_u32(s, "#size-cells", 2);
    fdt_prop_tab_str(s, "compatible",
                     "ucbbar,riscvemu-bar-soc", "simple-bus", NULL);
    fdt_prop(s, "ranges", NULL, 0);

    fdt_begin_node_num(s, "clint", CLINT_BASE_ADDR);
    fdt_prop_str(s, "compatible", "riscv,clint0");

    tab[0] = intc_phandle;
    tab[1] = 3; /* M IPI irq */
    tab[2] = intc_phandle;
    tab[3] = 7; /* M timer irq */
    fdt_prop_tab_u32(s, "interrupts-extended", tab, 4);

    fdt_prop_tab_u64_2(s, "reg", CLINT_BASE_ADDR, CLINT_SIZE);
    
    fdt_end_node(s); /* clint */

    fdt_begin_node_num(s, "test", SYSCON_BASE_ADDR);
    fdt_prop_tab_str(s, "compatible", "sifive,test1", "sifive,test0",
                     "syscon", NULL);
    fdt_prop_tab_u64_2(s, "reg", SYSCON_BASE_ADDR, SYSCON_SIZE);
    syscon_phandle = cur_phandle++;
    fdt_prop_u32(s, "phandle", syscon_phandle);
    fdt_end_node(s); /* test */

    fdt_begin_node_num(s, "plic", PLIC_BASE_ADDR);
    fdt_prop_u32(s, "#interrupt-cells", 1);
    fdt_prop(s, "interrupt-controller", NULL, 0);
    fdt_prop_str(s, "compatible", "riscv,plic0");
    fdt_prop_u32(s, "riscv,ndev", 31);
    fdt_prop_tab_u64_2(s, "reg", PLIC_BASE_ADDR, PLIC_SIZE);

    tab[0] = intc_phandle;
    tab[1] = 9; /* S ext irq */
    tab[2] = intc_phandle;
    tab[3] = 11; /* M ext irq */
    fdt_prop_tab_u32(s, "interrupts-extended", tab, 4);

    plic_phandle = cur_phandle++;
    fdt_prop_u32(s, "phandle", plic_phandle);

    fdt_end_node(s); /* plic */
    
    for(i = 0; i < m->virtio_count; i++) {
        fdt_begin_node_num(s, "virtio", VIRTIO_BASE_ADDR + i * VIRTIO_SIZE);
        fdt_prop_str(s, "compatible", "virtio,mmio");
        fdt_prop_tab_u64_2(s, "reg", VIRTIO_BASE_ADDR + i * VIRTIO_SIZE,
                           VIRTIO_SIZE);
        tab[0] = plic_phandle;
        tab[1] = VIRTIO_IRQ + i;
        fdt_prop_tab_u32(s, "interrupts-extended", tab, 2);
        fdt_end_node(s); /* virtio */
    }

    fb_dev = m->common.fb_dev;
    if (fb_dev) {
        fdt_begin_node_num(s, "framebuffer", FRAMEBUFFER_BASE_ADDR);
        fdt_prop_str(s, "compatible", "simple-framebuffer");
        fdt_prop_tab_u64_2(s, "reg", FRAMEBUFFER_BASE_ADDR, fb_dev->fb_size);
        fdt_prop_u32(s, "width", fb_dev->width);
        fdt_prop_u32(s, "height", fb_dev->height);
        fdt_prop_u32(s, "stride", fb_dev->stride);
        fdt_prop_str(s, "format", "a8r8g8b8");
        fdt_end_node(s); /* framebuffer */
    }
    
    fdt_end_node(s); /* soc */

    fdt_begin_node(s, "reboot");
    fdt_prop_str(s, "compatible", "syscon-reboot");
    fdt_prop_u32(s, "regmap", syscon_phandle);
    fdt_prop_u32(s, "offset", 0);
    fdt_prop_u32(s, "value", SYSCON_FINISHER_RESET);
    fdt_end_node(s); /* reboot */

    fdt_begin_node(s, "poweroff");
    fdt_prop_str(s, "compatible", "syscon-poweroff");
    fdt_prop_u32(s, "regmap", syscon_phandle);
    fdt_prop_u32(s, "offset", 0);
    fdt_prop_u32(s, "value", SYSCON_FINISHER_PASS);
    fdt_end_node(s); /* poweroff */

    fdt_begin_node(s, "chosen");
    fdt_prop_str(s, "bootargs", cmd_line ? cmd_line : "");
    if (kernel_size > 0) {
        fdt_prop_tab_u64(s, "riscv,kernel-start", kernel_start);
        fdt_prop_tab_u64(s, "riscv,kernel-end", kernel_start + kernel_size);
    }
    if (initrd_size > 0) {
        fdt_prop_tab_u64(s, "linux,initrd-start", initrd_start);
        fdt_prop_tab_u64(s, "linux,initrd-end", initrd_start + initrd_size);
    }
    

    fdt_end_node(s); /* chosen */
    
    fdt_end_node(s); /* / */

    size = fdt_output(s, dst);
#if 0
    {
        FILE *f;
        f = fopen("/tmp/riscvemu.dtb", "wb");
        fwrite(dst, 1, size, f);
        fclose(f);
    }
#endif
    fdt_end(s);
    return size;
}

static void copy_bios(RISCVMachine *s, const uint8_t *buf, int buf_len,
                      const uint8_t *kernel_buf, int kernel_buf_len,
                      const uint8_t *initrd_buf, int initrd_buf_len,
                      const char *cmd_line)
{
    uint32_t fdt_addr, align, kernel_base, initrd_base;
    uint8_t *ram_ptr;
    uint32_t *q;

    if (buf_len > s->ram_size) {
        vm_error("BIOS too big\n");
        exit(1);
    }

    ram_ptr = get_ram_ptr(s, RAM_BASE_ADDR, TRUE);
    memcpy(ram_ptr, buf, buf_len);

    kernel_base = 0;
    if (kernel_buf_len > 0) {
        /* copy the kernel if present */
        if (s->max_xlen == 32)
            align = 4 << 20; /* 4 MB page align */
        else
            align = 2 << 20; /* 2 MB page align */
        kernel_base = (buf_len + align - 1) & ~(align - 1);
        memcpy(ram_ptr + kernel_base, kernel_buf, kernel_buf_len);
        if (kernel_buf_len + kernel_base > s->ram_size) {
            vm_error("kernel too big");
            exit(1);
        }
    }

    initrd_base = 0;
    if (initrd_buf_len > 0) {
        /* same allocation as QEMU */
        initrd_base = s->ram_size / 2;
        if (initrd_base > (128 << 20))
            initrd_base = 128 << 20;
        memcpy(ram_ptr + initrd_base, initrd_buf, initrd_buf_len);
        if (initrd_buf_len + initrd_base > s->ram_size) {
            vm_error("initrd too big");
            exit(1);
        }
    }
    
    ram_ptr = get_ram_ptr(s, 0, TRUE);
    
    fdt_addr = 0x1000 + 8 * 8;

    riscv_build_fdt(s, ram_ptr + fdt_addr,
                    RAM_BASE_ADDR + kernel_base, kernel_buf_len,
                    RAM_BASE_ADDR + initrd_base, initrd_buf_len,
                    cmd_line);

    /* jump_addr = 0x80000000 */
    
    q = (uint32_t *)(ram_ptr + 0x1000);
    q[0] = 0x297 + 0x80000000 - 0x1000; /* auipc t0, jump_addr */
    q[1] = 0x597; /* auipc a1, dtb */
    q[2] = 0x58593 + ((fdt_addr - 4) << 20); /* addi a1, a1, dtb */
    q[3] = 0xf1402573; /* csrr a0, mhartid */
    q[4] = 0x00028067; /* jalr zero, t0, jump_addr */
}

static void riscv_flush_tlb_write_range(void *opaque, uint8_t *ram_addr,
                                        size_t ram_size)
{
    RISCVMachine *s = opaque;
    riscv_cpu_flush_tlb_write_range_ram(s->cpu_state, ram_addr, ram_size);
}

static void riscv_machine_set_defaults(VirtMachineParams *p)
{
}

static VirtMachine *riscv_machine_init(const VirtMachineParams *p)
{
    RISCVMachine *s;
    VIRTIODevice *blk_dev;
    int irq_num, i, max_xlen, ram_flags;
    VIRTIOBusDef vbus_s, *vbus = &vbus_s;


    if (!strcmp(p->machine_name, "riscv32")) {
        max_xlen = 32;
    } else if (!strcmp(p->machine_name, "riscv64")) {
        max_xlen = 64;
    } else if (!strcmp(p->machine_name, "riscv128")) {
        max_xlen = 128;
    } else {
        vm_error("unsupported machine: %s\n", p->machine_name);
        return NULL;
    }
    
    s = mallocz(sizeof(*s));
    s->common.vmc = p->vmc;
    s->ram_size = p->ram_size;
    s->max_xlen = max_xlen;
    s->mem_map = phys_mem_map_init();
    /* needed to handle the RAM dirty bits */
    s->mem_map->opaque = s;
    s->mem_map->flush_tlb_write_range = riscv_flush_tlb_write_range;

    s->common.console_input_filter = htif_console_input;
    s->common.console_input_wanted = htif_console_input_wanted;
    s->cpu_state = riscv_cpu_init(s->mem_map, max_xlen);
    /* expose the CLINT's mtime through the 'time' CSR as well, so that
       firmware (OpenSBI) and S-mode software can rdtime without traps */
    ((RISCVCPUCommonState *)s->cpu_state)->get_time = riscv_machine_get_time;
    ((RISCVCPUCommonState *)s->cpu_state)->get_time_opaque = s;
    if (!s->cpu_state) {
        vm_error("unsupported max_xlen=%d\n", max_xlen);
        /* XXX: should free resources */
        return NULL;
    }
    /* RAM */
    ram_flags = 0;
    cpu_register_ram(s->mem_map, RAM_BASE_ADDR, p->ram_size, ram_flags);
    cpu_register_ram(s->mem_map, 0x00000000, LOW_RAM_SIZE, 0);
    s->rtc_real_time = p->rtc_real_time;
    if (p->rtc_real_time) {
        s->rtc_start_time = rtc_get_real_time(s);
    }
    
    cpu_register_device(s->mem_map, CLINT_BASE_ADDR, CLINT_SIZE, s,
                        clint_read, clint_write, DEVIO_SIZE32);
    cpu_register_device(s->mem_map, PLIC_BASE_ADDR, PLIC_SIZE, s,
                        plic_read, plic_write, DEVIO_SIZE32);
    for(i = 1; i < 32; i++) {
        irq_init(&s->plic_irq[i], plic_set_irq, s, i);
    }

    cpu_register_device(s->mem_map, HTIF_BASE_ADDR, 16,
                        s, htif_read, htif_write, DEVIO_SIZE32);
    cpu_register_device(s->mem_map, SYSCON_BASE_ADDR, SYSCON_SIZE,
                        s, syscon_read, syscon_write, DEVIO_SIZE32);
    s->common.console = p->console;

    memset(vbus, 0, sizeof(*vbus));
    vbus->mem_map = s->mem_map;
    vbus->addr = VIRTIO_BASE_ADDR;
    irq_num = VIRTIO_IRQ;
    
    /* virtio console */
    if (p->console) {
        vbus->irq = &s->plic_irq[irq_num];
        s->common.console_dev = virtio_console_init(vbus, p->console);
        s->virtio_devs[s->virtio_count++] = s->common.console_dev;
        vbus->addr += VIRTIO_SIZE;
        irq_num++;
    }
    
    /* virtio net device */
    for(i = 0; i < p->eth_count; i++) {
        vbus->irq = &s->plic_irq[irq_num];
        s->virtio_devs[s->virtio_count++] =
            virtio_net_init(vbus, p->tab_eth[i].net);
        s->common.net = p->tab_eth[i].net;
        vbus->addr += VIRTIO_SIZE;
        irq_num++;
    }

    /* virtio block device */
    for(i = 0; i < p->drive_count; i++) {
        vbus->irq = &s->plic_irq[irq_num];
        blk_dev = virtio_block_init(vbus, p->tab_drive[i].block_dev);
        s->virtio_devs[s->virtio_count++] = blk_dev;
        s->blk_devs[s->blk_count++] = blk_dev;
        vbus->addr += VIRTIO_SIZE;
        irq_num++;
    }

    /* virtio filesystem */
    for(i = 0; i < p->fs_count; i++) {
        VIRTIODevice *fs_dev;
        vbus->irq = &s->plic_irq[irq_num];
        fs_dev = virtio_9p_init(vbus, p->tab_fs[i].fs_dev,
                                p->tab_fs[i].tag);
        s->virtio_devs[s->virtio_count++] = fs_dev;
        //        virtio_set_debug(fs_dev, VIRTIO_DEBUG_9P);
        vbus->addr += VIRTIO_SIZE;
        irq_num++;
    }

    if (p->display_device) {
        FBDevice *fb_dev;
        fb_dev = mallocz(sizeof(*fb_dev));
        s->common.fb_dev = fb_dev;
        if (!strcmp(p->display_device, "simplefb")) {
            simplefb_init(s->mem_map,
                          FRAMEBUFFER_BASE_ADDR,
                          fb_dev,
                          p->width, p->height);
            
        } else {
            vm_error("unsupported display device: %s\n", p->display_device);
            exit(1);
        }
    }

    if (p->input_device) {
        if (!strcmp(p->input_device, "virtio")) {
            vbus->irq = &s->plic_irq[irq_num];
            s->keyboard_dev = virtio_input_init(vbus,
                                                VIRTIO_INPUT_TYPE_KEYBOARD);
            s->virtio_devs[s->virtio_count++] = s->keyboard_dev;
            vbus->addr += VIRTIO_SIZE;
            irq_num++;

            vbus->irq = &s->plic_irq[irq_num];
            s->mouse_dev = virtio_input_init(vbus,
                                             VIRTIO_INPUT_TYPE_TABLET);
            s->virtio_devs[s->virtio_count++] = s->mouse_dev;
            vbus->addr += VIRTIO_SIZE;
            irq_num++;
        } else {
            vm_error("unsupported input device: %s\n", p->input_device);
            exit(1);
        }
    }
    
    /* entropy source, after the other devices so that their addresses
       and interrupts do not move */
    vbus->irq = &s->plic_irq[irq_num];
    s->virtio_devs[s->virtio_count++] = virtio_rng_init(vbus);
    vbus->addr += VIRTIO_SIZE;
    irq_num++;

    if (!p->files[VM_FILE_BIOS].buf) {
        vm_error("No bios found");
    }

    /* the config (and these buffers) is freed once the machine is up,
       but a reset needs them again */
    s->bios_len = p->files[VM_FILE_BIOS].len;
    s->bios_buf = memdup(p->files[VM_FILE_BIOS].buf, s->bios_len);
    s->kernel_len = p->files[VM_FILE_KERNEL].len;
    s->kernel_buf = memdup(p->files[VM_FILE_KERNEL].buf, s->kernel_len);
    s->initrd_len = p->files[VM_FILE_INITRD].len;
    s->initrd_buf = memdup(p->files[VM_FILE_INITRD].buf, s->initrd_len);
    s->cmdline = p->cmdline ? strdup(p->cmdline) : NULL;

    copy_bios(s, s->bios_buf, s->bios_len, s->kernel_buf, s->kernel_len,
              s->initrd_buf, s->initrd_len, s->cmdline);
    
    return (VirtMachine *)s;
}

/* Reboot: put the CPU, the interrupt controllers, HTIF and the virtio
   devices back in their power-on state and load the firmware again, as a
   cold boot would. The disks keep their contents. */
static void riscv_machine_reset(RISCVMachine *s)
{
    int i;

    s->reset_pending = FALSE;
    for(i = 0; i < s->virtio_count; i++)
        virtio_device_reset(s->virtio_devs[i]);
    s->plic_pending_irq = 0;
    s->plic_served_irq = 0;
    s->htif_tohost = 0;
    s->htif_fromhost = 0;
    s->htif_kbd_wanted = FALSE;
    s->timecmp = 0;
    riscv_cpu_reset(s->cpu_state);
    if (s->rtc_real_time)
        s->rtc_start_time = rtc_get_real_time(s);

    memset(get_ram_ptr(s, RAM_BASE_ADDR, TRUE), 0, s->ram_size);
    memset(get_ram_ptr(s, 0, TRUE), 0, LOW_RAM_SIZE);
    copy_bios(s, s->bios_buf, s->bios_len, s->kernel_buf, s->kernel_len,
              s->initrd_buf, s->initrd_len, s->cmdline);
#ifdef __EMSCRIPTEN__
    EM_ASM({ if (Module.onReset) Module.onReset(); });
#endif
}

/* a reset waits until no block request is outstanding */
static BOOL riscv_machine_reset_ready(RISCVMachine *s)
{
    int i;
    for(i = 0; i < s->blk_count; i++) {
        if (virtio_block_busy(s->blk_devs[i]))
            return FALSE;
    }
    return TRUE;
}

/* Snapshots. The state: the timer (as the current mtime, so that guest
   time continues from where it was saved), the CLINT, PLIC and HTIF
   registers, the CPU, each virtio device in order and the data written to
   each disk. The RAM is not included: see riscv_machine_get_ram(). */
#define RISCV_STATE_MAGIC 0x54535652 /* "RVST" */
#define RISCV_STATE_VERSION 1

static int riscv_machine_save_state(VirtMachine *s1, uint8_t *buf,
                                    int buf_size)
{
    RISCVMachine *s = (RISCVMachine *)s1;
    SnapWriter w1, *w = &w1;
    BlockDevice *bs;
    int i, len;
    uint8_t *p;

    if (s->halted || s->reset_pending || !riscv_machine_reset_ready(s))
        return -1;
    snap_writer_init(w, buf, buf_size);
    snap_put_u32(w, RISCV_STATE_MAGIC);
    snap_put_u32(w, RISCV_STATE_VERSION);
    snap_put_u32(w, s->max_xlen);
    snap_put_u64(w, s->ram_size);
    snap_put_u32(w, LOW_RAM_SIZE);

    snap_put_u64(w, rtc_get_time(s));
    snap_put_u64(w, s->timecmp);
    snap_put_u32(w, s->plic_pending_irq);
    snap_put_u32(w, s->plic_served_irq);
    snap_put_u64(w, s->htif_tohost);
    snap_put_u64(w, s->htif_fromhost);
    snap_put_u32(w, s->htif_kbd_wanted);

    len = riscv_cpu_save_state(s->cpu_state, NULL, 0);
    snap_put_u32(w, len);
    p = snap_reserve(w, len);
    if (p)
        riscv_cpu_save_state(s->cpu_state, p, len);

    snap_put_u32(w, s->virtio_count);
    for(i = 0; i < s->virtio_count; i++) {
        len = virtio_device_save(s->virtio_devs[i], NULL, 0);
        if (len < 0)
            return -1;
        snap_put_u32(w, len);
        p = snap_reserve(w, len);
        if (p)
            virtio_device_save(s->virtio_devs[i], p, len);
    }

    snap_put_u32(w, s->blk_count);
    for(i = 0; i < s->blk_count; i++) {
        bs = virtio_block_get_device(s->blk_devs[i]);
        if (!bs->save_writes)
            return -1;
        len = bs->save_writes(bs, NULL, 0);
        if (len < 0)
            return -1;
        snap_put_u32(w, len);
        p = snap_reserve(w, len);
        if (p)
            bs->save_writes(bs, p, len);
    }
    return snap_writer_end(w);
}

/* two passes: the first one checks everything, the second one (unless
   check_only) applies it, so a state that does not fit changes nothing */
static int riscv_machine_load_state(VirtMachine *s1, const uint8_t *buf,
                                    int len, BOOL check_only)
{
    RISCVMachine *s = (RISCVMachine *)s1;
    SnapReader r1, *r = &r1;
    uint64_t mtime, timecmp, tohost, fromhost;
    uint32_t pending, served, kbd_wanted;
    BlockDevice *bs;
    const uint8_t *p;
    int i, n, pass, l;

    for(pass = 0; pass < 2; pass++) {
        BOOL apply = (pass == 1);
        snap_reader_init(r, buf, len);
        if (snap_get_u32(r) != RISCV_STATE_MAGIC ||
            snap_get_u32(r) != RISCV_STATE_VERSION ||
            snap_get_u32(r) != s->max_xlen ||
            snap_get_u64(r) != s->ram_size ||
            snap_get_u32(r) != LOW_RAM_SIZE)
            return -1;
        mtime = snap_get_u64(r);
        timecmp = snap_get_u64(r);
        pending = snap_get_u32(r);
        served = snap_get_u32(r);
        tohost = snap_get_u64(r);
        fromhost = snap_get_u64(r);
        kbd_wanted = snap_get_u32(r);

        l = snap_get_u32(r);
        p = snap_get_data(r, l);
        if (!p || riscv_cpu_load_state(s->cpu_state, p, l, !apply) < 0)
            return -1;

        n = snap_get_u32(r);
        if (n != s->virtio_count)
            return -1;
        for(i = 0; i < n; i++) {
            l = snap_get_u32(r);
            p = snap_get_data(r, l);
            if (!p || virtio_device_load(s->virtio_devs[i], p, l, !apply) < 0)
                return -1;
        }

        n = snap_get_u32(r);
        if (n != s->blk_count)
            return -1;
        for(i = 0; i < n; i++) {
            bs = virtio_block_get_device(s->blk_devs[i]);
            l = snap_get_u32(r);
            p = snap_get_data(r, l);
            if (!p || !bs->load_writes || bs->load_writes(bs, p, l, !apply) < 0)
                return -1;
        }
        if (!snap_reader_done(r))
            return -1;

        if (apply) {
            /* the interrupt controller last: it is the source of truth
               for the interrupt lines of the devices */
            if (s->rtc_real_time)
                s->rtc_start_time = rtc_get_real_time(s) - mtime;
            s->timecmp = timecmp;
            s->plic_pending_irq = pending;
            s->plic_served_irq = served;
            s->htif_tohost = tohost;
            s->htif_fromhost = fromhost;
            s->htif_kbd_wanted = kbd_wanted;
            s->reset_pending = FALSE;
            s->halted = FALSE;
        } else if (check_only) {
            return 0;
        }
    }
    return 0;
}

/* the RAM regions, in a fixed order: the main RAM, then the low RAM (boot
   ROM and device tree) */
static uint8_t *riscv_machine_get_ram(VirtMachine *s1, int idx,
                                      uint64_t *psize)
{
    RISCVMachine *s = (RISCVMachine *)s1;
    switch(idx) {
    case 0:
        *psize = s->ram_size;
        return get_ram_ptr(s, RAM_BASE_ADDR, TRUE);
    case 1:
        *psize = LOW_RAM_SIZE;
        return get_ram_ptr(s, 0, TRUE);
    default:
        *psize = 0;
        return NULL;
    }
}

static void riscv_machine_vm_reset(VirtMachine *s1)
{
    riscv_machine_reset((RISCVMachine *)s1);
}

static void riscv_machine_end(VirtMachine *s1)
{
    RISCVMachine *s = (RISCVMachine *)s1;
    /* XXX: stop all */
    riscv_cpu_end(s->cpu_state);
    phys_mem_map_end(s->mem_map);
    free(s->bios_buf);
    free(s->kernel_buf);
    free(s->initrd_buf);
    free(s->cmdline);
    free(s);
}

/* in ms */
static int riscv_machine_get_sleep_duration(VirtMachine *s1, int delay)
{
    RISCVMachine *m = (RISCVMachine *)s1;
    RISCVCPUState *s = m->cpu_state;
    int64_t delay1;
    
    if (m->halted)
        return delay;
    if (m->reset_pending)
        return riscv_machine_reset_ready(m) ? 0 : delay;
    /* wait for an event: the only asynchronous event is the RTC timer */
    if (!(riscv_cpu_get_mip(s) & MIP_MTIP)) {
        delay1 = m->timecmp - rtc_get_time(m);
        if (delay1 <= 0) {
            riscv_cpu_set_mip(s, MIP_MTIP);
            delay = 0;
        } else {
            /* convert delay to ms */
            delay1 = delay1 / (RTC_FREQ / 1000);
            if (delay1 < delay)
                delay = delay1;
        }
    }
    if (!riscv_cpu_get_power_down(s))
        delay = 0;
    return delay;
}

static void riscv_machine_interp(VirtMachine *s1, int max_exec_cycle)
{
    RISCVMachine *s = (RISCVMachine *)s1;
    if (s->halted)
        return;
    if (s->reset_pending) {
        if (!riscv_machine_reset_ready(s))
            return;
        riscv_machine_reset(s);
    }
    riscv_cpu_interp(s->cpu_state, max_exec_cycle);
}

static void riscv_vm_send_key_event(VirtMachine *s1, BOOL is_down,
                                    uint16_t key_code)
{
    RISCVMachine *s = (RISCVMachine *)s1;
    if (s->keyboard_dev) {
        virtio_input_send_key_event(s->keyboard_dev, is_down, key_code);
    }
}

static BOOL riscv_vm_mouse_is_absolute(VirtMachine *s)
{
    return TRUE;
}

static void riscv_vm_send_mouse_event(VirtMachine *s1, int dx, int dy, int dz,
                                      unsigned int buttons)
{
    RISCVMachine *s = (RISCVMachine *)s1;
    if (s->mouse_dev) {
        virtio_input_send_mouse_event(s->mouse_dev, dx, dy, dz, buttons);
    }
}

const VirtMachineClass riscv_machine_class = {
    "riscv32,riscv64,riscv128",
    riscv_machine_set_defaults,
    riscv_machine_init,
    riscv_machine_end,
    riscv_machine_get_sleep_duration,
    riscv_machine_interp,
    riscv_vm_mouse_is_absolute,
    riscv_vm_send_mouse_event,
    riscv_vm_send_key_event,
    riscv_machine_save_state,
    riscv_machine_load_state,
    riscv_machine_get_ram,
    riscv_machine_vm_reset,
};
