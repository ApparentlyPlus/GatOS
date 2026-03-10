/*
 * xhci.c - eXtensible Host Controller Interface Driver
 */

#include <kernel/drivers/xhci.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/usb.h>
#include <kernel/drivers/input.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/sys/apic.h>
#include <kernel/sys/spinlock.h>
#include <kernel/sys/timers.h>
#include <kernel/debug.h>
#include <klibc/string.h>
#include <kernel/sys/panic.h>

static uint8_t *g_cap, *g_op, *g_rt;
static uint32_t *g_db;
static uint32_t g_slots, g_ports, g_ctx_sz;

static uint64_t *g_dcbaa;
static uint64_t g_dcbaa_phys;

static ring_t g_cmd;
static ring_t g_evt;
static erst_t *g_erst;
static uint64_t g_erst_phys;

static xhci_slot_t g_kbd;
static spinlock_t g_lock;

static inline uint32_t cr32(uint32_t o) { return *(volatile uint32_t *)(g_cap + o); }
static inline uint8_t cr8(uint32_t o) { return *(volatile uint8_t *)(g_cap + o); }
static inline uint32_t or32(uint32_t o) { return *(volatile uint32_t *)(g_op + o); }
static inline void ow32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_op + o) = v; }
static inline void ow64(uint32_t o, uint64_t v) { ow32(o, (uint32_t)v); ow32(o + 4, (uint32_t)(v >> 32)); }
static inline uint32_t rr32(uint32_t o) { return *(volatile uint32_t *)(g_rt + o); }
static inline void rw32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_rt + o) = v; }
static inline void rw64(uint32_t o, uint64_t v) { rw32(o, (uint32_t)v); rw32(o + 4, (uint32_t)(v >> 32)); }
static inline uint32_t ir32(uint32_t r) { return rr32(XHCI_IR0 + r); }
static inline void iw32(uint32_t r, uint32_t v) { rw32(XHCI_IR0 + r, v); }
static inline void iw64(uint32_t r, uint64_t v) { rw64(XHCI_IR0 + r, v); }
static inline void dbw(uint32_t s, uint32_t v) { *(volatile uint32_t *)(g_db + s) = v; }

static void *dma_alloc(size_t sz, uint64_t *phys) {
    sz = align_up(sz, PAGE_SIZE);
    if (pmm_alloc(sz, phys) != PMM_OK) panicf("[XHCI] OOM %zu\n", sz);
    void *v = (void *)PHYSMAP_P2V(*phys);
    kmemset(v, 0, sz);
    return v;
}

static void ring_init(ring_t *r, uint32_t cnt) {
    r->trbs = dma_alloc(cnt * sizeof(trb_t), &r->phys);
    r->enq = r->deq = 0;
    r->cyc = 1;
    trb_t *link = &r->trbs[cnt - 1];
    link->dw0 = (uint32_t)r->phys;
    link->dw1 = (uint32_t)(r->phys >> 32);
    link->dw2 = 0;
    link->ctrl = TRB_TYPE(TRB_LINK) | TRB_TC | r->cyc;
}

static void enq(ring_t *r, uint32_t cnt, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t c) {
    trb_t *t = &r->trbs[r->enq];
    t->dw0 = d0;
    t->dw1 = d1;
    t->dw2 = d2;
    __asm__ volatile("" ::: "memory");
    t->ctrl = c | (r->cyc ? TRB_C : 0);
    if (++r->enq == cnt - 1) {
        trb_t *l = &r->trbs[cnt - 1];
        l->ctrl = TRB_TYPE(TRB_LINK) | TRB_TC | (r->cyc ? TRB_C : 0);
        r->cyc ^= 1;
        r->enq = 0;
    }
}

static void evt_init(void) {
    g_evt.trbs = dma_alloc(ERING_SZ * sizeof(trb_t), &g_evt.phys);
    g_evt.deq = 0;
    g_evt.cyc = 1;
    g_erst = dma_alloc(sizeof(erst_t), &g_erst_phys);
    g_erst[0].base = g_evt.phys;
    g_erst[0].size = ERING_SZ;
    iw32(IR_IMOD, 0);
    iw32(IR_ERSTSZ, 1);
    iw64(IR_ERSTBA_LO, g_erst_phys);
    iw64(IR_ERDP_LO, g_evt.phys | ERDP_EHB);
    iw32(IR_IMAN, ir32(IR_IMAN) | IMAN_IE | IMAN_IP);
}

