/*
 * xhci.c - eXtensible Host Controller Interface Driver
 *
 * Features:
 * 
 * - Full xHCI 1.1 compliance with support for USB 2.0 and 3.0 devices
 * - BIOS handoff and Intel USB port routing quirks
 * - TRB-based command submission and event handling
 * - Dynamic device slot allocation and context management
 * - Control transfer support for device enumeration and configuration
 * - Hub enumeration and downstream port management
 * - Integration with input subsystem for USB keyboards
 * - Robust error handling and timeouts for hardware interactions
 * - DMA memory management with kernel address mapping
 * - Spinlock synchronization for concurrent access to shared data structures
 * 
 * Author: u/ApparentlyPlus
 */

#include <kernel/drivers/xhci.h>
#include <kernel/drivers/pci.h>
#include <kernel/drivers/usb.h>
#include <kernel/drivers/input.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/memory/heap.h>
#include <arch/x86_64/memory/paging.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/sys/apic.h>
#include <kernel/sys/spinlock.h>
#include <kernel/sys/timers.h>
#include <kernel/debug.h>
#include <klibc/stdio.h>
#include <klibc/string.h>
#include <kernel/sys/panic.h>

static xhci_hc_t *hcs[16];
static int hc_cnt = 0;

static inline uint32_t cr32(xhci_hc_t *hc, uint32_t o) { return *(volatile uint32_t *)(hc->cap + o); }
static inline uint8_t  cr8 (xhci_hc_t *hc, uint32_t o) { return *(volatile uint8_t  *)(hc->cap + o); }
static inline uint32_t or32(xhci_hc_t *hc, uint32_t o) { return *(volatile uint32_t *)(hc->op  + o); }
static inline void     ow32(xhci_hc_t *hc, uint32_t o, uint32_t v) { *(volatile uint32_t *)(hc->op + o) = v; }
static inline void     ow64(xhci_hc_t *hc, uint32_t o, uint64_t v) { ow32(hc, o, (uint32_t)v); ow32(hc, o + 4, (uint32_t)(v >> 32)); }
static inline uint32_t rr32(xhci_hc_t *hc, uint32_t o) { return *(volatile uint32_t *)(hc->rt  + o); }
static inline void     rw32(xhci_hc_t *hc, uint32_t o, uint32_t v) { *(volatile uint32_t *)(hc->rt + o) = v; }
static inline void     rw64(xhci_hc_t *hc, uint32_t o, uint64_t v) { rw32(hc, o, (uint32_t)v); rw32(hc, o + 4, (uint32_t)(v >> 32)); }
static inline uint32_t ir32(xhci_hc_t *hc, uint32_t r) { return rr32(hc, XHCI_IR0 + r); }
static inline void     iw32(xhci_hc_t *hc, uint32_t r, uint32_t v) { rw32(hc, XHCI_IR0 + r, v); }
static inline void     iw64(xhci_hc_t *hc, uint32_t r, uint64_t v) { rw64(hc, XHCI_IR0 + r, v); }
static inline void     dbw (xhci_hc_t *hc, uint32_t s, uint32_t v) { *(volatile uint32_t *)(hc->db  + s) = v; }

/*
 * assert_dma_clean - Ensures that allocated DMA memory does not overlap with the kernel image
 */
static void assert_dma_clean(uint64_t phys, size_t sz, const char *name) {
    uint64_t kstart = get_kstart(false);
    uint64_t kend = get_kend(false);
    uint64_t end = phys + sz;
    if (!(end <= kstart || phys >= kend)) {
        panicf("[XHCI] DMA %s overlaps kernel image: [0x%lx..0x%lx) vs [0x%lx..0x%lx)\n",
               name, phys, end, kstart, kend);
    }
}

/*
 * dma_alloc - Allocates contiguous physical memory for DMA and returns its virtual address
 */
static void *dma_alloc(xhci_hc_t *hc, size_t sz, uint64_t *phys) {
    sz = align_up(sz, PAGE_SIZE);
    if (pmm_alloc(sz, phys) != PMM_OK)
        panicf("[XHCI] DMA OOM (%zu bytes)\n", sz);
    if (!hc->ac64 && *phys >= 0x100000000ULL)
        panicf("[XHCI] DMA alloc returned >4GiB (0x%lx) on 32-bit controller\n", *phys);
    void *v = (void *)PHYSMAP_P2V(*phys);
    kmemset(v, 0, sz);
    assert_dma_clean(*phys, sz, "alloc");
    return v;
}

/*
 * ring_init - set up a TRB ring; Link TRB at tail keeps it circular
 */
static void ring_init(xhci_hc_t *hc, ring_t *r, uint32_t cnt) {
    r->trbs = dma_alloc(hc, cnt * sizeof(trb_t), &r->phys);
    r->enq = r->deq = 0;
    r->cyc = 1;
    trb_t *link = &r->trbs[cnt - 1];
    link->dw0 = (uint32_t)r->phys;
    link->dw1 = (uint32_t)(r->phys >> 32);
    link->dw2 = 0;
    link->ctrl = TRB_TYPE(TRB_LINK) | TRB_TC | r->cyc;
}

