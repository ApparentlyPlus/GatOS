/*
 * xhci.h - eXtensible Host Controller Interface (xHCI) Driver
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <arch/x86_64/cpu/interrupts.h>
#include <kernel/sys/spinlock.h>
#include <kernel/sys/process.h>

#define XHCI_CAPLEN         0x00
#define XHCI_HCIVERSION     0x02
#define XHCI_HCSPARAMS1     0x04
#define XHCI_HCSPARAMS2     0x08
#define XHCI_HCCPARAMS1     0x10
#define XHCI_DBOFF          0x14
#define XHCI_RTSOFF         0x18

#define HCS1_SLOTS(x)       ((x) & 0xFF)
#define HCS1_PORTS(x)       (((x) >> 24) & 0xFF)
#define HCS2_SCRATCH_HI(x)  (((x) >> 21) & 0x1F)
#define HCS2_SCRATCH_LO(x)  (((x) >> 27) & 0x1F)
#define HCS2_SCRATCH(x)     ((HCS2_SCRATCH_HI(x) << 5) | HCS2_SCRATCH_LO(x))
#define HCC1_CSZ(x)         (((x) >> 2) & 1)
#define HCC1_XECP(x)        (((x) >> 16) & 0xFFFF)

#define XHCI_CMD            0x00
#define XHCI_STS            0x04
#define XHCI_CRCR_LO        0x18
#define XHCI_DCBAAP_LO      0x30
#define XHCI_CONFIG         0x38
#define XHCI_PORTSC(n)      (0x400 + (n) * 0x10)

#define CMD_RS              (1 << 0)
#define CMD_HCRST           (1 << 1)
#define CMD_INTE            (1 << 2)
#define CMD_HSEE            (1 << 3)

#define STS_HCH             (1 << 0)
#define STS_EINT            (1 << 3)
#define STS_CNR             (1 << 11)

#define PORT_CCS            (1 << 0)
#define PORT_PR             (1 << 4)
#define PORT_SPD_SHIFT      10
#define PORT_SPD_MASK       0xF
#define PORT_CSC            (1 << 17)
#define PORT_PRC            (1 << 21)
#define PORT_RW1C           (PORT_CSC | PORT_PRC | (1<<18) | (1<<19) | (1<<20) | (1<<22) | (1<<23))
#define PORT_PRESERVE       ((1<<9) | (3<<14) | (7<<25))  // PP, PIC, wake enables only

#define SPD_FS              1
#define SPD_LS              2
#define SPD_HS              3
#define SPD_SS              4
#define SPD_SSP             5

#define XHCI_IR0            0x20
#define IR_IMAN             0x00
#define IR_IMOD             0x04
#define IR_ERSTSZ           0x08
#define IR_ERSTBA_LO        0x10
#define IR_ERDP_LO          0x18

#define IMAN_IP             (1 << 0)
#define IMAN_IE             (1 << 1)
#define ERDP_EHB            (1 << 3)

#define EXCAP_LEGACY        0x01
#define LEG_BIOS_SEM        (1 << 16)
#define LEG_OS_SEM          (1 << 24)

typedef struct {
    uint32_t dw0, dw1, dw2, ctrl;
} __attribute__((packed)) trb_t;

#define TRB_C               (1 << 0)
#define TRB_TC              (1 << 1)
#define TRB_IOC             (1 << 5)
#define TRB_IDT             (1 << 6)
#define TRB_BSR             (1 << 9)
#define TRB_DIR_IN          (1 << 16)

#define TRB_TYPE(x)         (((x) & 0x3F) << 10)
#define GET_TYPE(c)         (((c) >> 10) & 0x3F)
#define TRB_SLOT(x)         (((x) & 0xFF) << 24)
#define GET_SLOT(c)         (((c) >> 24) & 0xFF)
#define GET_EP(c)           (((c) >> 16) & 0x1F)
#define TRB_INTR(x)         (((x) & 0x3FF) << 22)

#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_EN_SLOT         9
#define TRB_DIS_SLOT        10
#define TRB_ADDR_DEV        11
#define TRB_CFG_EP          12
#define TRB_EVAL_CTX        13
#define TRB_EV_XFER         32
#define TRB_EV_CMD          33
#define TRB_EV_PORT         34

#define CC_SUCCESS          1
#define CC_SHORT_PKT        13
#define GET_CC(d)           (((d) >> 24) & 0xFF)

typedef struct {
    uint64_t base;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed)) erst_t;

typedef struct {
    uint32_t dw0, dw1, dw2, dw3, rsvd[4];
} slot_ctx_t;

typedef struct {
    uint32_t dw0, dw1, deq_lo, deq_hi, dw4, rsvd[3];
} ep_ctx_t;

typedef struct {
    uint32_t drop, add, rsvd[6];
} ctrl_ctx_t;

#define SLOT_SPD(x)         (((x) & 0xF) << 20)
#define SLOT_CTX_ENT(x)     (((x) & 0x1F) << 27)
#define SLOT_PORT(x)        (((x) & 0xFF) << 16)

#define EP_IVAL(x)          (((x) & 0xFF) << 16)
#define EP_CERR(x)          (((x) & 0x3) << 1)
#define EP_TYPE(x)          (((x) & 0x7) << 3)
#define EP_PKT(x)           (((x) & 0xFFFF) << 16)
#define EP_DCS              1
#define EP_AVG(x)           ((x) & 0xFFFF)
#define EP_ESIT(x)          (((x) & 0xFFFF) << 16)

#define EP_CTRL             4
#define EP_INTR_IN          7

#define RING_SZ             32
#define ERING_SZ            64

typedef struct {
    trb_t *trbs;
    uint64_t phys;
    uint32_t enq, deq;
    uint8_t cyc;
} ring_t;

typedef struct {
    uint8_t id, spd, port, iface;
    uint8_t ep_addr, ep_ival, ep_idx;
    uint16_t ep_mps;
    uint8_t cfg_val;
    bool active;
    uint64_t out_phys;
    ring_t ep0, intr;
    uint64_t hid_phys;
    uint8_t *hid_buf;
    uint64_t led_phys;
    uint8_t *led_buf;
    uint8_t cur_leds;
    uint8_t prev[8];
    uint8_t route_string; // For nested hubs
    uint8_t root_hub_port;
    uint8_t tt_slot;
    uint8_t tt_port;

    // Hub support
    bool is_hub;
    uint8_t hub_num_ports;
} xhci_slot_t;

// IRQ driven completion slot
typedef struct {
    volatile int  done;
    trb_t         result;
    thread_t     *waiter;
} xhci_completion_t;

typedef struct {
    uint8_t *cap;
    uint8_t *op;
    uint8_t *rt;
    uint32_t *db;
    uint32_t slots;
    uint32_t ports;
    uint32_t ctx_sz;
    bool ac64;

    uint64_t *dcbaa;
    uint64_t dcbaa_phys;

    ring_t cmd;
    ring_t evt;
    erst_t *erst;
    uint64_t erst_phys;
    uint8_t *scratch;
    uint64_t scratch_phys;

    xhci_slot_t dev_slots[256];
    spinlock_t lock;
    uint8_t msi_vec;

    // Per HC completion slots
    xhci_completion_t cmd_comp;
    xhci_completion_t xfer_comp;

    // Hotplug worker thread
    thread_t *worker;
    volatile uint32_t pending_ports;     // bitmask, bit N = root port N needs attention
    volatile uint32_t pending_hub_slots; // bitmask, bit N = hub in slot N has port changes
} xhci_hc_t;

#define XHCI_MSI_VEC_BASE   50

bool xhci_init(void);
void xhci_hotplug_init(void);
cpu_context_t *xhci_irq_handler(cpu_context_t *ctx);