static trb_t *deq(void) {
    trb_t *t = &g_evt.trbs[g_evt.deq];
    __asm__ volatile("" ::: "memory");
    if ((t->ctrl & TRB_C) != (uint32_t)g_evt.cyc) return NULL;
    if (++g_evt.deq >= ERING_SZ) {
        g_evt.deq = 0;
        g_evt.cyc ^= 1;
    }
    iw64(IR_ERDP_LO, (g_evt.phys + g_evt.deq * sizeof(trb_t)) | ERDP_EHB);
    return t;
}

static trb_t wait_ev(uint8_t type, uint32_t tmo) {
    trb_t res = {0};
    uint64_t dl = get_uptime_ms() + tmo;
    while (get_uptime_ms() < dl) {
        bool was = spinlock_acquire(&g_lock);
        trb_t *t = deq();
        if (t) {
            uint8_t ty = GET_TYPE(t->ctrl);
            res = *t;
            spinlock_release(&g_lock, was);
            ow32(XHCI_STS, STS_EINT);
            iw32(IR_IMAN, ir32(IR_IMAN) | IMAN_IP);
            if (ty == type) return res;
            kmemset(&res, 0, sizeof(res));
        } else {
            spinlock_release(&g_lock, was);
        }
        __asm__ volatile("pause");
    }
    return res;
}

static void bios_handoff(void) {
    uint32_t xecp = HCC1_XECP(cr32(XHCI_HCCPARAMS1));
    if (!xecp) return;
    uint8_t *cap = g_cap + xecp * 4;
    for (int i = 0; i < 256; i++) {
        if (*cap == EXCAP_LEGACY) {
            volatile uint32_t *reg = (volatile uint32_t *)cap;
            *reg |= LEG_OS_SEM;
            for (int j = 0; j < 1000; j++) {
                if (!(*reg & LEG_BIOS_SEM)) break;
                sleep_ms(1);
            }
            *(volatile uint32_t *)(cap + 4) &= ~0x1F;
            return;
        }
        if (!cap[1]) break;
        cap += cap[1] * 4;
    }
}

static bool reset_hc(void) {
    ow32(XHCI_CMD, or32(XHCI_CMD) & ~CMD_RS);
    for (int i = 0; i < 100; i++) {
        if (or32(XHCI_STS) & STS_HCH) break;
        sleep_ms(1);
    }
    ow32(XHCI_CMD, or32(XHCI_CMD) | CMD_HCRST);
    for (int i = 0; i < 100; i++) {
        sleep_ms(1);
        if (!(or32(XHCI_CMD) & CMD_HCRST) && !(or32(XHCI_STS) & STS_CNR)) return true;
    }
    return false;
}

static void start_hc(void) {
    ow32(XHCI_CMD, or32(XHCI_CMD) | CMD_RS | CMD_INTE | CMD_HSEE);
    for (int i = 0; i < 100; i++) {
        if (!(or32(XHCI_STS) & STS_HCH)) break;
        sleep_ms(1);
    }
    for (int i = 0; i < 500; i++) {
        if (!(or32(XHCI_STS) & STS_CNR)) return;
        sleep_ms(1);
    }
}

static uint8_t cmd_slot(void) {
    enq(&g_cmd, RING_SZ, 0, 0, 0, TRB_TYPE(TRB_EN_SLOT));
    dbw(0, 0);
    trb_t ev = wait_ev(TRB_EV_CMD, 500);
    return GET_CC(ev.dw2) == CC_SUCCESS ? GET_SLOT(ev.ctrl) : 0;
}

static bool cmd_addr(uint64_t ctx, uint8_t slot, bool bsr) {
    enq(&g_cmd, RING_SZ, (uint32_t)ctx, (uint32_t)(ctx >> 32), 0, TRB_TYPE(TRB_ADDR_DEV) | TRB_SLOT(slot) | (bsr ? TRB_BSR : 0));
    dbw(0, 0);
    return GET_CC(wait_ev(TRB_EV_CMD, 500).dw2) == CC_SUCCESS;
}

static bool cmd_cfg(uint64_t ctx, uint8_t slot) {
    enq(&g_cmd, RING_SZ, (uint32_t)ctx, (uint32_t)(ctx >> 32), 0, TRB_TYPE(TRB_CFG_EP) | TRB_SLOT(slot));
    dbw(0, 0);
    return GET_CC(wait_ev(TRB_EV_CMD, 500).dw2) == CC_SUCCESS;
}