/*
 * enq - post a TRB to a ring, wrapping with a Link at [cnt-1]
 */
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

/*
 * evt_init - Initializes the event ring and event ring segment table for the host controller
 */
static void evt_init(xhci_hc_t *hc) {
    hc->evt.trbs = dma_alloc(hc, ERING_SZ * sizeof(trb_t), &hc->evt.phys);
    hc->evt.deq = 0;
    hc->evt.cyc = 1;
    hc->erst = dma_alloc(hc, sizeof(erst_t), &hc->erst_phys);
    hc->erst[0].base = hc->evt.phys;
    hc->erst[0].size = ERING_SZ;
    iw32(hc, IR_IMOD, 0);
    iw32(hc, IR_ERSTSZ, 1);
    iw64(hc, IR_ERSTBA_LO, hc->erst_phys);
    iw64(hc, IR_ERDP_LO, hc->evt.phys | ERDP_EHB);
    iw32(hc, IR_IMAN, ir32(hc, IR_IMAN) | IMAN_IE | IMAN_IP);
}

/*
 * deq - pop next event if the cycle bit matches, advance ERDP
 */
static trb_t *deq(xhci_hc_t *hc) {
    trb_t *t = &hc->evt.trbs[hc->evt.deq];
    __asm__ volatile("" ::: "memory");
    if ((t->ctrl & TRB_C) != (uint32_t)hc->evt.cyc) return NULL;
    if (++hc->evt.deq >= ERING_SZ) {
        hc->evt.deq = 0;
        hc->evt.cyc ^= 1;
    }
    iw64(hc, IR_ERDP_LO, (hc->evt.phys + hc->evt.deq * sizeof(trb_t)) | ERDP_EHB);
    return t;
}

/*
 * wait_ev - spin on the event ring until we see the expected TRB type or timeout
 */
static trb_t wait_ev(xhci_hc_t *hc, uint8_t type, uint32_t tmo) {
    trb_t res = {0};
    uint64_t dl = get_uptime_ms() + tmo;
    while (get_uptime_ms() < dl) {
        bool was = spinlock_acquire(&hc->lock);
        trb_t *t = deq(hc);
        if (t) {
            uint8_t ty = GET_TYPE(t->ctrl);
            res = *t;
            spinlock_release(&hc->lock, was);
            ow32(hc, XHCI_STS, STS_EINT);
            iw32(hc, IR_IMAN, ir32(hc, IR_IMAN) | IMAN_IP);
            if (ty == type) return res;
            kmemset(&res, 0, sizeof(res));
        } else {
            spinlock_release(&hc->lock, was);
        }
        __asm__ volatile("pause");
    }
    return res;
}

/*
 * bios_handoff - hand off xHCI ownership from BIOS; also reroutes Intel USB2 ports to xHCI
 */