static bool cmd_eval(uint64_t ctx, uint8_t slot) {
    enq(&g_cmd, RING_SZ, (uint32_t)ctx, (uint32_t)(ctx >> 32), 0, TRB_TYPE(TRB_EVAL_CTX) | TRB_SLOT(slot));
    dbw(0, 0);
    return GET_CC(wait_ev(TRB_EV_CMD, 500).dw2) == CC_SUCCESS;
}

static inline ctrl_ctx_t *get_ctrl(void *c) { return c; }
static inline slot_ctx_t *get_slot(void *c) { return (void *)((uint8_t *)c + g_ctx_sz); }
static inline ep_ctx_t *get_ep(void *c, uint8_t i) { return (void *)((uint8_t *)c + g_ctx_sz * (2 + i)); }

static int ctrl_xfer(xhci_slot_t *s, usb_setup_t *req, uint64_t buf) {
    bool in = req->bmRequestType & USB_DIR_IN;
    uint32_t d0, d1;
    kmemcpy(&d0, req, 4);
    kmemcpy(&d1, (uint8_t *)req + 4, 4);
    uint32_t trt = req->wLength ? (in ? 3 << 16 : 2 << 16) : 0;
    enq(&s->ep0, RING_SZ, d0, d1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
    if (req->wLength) enq(&s->ep0, RING_SZ, (uint32_t)buf, (uint32_t)(buf >> 32), req->wLength, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    enq(&s->ep0, RING_SZ, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (in && req->wLength ? 0 : TRB_DIR_IN));
    dbw(s->id, 1);
    trb_t ev = wait_ev(TRB_EV_XFER, 500);
    uint8_t cc = GET_CC(ev.dw2);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) return -1;
    return req->wLength - (ev.dw2 & 0xFFFFFF);
}

static uint8_t *g_scratch;
static uint64_t g_scratch_phys;

static int get_desc(xhci_slot_t *s, uint8_t ty, uint8_t idx, uint16_t len) {
    usb_setup_t r = { USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, (uint16_t)((ty << 8) | idx), 0, len };
    return ctrl_xfer(s, &r, g_scratch_phys);
}

static int set_cfg(xhci_slot_t *s, uint8_t v) {
    usb_setup_t r = { USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, v, 0, 0 };
    return ctrl_xfer(s, &r, 0);
}

static int set_proto(xhci_slot_t *s, uint8_t i, uint8_t p) {
    usb_setup_t r = { USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_HID_REQ_SET_PROTOCOL, p, i, 0 };
    return ctrl_xfer(s, &r, 0);
}

static int set_idle(xhci_slot_t *s, uint8_t i) {
    usb_setup_t r = { USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_HID_REQ_SET_IDLE, 0, i, 0 };
    return ctrl_xfer(s, &r, 0);
}

static bool parse_cfg(xhci_slot_t *s, uint8_t *buf, uint16_t len) {
    uint16_t p = 0;
    uint8_t iface = 0xFF;
    bool is_kbd = false;
    while (p < len) {
        if (buf[p] < 2 || p + buf[p] > len) break;
        if (buf[p + 1] == USB_DESC_CONFIG) {
            s->cfg_val = ((usb_config_desc_t *)(buf + p))->bConfigurationValue;
        } else if (buf[p + 1] == USB_DESC_INTERFACE) {
            usb_interface_desc_t *id = (void *)(buf + p);
            is_kbd = id->bInterfaceClass == USB_CLASS_HID && id->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT && id->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD;
            iface = id->bInterfaceNumber;
        } else if (buf[p + 1] == USB_DESC_ENDPOINT) {
            usb_endpoint_desc_t *ed = (void *)(buf + p);
            if (is_kbd && (ed->bEndpointAddress & USB_EP_DIR_IN) && (ed->bmAttributes & 3) == USB_EP_TYPE_INTERRUPT) {
                s->iface = iface;
                s->ep_addr = ed->bEndpointAddress;
                s->ep_mps = ed->wMaxPacketSize & 0x7FF;
                s->ep_ival = ed->bInterval;
                return true; /* Found it, no need to parse the rest of the composite device */
            }
        }
        p += buf[p];
    }
    return false;
}

static uint16_t get_mps(uint8_t spd) {
    return (spd == SPD_SS || spd == SPD_SSP) ? 512 : (spd == SPD_LS ? 8 : 64);
}

static bool reset_port(uint8_t p) {
    uint32_t sc = or32(XHCI_PORTSC(p));
    ow32(XHCI_PORTSC(p), (sc & PORT_PRESERVE) | PORT_RW1C);
    ow32(XHCI_PORTSC(p), (or32(XHCI_PORTSC(p)) & PORT_PRESERVE) | PORT_PR);
    for (int i = 0; i < 200; i++) {
        sleep_ms(1);
        if (or32(XHCI_PORTSC(p)) & PORT_PRC) {
            ow32(XHCI_PORTSC(p), (or32(XHCI_PORTSC(p)) & PORT_PRESERVE) | PORT_PRC);
            return true;
        }
    }
    return false;
}

static bool enum_dev(uint8_t p, uint8_t spd) {
    xhci_slot_t *s = &g_kbd;
    kmemset(s, 0, sizeof(*s));
    s->port = p;
    s->spd = spd;
    if (!(s->id = cmd_slot())) return false;

    s->out_phys = 0;
    void *out = dma_alloc(PAGE_SIZE, &s->out_phys);
    g_dcbaa[s->id] = s->out_phys;
    ring_init(&s->ep0, RING_SZ);

    uint64_t in_phys = 0;
    void *in = dma_alloc(PAGE_SIZE, &in_phys);
    ctrl_ctx_t *c = get_ctrl(in);
    c->add = 3;
    slot_ctx_t *sc = get_slot(in);
    sc->dw0 = SLOT_SPD(spd) | SLOT_CTX_ENT(1);
    sc->dw1 = SLOT_PORT(p + 1);
    ep_ctx_t *e0 = get_ep(in, 0);
    uint16_t mps = get_mps(spd);
    e0->dw1 = EP_CERR(3) | EP_TYPE(EP_CTRL) | EP_PKT(mps);
    e0->deq_lo = (uint32_t)s->ep0.phys | EP_DCS;
    e0->deq_hi = (uint32_t)(s->ep0.phys >> 32);
    e0->dw4 = EP_AVG(8);

    if (!cmd_addr(in_phys, s->id, false)) goto fail;
    if (get_desc(s, USB_DESC_DEVICE, 0, 8) < 8) goto fail;
    
    usb_device_desc_t *dd = (void *)g_scratch;
    uint16_t real_mps = (spd == SPD_SS || spd == SPD_SSP) ? (1 << dd->bMaxPacketSize0) : dd->bMaxPacketSize0;
    if (real_mps != mps) {
        kmemset(in, 0, PAGE_SIZE);
        c->add = 2;
        e0->dw1 = EP_CERR(3) | EP_TYPE(EP_CTRL) | EP_PKT(real_mps);
        e0->deq_lo = (uint32_t)s->ep0.phys | EP_DCS;
        e0->deq_hi = (uint32_t)(s->ep0.phys >> 32);
        e0->dw4 = EP_AVG(8);
        cmd_eval(in_phys, s->id);
    }

    int n = get_desc(s, USB_DESC_CONFIG, 0, 2048);
    if (n <= 0 || !parse_cfg(s, g_scratch, n)) goto fail;
    if (set_cfg(s, s->cfg_val) < 0) goto fail;

    ring_init(&s->intr, RING_SZ);
    s->ep_idx = (s->ep_addr & 0xF) * 2 + 1;

    kmemset(in, 0, PAGE_SIZE);
    c->add = 1 | (1 << s->ep_idx);
    kmemcpy(get_slot(in), (void *)PHYSMAP_P2V(s->out_phys), g_ctx_sz);
    get_slot(in)->dw0 = (get_slot(in)->dw0 & ~(0x1F << 27)) | SLOT_CTX_ENT(s->ep_idx);
    
    ep_ctx_t *ei = get_ep(in, s->ep_idx - 1);
    uint8_t iv = (spd >= SPD_HS) ? (s->ep_ival ? s->ep_ival - 1 : 0) : s->ep_ival;
    ei->dw0 = EP_IVAL(iv);
    ei->dw1 = EP_CERR(3) | EP_TYPE(EP_INTR_IN) | EP_PKT(s->ep_mps);
    ei->deq_lo = (uint32_t)s->intr.phys | EP_DCS;
    ei->deq_hi = (uint32_t)(s->intr.phys >> 32);
    ei->dw4 = EP_AVG(s->ep_mps) | EP_ESIT(s->ep_mps);

    if (!cmd_cfg(in_phys, s->id)) goto fail;
    set_proto(s, s->iface, USB_HID_PROTOCOL_BOOT);
    set_idle(s, s->iface);

    s->hid_buf = dma_alloc(PAGE_SIZE, &s->hid_phys);
    pmm_free(in_phys, PAGE_SIZE);
    s->active = true;
    return true;

fail:
    if (in_phys) pmm_free(in_phys, PAGE_SIZE);
    if (s->ep0.phys) pmm_free(s->ep0.phys, PAGE_SIZE);
    if (s->intr.phys) pmm_free(s->intr.phys, PAGE_SIZE);
    if (s->out_phys) { pmm_free(s->out_phys, PAGE_SIZE); g_dcbaa[s->id] = 0; }
    return false;
}

static const keycode_t kmap[116] = {
    KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS, KEY_EQUAL, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_BACKSLASH, KEY_UNKNOWN, KEY_SEMICOLON, KEY_QUOTE, KEY_BACKTICK, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_UNKNOWN, KEY_SCROLLLOCK, KEY_UNKNOWN, KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END, KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP, KEY_NUMLOCK, KEY_KPSLASH, KEY_KPMULT, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KP0, KEY_KPDOT, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN
};

static const uint8_t mmap[8] = { MOD_LCTRL, MOD_LSHIFT, MOD_LALT, MOD_LGUI, MOD_RCTRL, MOD_RSHIFT, MOD_RALT, MOD_RGUI };

static void handle_hid(const uint8_t *r) {
    uint8_t nmod = 0, c = r[0] ^ g_kbd.prev[0];
    for (int i = 0; i < 8; i++) if (r[0] & (1 << i)) nmod |= mmap[i];

    if (c) {
        for (int i = 0; i < 8; i++) {
            if (!(c & (1 << i))) continue;
            keycode_t k = (i == 0) ? KEY_LEFT_CTRL : (i == 1) ? KEY_LEFT_SHIFT : (i == 2) ? KEY_LEFT_ALT : (i == 3) ? KEY_LEFT_GUI : (i == 4) ? KEY_RIGHT_CTRL : (i == 5) ? KEY_RIGHT_SHIFT : (i == 6) ? KEY_RIGHT_ALT : (i == 7) ? KEY_RIGHT_GUI : KEY_UNKNOWN;
            if (k != KEY_UNKNOWN) input_handle_key((key_event_t){ k, (r[0] & (1 << i)) != 0, nmod, 0 });
        }
    }

    for (int i = 2; i < 8; i++) {
        if (g_kbd.prev[i]) {
            bool found = false;
            for (int j = 2; j < 8; j++) if (r[j] == g_kbd.prev[i]) found = true;
            if (!found && g_kbd.prev[i] < 116 && kmap[g_kbd.prev[i]] != KEY_UNKNOWN)
                input_handle_key((key_event_t){ kmap[g_kbd.prev[i]], false, nmod, 0 });
        }
        if (r[i]) {
            bool found = false;
            for (int j = 2; j < 8; j++) if (g_kbd.prev[j] == r[i]) found = true;
            if (!found && r[i] < 116 && kmap[r[i]] != KEY_UNKNOWN)
                input_handle_key((key_event_t){ kmap[r[i]], true, nmod, 0 });
        }
    }
    kmemcpy(g_kbd.prev, r, 8);
}

static void arm_int(void) {
    enq(&g_kbd.intr, RING_SZ, (uint32_t)g_kbd.hid_phys, (uint32_t)(g_kbd.hid_phys >> 32), 8, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_INTR(0));
    dbw(g_kbd.id, g_kbd.ep_idx);
}

static void proc_evts(void) {
    for (;;) {
        bool was = spinlock_acquire(&g_lock);
        trb_t *t = deq();
        if (!t) { spinlock_release(&g_lock, was); break; }
        trb_t ev = *t;
        spinlock_release(&g_lock, was);
        
        ow32(XHCI_STS, STS_EINT);
        iw32(IR_IMAN, ir32(IR_IMAN) | IMAN_IP);
        
        if (GET_TYPE(ev.ctrl) == TRB_EV_XFER) {
            uint8_t cc = GET_CC(ev.dw2);
            if (GET_SLOT(ev.ctrl) == g_kbd.id && GET_EP(ev.ctrl) == g_kbd.ep_idx && g_kbd.active) {
                if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) handle_hid(g_kbd.hid_buf);
                arm_int();
            }
        }
    }
}