static void bios_handoff(xhci_hc_t *hc, pci_dev_t *pci) {
    // Intel Port Routing (Panther Point / Lynx Point quirks)
    // Routes switchable USB 2.0/3.0 ports from EHCI to xHCI

    // I am not really proud of this code xD
    if (pci->vendor_id == 0x8086) {
        uint32_t usb3_mask = pci_read32(pci, 0xDC);
        pci_write32(pci, 0xD8, usb3_mask);
        uint32_t usb2_mask = pci_read32(pci, 0xD4);
        pci_write32(pci, 0xD0, usb2_mask);
    }

    uint32_t xecp = HCC1_XECP(cr32(hc, XHCI_HCCPARAMS1));
    if (!xecp) return;
    uint8_t *cap = hc->cap + xecp * 4;
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

/*
 * reset_hc - stop the HC then issue a full reset — wait up to 100ms each
 */
static bool reset_hc(xhci_hc_t *hc) {
    ow32(hc, XHCI_CMD, or32(hc, XHCI_CMD) & ~CMD_RS);
    for (int i = 0; i < 100; i++) {
        if (or32(hc, XHCI_STS) & STS_HCH) break;
        sleep_ms(1);
    }
    ow32(hc, XHCI_CMD, or32(hc, XHCI_CMD) | CMD_HCRST);
    for (int i = 0; i < 100; i++) {
        sleep_ms(1);
        if (!(or32(hc, XHCI_CMD) & CMD_HCRST) && !(or32(hc, XHCI_STS) & STS_CNR)) return true;
    }
    return false;
}

/*
 * start_hc - Starts the host controller by setting the run/stop bit and enabling interrupts
 */
static void start_hc(xhci_hc_t *hc) {
    ow32(hc, XHCI_CMD, or32(hc, XHCI_CMD) | CMD_RS | CMD_INTE | CMD_HSEE);
    for (int i = 0; i < 100; i++) {
        if (!(or32(hc, XHCI_STS) & STS_HCH)) break;
        sleep_ms(1);
    }
    for (int i = 0; i < 500; i++) {
        if (!(or32(hc, XHCI_STS) & STS_CNR)) return;
        sleep_ms(1);
    }
}

/*
 * cmd_slot - allocate a device slot, return slot_id or 0 on fail
 */
static uint8_t cmd_slot(xhci_hc_t *hc) {
    enq(&hc->cmd, RING_SZ, 0, 0, 0, TRB_TYPE(TRB_EN_SLOT));
    dbw(hc, 0, 0);
    trb_t ev = wait_ev(hc, TRB_EV_CMD, 500);
    uint8_t cc = GET_CC(ev.dw2);
    uint8_t id = GET_SLOT(ev.ctrl);
    if (cc != CC_SUCCESS) LOGF("[XHCI] cmd_slot failed: cc=%u\n", cc);
    return cc == CC_SUCCESS ? id : 0;
}

/*
 * cmd_addr - address a slot (bsr=true skips SET_ADDRESS for hubs)
 */
static bool cmd_addr(xhci_hc_t *hc, uint64_t ctx, uint8_t slot, bool bsr) {
    enq(&hc->cmd, RING_SZ, (uint32_t)ctx, (uint32_t)(ctx >> 32), 0, TRB_TYPE(TRB_ADDR_DEV) | TRB_SLOT(slot) | (bsr ? TRB_BSR : 0));
    dbw(hc, 0, 0);
    trb_t ev = wait_ev(hc, TRB_EV_CMD, 500);
    uint8_t cc = GET_CC(ev.dw2);
    if (cc != CC_SUCCESS) LOGF("[XHCI] cmd_addr failed: cc=%u (bsr=%d)\n", cc, bsr);
    return cc == CC_SUCCESS;
}

/*
 * cmd_cfg - configure endpoints for a slot
 */
static bool cmd_cfg(xhci_hc_t *hc, uint64_t ctx, uint8_t slot) {
    enq(&hc->cmd, RING_SZ, (uint32_t)ctx, (uint32_t)(ctx >> 32), 0, TRB_TYPE(TRB_CFG_EP) | TRB_SLOT(slot));
    dbw(hc, 0, 0);
    uint8_t cc = GET_CC(wait_ev(hc, TRB_EV_CMD, 500).dw2);
    if (cc != CC_SUCCESS) LOGF("[XHCI] cmd_cfg failed: cc=%u\n", cc);
    return cc == CC_SUCCESS;
}

/*
 * cmd_eval - evaluate context (update MPS etc.)
 */
static bool cmd_eval(xhci_hc_t *hc, uint64_t ctx, uint8_t slot) {
    enq(&hc->cmd, RING_SZ, (uint32_t)ctx, (uint32_t)(ctx >> 32), 0, TRB_TYPE(TRB_EVAL_CTX) | TRB_SLOT(slot));
    dbw(hc, 0, 0);
    uint8_t cc = GET_CC(wait_ev(hc, TRB_EV_CMD, 500).dw2);
    if (cc != CC_SUCCESS) LOGF("[XHCI] cmd_eval failed: cc=%u\n", cc);
    return cc == CC_SUCCESS;
}

/*
 * get_ctrl - Gets the control context from the input context buffer
 */
static inline ctrl_ctx_t *get_ctrl(void *c) { return c; }

/*
 * get_slot - Gets the slot context from the input context buffer based on context size
 */
static inline slot_ctx_t *get_slot(xhci_hc_t *hc, void *c) { return (void *)((uint8_t *)c + hc->ctx_sz); }

/*
 * get_ep - Gets an endpoint context from the input context buffer
 */
static inline ep_ctx_t *get_ep(xhci_hc_t *hc, void *c, uint8_t i) { return (void *)((uint8_t *)c + hc->ctx_sz * (2 + i)); }

/*
 * ctrl_xfer - Performs a standard control transfer on the default control endpoint
 */
static int ctrl_xfer(xhci_hc_t *hc, xhci_slot_t *s, usb_setup_t *req, uint64_t buf) {
    bool in = req->bmRequestType & USB_DIR_IN;
    uint32_t d0, d1;
    kmemcpy(&d0, req, 4);
    kmemcpy(&d1, (uint8_t *)req + 4, 4);
    uint32_t trt = req->wLength ? (in ? 3 << 16 : 2 << 16) : 0;
    enq(&s->ep0, RING_SZ, d0, d1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
    if (req->wLength) enq(&s->ep0, RING_SZ, (uint32_t)buf, (uint32_t)(buf >> 32), req->wLength, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    enq(&s->ep0, RING_SZ, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (in && req->wLength ? 0 : TRB_DIR_IN));
    dbw(hc, s->id, 1);
    trb_t ev = wait_ev(hc, TRB_EV_XFER, 500);
    uint8_t cc = GET_CC(ev.dw2);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) {
        LOGF("[XHCI] ctrl_xfer failed: cc=%u req=0x%02x len=%u\n", cc, req->bRequest, req->wLength);
        return -1;
    }
    return req->wLength - (ev.dw2 & 0xFFFFFF);
}

/*
 * get_desc - Retrieves a USB descriptor from a device
 */
static int get_desc(xhci_hc_t *hc, xhci_slot_t *s, uint8_t ty, uint8_t idx, uint16_t len) {
    usb_setup_t r = { USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, (uint16_t)((ty << 8) | idx), 0, len };
    return ctrl_xfer(hc, s, &r, hc->scratch_phys);
}

/*
 * set_cfg - Sets the active configuration for a USB device
 */
static int set_cfg(xhci_hc_t *hc, xhci_slot_t *s, uint8_t v) {
    usb_setup_t r = { USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, v, 0, 0 };
    return ctrl_xfer(hc, s, &r, 0);
}

/*
 * set_proto - Sets the HID protocol (e.g., Boot Protocol) for a specific interface
 */
static int set_proto(xhci_hc_t *hc, xhci_slot_t *s, uint8_t i, uint8_t p) {
    usb_setup_t r = { USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_HID_REQ_SET_PROTOCOL, p, i, 0 };
    return ctrl_xfer(hc, s, &r, 0);
}

/*
 * set_idle - Sets the idle rate for an interrupt IN endpoint on a HID device
 */
static int set_idle(xhci_hc_t *hc, xhci_slot_t *s, uint8_t i) {
    usb_setup_t r = { USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_HID_REQ_SET_IDLE, 0, i, 0 };
    return ctrl_xfer(hc, s, &r, 0);
}

/*
 * parse_cfg - Parses the configuration descriptor to find the keyboard interface and endpoint
 */
static bool parse_cfg(xhci_hc_t *hc, xhci_slot_t *s, uint8_t *buf, uint16_t len) {
    uint16_t p = 0;
    uint8_t iface = 0xFF;
    bool is_kbd = false;
    LOGF("[XHCI] Parsing config descriptor (len=%u)\n", len);
    while (p < len) {
        if (buf[p] < 2 || p + buf[p] > len) break;
        if (buf[p + 1] == USB_DESC_CONFIG) {
            s->cfg_val = ((usb_config_desc_t *)(buf + p))->bConfigurationValue;
        } else if (buf[p + 1] == USB_DESC_INTERFACE) {
            usb_interface_desc_t *id = (void *)(buf + p);
            is_kbd = id->bInterfaceClass == USB_CLASS_HID && id->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD;
            iface = id->bInterfaceNumber;
            LOGF("[XHCI]   iface %u: cls=%u sub=%u proto=%u\n", iface, id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol);
        } else if (buf[p + 1] == USB_DESC_ENDPOINT) {
            usb_endpoint_desc_t *ed = (void *)(buf + p);
            if (is_kbd && (ed->bEndpointAddress & USB_EP_DIR_IN) && (ed->bmAttributes & 3) == USB_EP_TYPE_INTERRUPT) {
                LOGF("[XHCI]   Found interrupt IN endpoint 0x%02x\n", ed->bEndpointAddress);
                s->iface = iface;
                s->ep_addr = ed->bEndpointAddress;
                s->ep_mps = ed->wMaxPacketSize & 0x7FF;
                s->ep_ival = ed->bInterval;
                return true; 
            }
        }
        p += buf[p];
    }
    LOGF("[XHCI] No keyboard interface found in config descriptor\n");
    return false;
}

/*
 * get_mps - Returns the max packet size for the default control endpoint based on speed
 */
static uint16_t get_mps(uint8_t spd) {
    return (spd == SPD_SS || spd == SPD_SSP) ? 512 : (spd == SPD_LS ? 8 : 64);
}

/*
 * reset_port - Triggers a port reset and waits for it to complete
 */
static bool reset_port(xhci_hc_t *hc, uint8_t p) {
    uint32_t sc = or32(hc, XHCI_PORTSC(p));
    uint32_t safe_sc = sc & 0x0E00C200;
    ow32(hc, XHCI_PORTSC(p), safe_sc | PORT_RW1C);
    ow32(hc, XHCI_PORTSC(p), safe_sc | PORT_PR);
    for (int i = 0; i < 200; i++) {
        sleep_ms(1);
        sc = or32(hc, XHCI_PORTSC(p));
        if (sc & PORT_PRC) {
            safe_sc = sc & 0x0E00C200;
            ow32(hc, XHCI_PORTSC(p), safe_sc | PORT_PRC);
            return true;
        }
    }
    return false;
}

static void enum_hub(xhci_hc_t *hc, xhci_slot_t *hs);
static bool enum_dev(xhci_hc_t *hc, uint8_t p, uint8_t spd, uint32_t route_string, uint8_t root_hub_port, uint8_t tt_slot, uint8_t tt_port);

/*
 * enum_hub - Enumerates downstream ports of an identified USB hub
 */
static void enum_hub(xhci_hc_t *hc, xhci_slot_t *hs) {
    usb_setup_t req_hub = { USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, (uint16_t)((0x29 << 8) | 0), 0, 8 };
    if (hs->spd == SPD_SS || hs->spd == SPD_SSP) {
        req_hub.wValue = (0x2A << 8) | 0;
    }

    int n = ctrl_xfer(hc, hs, &req_hub, hc->scratch_phys);
    if (n < 2) {
        LOGF("[XHCI] slot %u: failed to get hub descriptor\n", hs->id);
        return;
    }
    uint8_t num_ports = hc->scratch[2];
    LOGF("[XHCI] Hub slot %u has %u ports\n", hs->id, num_ports);

    for (uint8_t i = 1; i <= num_ports; i++) {
        usb_setup_t pwr = { USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER, 3 /* SET_FEATURE */, 8 /* PORT_POWER */, i, 0 };
        ctrl_xfer(hc, hs, &pwr, 0);
    }
    sleep_ms(50);

    for (uint8_t i = 1; i <= num_ports; i++) {
        usb_setup_t rst = { USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER, 3 /* SET_FEATURE */, 4 /* PORT_RESET */, i, 0 };
        ctrl_xfer(hc, hs, &rst, 0);
        sleep_ms(50);

        usb_setup_t sts = { USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, 0 /* GET_STATUS */, 0, i, 4 };
        if (ctrl_xfer(hc, hs, &sts, hc->scratch_phys) == 4) {
            uint32_t status = *(uint32_t *)hc->scratch;
            if (status & 1) {
                uint8_t dspd = SPD_FS;
                if (status & (1 << 9)) dspd = SPD_LS;
                else if (status & (1 << 10)) dspd = SPD_HS;

                uint8_t tt_s = hs->tt_slot ? hs->tt_slot : (hs->spd == SPD_HS && dspd != SPD_HS ? hs->id : 0);
                uint8_t tt_p = hs->tt_slot ? hs->tt_port : (hs->spd == SPD_HS && dspd != SPD_HS ? i : 0);
                uint32_t route = hs->route_string;
                if (hs->spd == SPD_SS || hs->spd == SPD_SSP) {
                    for (int shift = 0; shift < 20; shift += 4) {
                        if ((route & (0xF << shift)) == 0) {
                            route |= (i & 0xF) << shift;
                            break;
                        }
                    }
                }

                enum_dev(hc, i, dspd, route, hs->root_hub_port, tt_s, tt_p);
            }
        }
    }
}

/*
 * enum_dev - Enumerates a generic USB device or hub, configuring its slot and endpoints
 */
static bool enum_dev(xhci_hc_t *hc, uint8_t p, uint8_t spd, uint32_t route_string, uint8_t root_hub_port, uint8_t tt_slot, uint8_t tt_port) {
    uint8_t slot_id = cmd_slot(hc);
    if (!slot_id) return false;

    xhci_slot_t *s = &hc->dev_slots[slot_id];
    kmemset(s, 0, sizeof(*s));
    s->id = slot_id;
    s->port = p;
    s->spd = spd;
    s->route_string = route_string;
    s->root_hub_port = root_hub_port;
    s->tt_slot = tt_slot;
    s->tt_port = tt_port;

    LOGF("[XHCI] slot %u: enum start (spd=%u route=%x rport=%u tt=%u:%u)\n", slot_id, spd, route_string, root_hub_port, tt_slot, tt_port);

    s->out_phys = 0;
    void *out = dma_alloc(hc, PAGE_SIZE, &s->out_phys);
    hc->dcbaa[s->id] = s->out_phys;
    ring_init(hc, &s->ep0, RING_SZ);

    uint64_t in_phys = 0;
    void *in = dma_alloc(hc, PAGE_SIZE, &in_phys);
    ctrl_ctx_t *c = get_ctrl(in);
    c->add = 3;
    slot_ctx_t *sc = get_slot(hc, in);
    sc->dw0 = SLOT_SPD(spd) | SLOT_CTX_ENT(1) | (route_string & 0xFFFFF);
    sc->dw1 = SLOT_PORT(root_hub_port);
    if (tt_slot) {
        sc->dw2 = (tt_slot) | (tt_port << 8);
    }
    
    ep_ctx_t *e0 = get_ep(hc, in, 0);
    uint16_t mps = get_mps(spd);
    e0->dw1 = EP_CERR(3) | EP_TYPE(EP_CTRL) | EP_PKT(mps);
    e0->deq_lo = (uint32_t)s->ep0.phys | EP_DCS;
    e0->deq_hi = (uint32_t)(s->ep0.phys >> 32);
    e0->dw4 = EP_AVG(8);

    bool is_ss = (spd == SPD_SS || spd == SPD_SSP);
    if (!cmd_addr(hc, in_phys, s->id, is_ss)) goto fail;

    sleep_ms(50);

    int r18 = get_desc(hc, s, USB_DESC_DEVICE, 0, 18);
    if (r18 < 18) goto fail;

    usb_device_desc_t *dd = (void *)hc->scratch;
    uint16_t vid = dd->idVendor;
    uint16_t pid = dd->idProduct;
    LOGF("[XHCI] slot %u: dev vid=0x%04x pid=0x%04x cls=%u\n", s->id, vid, pid, dd->bDeviceClass);

    uint16_t real_mps = (spd == SPD_SS || spd == SPD_SSP) ? (1 << dd->bMaxPacketSize0) : dd->bMaxPacketSize0;
    if (real_mps != mps) {
        kmemset(in, 0, PAGE_SIZE);
        c->add = 2;
        e0->dw1 = EP_CERR(3) | EP_TYPE(EP_CTRL) | EP_PKT(real_mps);
        e0->deq_lo = (uint32_t)s->ep0.phys | EP_DCS;
        e0->deq_hi = (uint32_t)(s->ep0.phys >> 32);
        e0->dw4 = EP_AVG(8);
        cmd_eval(hc, in_phys, s->id);
    }

    if (dd->bDeviceClass == 9) {
        
        int n = get_desc(hc, s, USB_DESC_CONFIG, 0, 9);
        if (n >= 9) {
            uint16_t total_len = ((usb_config_desc_t *)hc->scratch)->wTotalLength;
            if (total_len > 2048) total_len = 2048;
            n = get_desc(hc, s, USB_DESC_CONFIG, 0, total_len);
            if (n > 0) {
                uint8_t cfg_val = ((usb_config_desc_t *)hc->scratch)->bConfigurationValue;
                set_cfg(hc, s, cfg_val);
                enum_hub(hc, s);
            }
        }
        pmm_free(in_phys, PAGE_SIZE);
        return false;
    }

    int n = get_desc(hc, s, USB_DESC_CONFIG, 0, 9);
    if (n < 9) goto fail;
    
    uint16_t total_len = ((usb_config_desc_t *)hc->scratch)->wTotalLength;
    if (total_len > 2048) total_len = 2048;

    n = get_desc(hc, s, USB_DESC_CONFIG, 0, total_len);
    if (n <= 0 || !parse_cfg(hc, s, hc->scratch, n)) goto fail;
    if (set_cfg(hc, s, s->cfg_val) < 0) goto fail;

    ring_init(hc, &s->intr, RING_SZ);
    s->ep_idx = (s->ep_addr & 0xF) * 2 + 1;

    kmemset(in, 0, PAGE_SIZE);
    c->add = 1 | (1 << s->ep_idx);
    kmemcpy(get_slot(hc, in), (void *)PHYSMAP_P2V(s->out_phys), hc->ctx_sz);
    get_slot(hc, in)->dw0 = (get_slot(hc, in)->dw0 & ~(0x1F << 27)) | SLOT_CTX_ENT(s->ep_idx);
    
    ep_ctx_t *ei = get_ep(hc, in, s->ep_idx - 1);
    uint8_t iv = 0;
    if (spd == SPD_FS || spd == SPD_LS) {
        uint8_t b = s->ep_ival;
        if (b == 0) b = 1;
        uint8_t msb = 0;
        while (b > 1) { msb++; b >>= 1; }
        iv = msb + 3;
    } else {
        iv = s->ep_ival ? s->ep_ival - 1 : 0;
    }
    ei->dw0 = EP_IVAL(iv);
    ei->dw1 = EP_CERR(3) | EP_TYPE(7) | EP_PKT(s->ep_mps);
    ei->deq_lo = (uint32_t)s->intr.phys | EP_DCS;
    ei->deq_hi = (uint32_t)(s->intr.phys >> 32);
    ei->dw4 = EP_AVG(8) | EP_ESIT(s->ep_mps);

    if (!cmd_cfg(hc, in_phys, s->id)) goto fail;
    set_proto(hc, s, s->iface, USB_HID_PROTOCOL_BOOT);
    set_idle(hc, s, s->iface);

    s->hid_buf = dma_alloc(hc, PAGE_SIZE, &s->hid_phys);
    s->led_buf = dma_alloc(hc, PAGE_SIZE, &s->led_phys);
    pmm_free(in_phys, PAGE_SIZE);
    s->active = true;
    kprintf("[XHCI] USB keyboard found (VID=0x%04x PID=0x%04x) and configured!\n", vid, pid);
    return true;

fail:
    if (in_phys) pmm_free(in_phys, PAGE_SIZE);
    if (s->ep0.phys) pmm_free(s->ep0.phys, PAGE_SIZE);
    if (s->intr.phys) pmm_free(s->intr.phys, PAGE_SIZE);
    if (s->out_phys) { pmm_free(s->out_phys, PAGE_SIZE); hc->dcbaa[s->id] = 0; }
    if (s->hid_phys) pmm_free(s->hid_phys, PAGE_SIZE);
    if (s->led_phys) pmm_free(s->led_phys, PAGE_SIZE);
    return false;
}

static const keycode_t kmap[256] = {
    KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS, KEY_EQUAL, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_BACKSLASH, KEY_UNKNOWN, KEY_SEMICOLON, KEY_QUOTE, KEY_BACKTICK, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_UNKNOWN, KEY_SCROLLLOCK, KEY_UNKNOWN, KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END, KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP, KEY_NUMLOCK, KEY_KPSLASH, KEY_KPMULT, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KP0, KEY_KPDOT
};

static const uint8_t mmap[8] = { MOD_LCTRL, MOD_LSHIFT, MOD_LALT, MOD_LGUI, MOD_RCTRL, MOD_RSHIFT, MOD_RALT, MOD_RGUI };

static uint8_t usb_locks = 0;

/*
 * update_leds - Sends a SET_REPORT request to update the keyboard LED indicators
 */
static void update_leds(xhci_hc_t *hc, xhci_slot_t *s, uint8_t leds) {
    if (!s->led_buf) return;
    s->led_buf[0] = leds;
    usb_setup_t req = { USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_HID_REQ_SET_REPORT, (0x02 << 8) | 0, s->iface, 1 };
    uint32_t d0, d1;
    kmemcpy(&d0, &req, 4);
    kmemcpy(&d1, (uint8_t *)&req + 4, 4);
    uint32_t trt = 2 << 16;
    enq(&s->ep0, RING_SZ, d0, d1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
    enq(&s->ep0, RING_SZ, (uint32_t)s->led_phys, (uint32_t)(s->led_phys >> 32), 1, TRB_TYPE(TRB_DATA));
    enq(&s->ep0, RING_SZ, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | TRB_DIR_IN);
    dbw(hc, s->id, 1);
}

/*
 * handle_hid - Processes HID keyboard interrupt data, tracking key states and managing modifiers
 */
static void handle_hid(xhci_hc_t *hc, xhci_slot_t *s, const uint8_t *r) {
    uint8_t nmod = 0, c = r[0] ^ s->prev[0];
    for (int i = 0; i < 8; i++) if (r[0] & (1 << i)) nmod |= mmap[i];

    if (c) {
        for (int i = 0; i < 8; i++) {
            if (!(c & (1 << i))) continue;
            keycode_t k = (i == 0) ? KEY_LEFT_CTRL : (i == 1) ? KEY_LEFT_SHIFT : (i == 2) ? KEY_LEFT_ALT : (i == 3) ? KEY_LEFT_GUI : (i == 4) ? KEY_RIGHT_CTRL : (i == 5) ? KEY_RIGHT_SHIFT : (i == 6) ? KEY_RIGHT_ALT : (i == 7) ? KEY_RIGHT_GUI : KEY_UNKNOWN;
            if (k != KEY_UNKNOWN) input_handle_key((key_event_t){ k, (r[0] & (1 << i)) != 0, nmod, usb_locks });
        }
    }

    for (int i = 2; i < 8; i++) {
        if (s->prev[i]) {
            bool found = false;
            for (int j = 2; j < 8; j++) if (r[j] == s->prev[i]) found = true;
            if (!found && s->prev[i] < 116 && kmap[s->prev[i]] != KEY_UNKNOWN)
                input_handle_key((key_event_t){ kmap[s->prev[i]], false, nmod, usb_locks });
        }
        if (r[i]) {
            bool found = false;
            for (int j = 2; j < 8; j++) if (s->prev[j] == r[i]) found = true;
            if (!found && r[i] < 116 && kmap[r[i]] != KEY_UNKNOWN) {
                keycode_t k = kmap[r[i]];
                bool locks_changed = false;
                if (k == KEY_CAPSLOCK) { usb_locks ^= LOCK_CAPS; locks_changed = true; }
                else if (k == KEY_NUMLOCK) { usb_locks ^= LOCK_NUM; locks_changed = true; }
                else if (k == KEY_SCROLLLOCK) { usb_locks ^= LOCK_SCROLL; locks_changed = true; }
                
                input_handle_key((key_event_t){ k, true, nmod, usb_locks });
                
                if (locks_changed) {
                    uint8_t hid_leds = 0;
                    if (usb_locks & LOCK_NUM) hid_leds |= 1;
                    if (usb_locks & LOCK_CAPS) hid_leds |= 2;
                    if (usb_locks & LOCK_SCROLL) hid_leds |= 4;
                    if (s->cur_leds != hid_leds) {
                        s->cur_leds = hid_leds;
                        update_leds(hc, s, hid_leds);
                    }
                }
            }
        }
    }
    kmemcpy(s->prev, r, 8);
}

/*
 * arm_int - Re-arms the interrupt IN endpoint to receive further HID reports
 */
static void arm_int(xhci_hc_t *hc, xhci_slot_t *s) {
    enq(&s->intr, RING_SZ, (uint32_t)s->hid_phys, (uint32_t)(s->hid_phys >> 32), 8, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_INTR(0));
    dbw(hc, s->id, s->ep_idx);
}

/*
 * proc_evts - Processes pending events from the host controller's event ring
 */
static void proc_evts(xhci_hc_t *hc) {
    for (;;) {
        bool was = spinlock_acquire(&hc->lock);
        trb_t *t = deq(hc);
        if (!t) { spinlock_release(&hc->lock, was); break; }
        trb_t ev = *t;
        spinlock_release(&hc->lock, was);
        
        ow32(hc, XHCI_STS, STS_EINT);
        iw32(hc, IR_IMAN, ir32(hc, IR_IMAN) | IMAN_IP);
        
        if (GET_TYPE(ev.ctrl) == TRB_EV_XFER) {
            uint8_t cc = GET_CC(ev.dw2);
            uint8_t slot_id = GET_SLOT(ev.ctrl);
            if (slot_id < 256) {
                xhci_slot_t *s = &hc->dev_slots[slot_id];
                if (s->active && GET_EP(ev.ctrl) == s->ep_idx) {
                    if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) handle_hid(hc, s, s->hid_buf);
                    arm_int(hc, s);
                }
            }
        }
    }
}

/*
 * xhci_irq_handler - Handles the MSI/IRQ for the xHCI controller, triggering event processing
 */
cpu_context_t *xhci_irq_handler(cpu_context_t *ctx) {
    for (int i = 0; i < hc_cnt; i++) {
        proc_evts(hcs[i]);
    }
    return ctx;
}

/*
 * xhci_init - Discovers and initializes xHCI controllers, starting enumeration for connected devices
 */
bool xhci_init(void) {
    pci_dev_t pcis[16];
    int cnt = pci_get_xhci_controllers(pcis, 16);
    if (cnt == 0) return false;

    bool any_kbd = false;

    for (int i = 0; i < cnt; i++) {
        pci_dev_t *pci = &pcis[i];
        pci_enable(pci);

        xhci_hc_t *hc = (xhci_hc_t *)kmalloc(sizeof(xhci_hc_t));
        kmemset(hc, 0, sizeof(xhci_hc_t));
        hcs[hc_cnt++] = hc;

        uint32_t ms = align_up(pci->bar0_size, PAGE_SIZE);
        if (ms < PAGE_SIZE) ms = PAGE_SIZE;
        if (vmm_alloc(NULL, ms, VM_FLAG_WRITE | VM_FLAG_MMIO, (void *)pci->bar0_phys, (void **)&hc->cap) != VMM_OK) continue;

        hc->op = hc->cap + cr8(hc, XHCI_CAPLEN);
        hc->slots = HCS1_SLOTS(cr32(hc, XHCI_HCSPARAMS1));
        hc->ports = HCS1_PORTS(cr32(hc, XHCI_HCSPARAMS1));
        uint32_t hcc1 = cr32(hc, XHCI_HCCPARAMS1);
        hc->ctx_sz = HCC1_CSZ(hcc1) ? 64 : 32;
        hc->ac64 = (hcc1 & 1) != 0;
        hc->rt = hc->cap + (cr32(hc, XHCI_RTSOFF) & ~0x1F);
        hc->db = (uint32_t *)(hc->cap + (cr32(hc, XHCI_DBOFF) & ~3));
        hc->msi_vec = XHCI_MSI_VEC_BASE + i;

        bios_handoff(hc, pci);
        if (!reset_hc(hc)) continue;
        spinlock_init(&hc->lock, "xhci_evts");

        hc->dcbaa = dma_alloc(hc, align_up((hc->slots + 1) * 8, 64), &hc->dcbaa_phys);
        uint32_t scratches = HCS2_SCRATCH(cr32(hc, XHCI_HCSPARAMS2));
        if (scratches) {
            uint64_t spa;
            uint64_t *spa_buf = dma_alloc(hc, scratches * 8, &spa);
            for (uint32_t j = 0; j < scratches; j++) dma_alloc(hc, PAGE_SIZE, &spa_buf[j]);
            hc->dcbaa[0] = spa;
        }

        ow32(hc, XHCI_CONFIG, hc->slots & 0xFF);
        ow64(hc, XHCI_DCBAAP_LO, hc->dcbaa_phys);
        ring_init(hc, &hc->cmd, RING_SZ);
        ow64(hc, XHCI_CRCR_LO, (hc->cmd.phys & ~0x3F) | hc->cmd.cyc);
        evt_init(hc);

        start_hc(hc);
        hc->scratch = dma_alloc(hc, PAGE_SIZE, &hc->scratch_phys);

        for (uint8_t p = 0; p < hc->ports; p++) {
            uint32_t sc = or32(hc, XHCI_PORTSC(p));
            if (!(sc & PORT_CCS)) continue;

            if (!reset_port(hc, p)) continue;
            sleep_ms(50);

            sc = or32(hc, XHCI_PORTSC(p));
            if (!(sc & PORT_CCS)) continue;
            if (!(sc & (1 << 1))) continue;

            uint8_t spd = (sc >> PORT_SPD_SHIFT) & PORT_SPD_MASK;
            if (!spd) spd = SPD_FS;

            if (enum_dev(hc, p, spd, 0, p + 1, 0, 0)) {
                any_kbd = true;
            }
        }

        irq_register(hc->msi_vec, (irq_handler_t)xhci_irq_handler);
        pci_cfg_msi(pci, hc->msi_vec, lapic_get_id());

        for (int j = 0; j < 256; j++) {
            if (hc->dev_slots[j].active) arm_int(hc, &hc->dev_slots[j]);
        }
    }

    if (!any_kbd) {
        return false;
    }
    return true;
}