cpu_context_t *xhci_irq_handler(cpu_context_t *ctx) {
    proc_evts();
    lapic_eoi();
    return ctx;
}

bool xhci_init(void) {
    pci_dev_t pci;
    if (!pci_find_xhci(&pci)) return false;
    pci_enable(&pci);

    uint32_t ms = align_up(pci.bar0_size, PAGE_SIZE);
    if (ms < PAGE_SIZE) ms = PAGE_SIZE;
    if (vmm_alloc(NULL, ms, VM_FLAG_WRITE | VM_FLAG_MMIO, (void *)pci.bar0_phys, (void **)&g_cap) != VMM_OK) return false;

    g_op = g_cap + cr8(XHCI_CAPLEN);
    g_slots = HCS1_SLOTS(cr32(XHCI_HCSPARAMS1));
    g_ports = HCS1_PORTS(cr32(XHCI_HCSPARAMS1));
    g_ctx_sz = HCC1_CSZ(cr32(XHCI_HCCPARAMS1)) ? 64 : 32;
    g_rt = g_cap + (cr32(XHCI_RTSOFF) & ~0x1F);
    g_db = (uint32_t *)(g_cap + (cr32(XHCI_DBOFF) & ~3));

    bios_handoff();
    if (!reset_hc()) return false;
    spinlock_init(&g_lock, "xhci_evts");

    g_dcbaa = dma_alloc(align_up((g_slots + 1) * 8, 64), &g_dcbaa_phys);
    uint32_t scratches = HCS2_SCRATCH(cr32(XHCI_HCSPARAMS2));
    if (scratches) {
        uint64_t spa;
        uint64_t *spa_buf = dma_alloc(scratches * 8, &spa);
        for (uint32_t i = 0; i < scratches; i++) dma_alloc(PAGE_SIZE, &spa_buf[i]);
        g_dcbaa[0] = spa;
    }

    ow32(XHCI_CONFIG, g_slots & 0xFF);
    ow64(XHCI_DCBAAP_LO, g_dcbaa_phys);
    ring_init(&g_cmd, RING_SZ);
    ow64(XHCI_CRCR_LO, (g_cmd.phys & ~0x3F) | g_cmd.cyc);
    evt_init();

    irq_register(XHCI_MSI_VEC, (irq_handler_t)xhci_irq_handler);
    pci_cfg_msi(&pci, XHCI_MSI_VEC, lapic_get_id());

    start_hc();
    g_scratch = dma_alloc(PAGE_SIZE, &g_scratch_phys);

    /* QEMU usb-host and some real hardware can take a few hundred milliseconds 
       to report a device connection after the controller starts. We poll for up to 500ms. */
    bool kbd_found = false;
    for (int retry = 0; retry < 10 && !kbd_found; retry++) {
        for (uint8_t p = 0; p < g_ports && !kbd_found; p++) {
            uint32_t sc = or32(XHCI_PORTSC(p));
            if (!(sc & PORT_CCS)) continue;
            
            LOGF("[XHCI] port %u: device present, resetting...\n", p);
            if (!reset_port(p)) {
                LOGF("[XHCI] port %u: reset failed\n", p);
                continue;
            }
            
            sc = or32(XHCI_PORTSC(p));
            LOGF("[XHCI] port %u: after reset PORTSC=0x%x\n", p, sc);
            if (!(sc & PORT_CCS)) continue;
            
            uint8_t spd = (sc >> PORT_SPD_SHIFT) & PORT_SPD_MASK;
            if (!spd) spd = SPD_FS;
            
            LOGF("[XHCI] port %u: enumerating speed=%u\n", p, spd);
            sleep_ms(50);
            
            kbd_found = enum_dev(p, spd);
            if (!kbd_found) {
                LOGF("[XHCI] port %u: enum_dev returned false\n", p);
            }
        }
        if (!kbd_found) sleep_ms(50);
    }

    if (!kbd_found) {
        LOGF("[XHCI] No USB xHCI keyboard found\n");
        return false;
    }
    arm_int();
    return true;
}
