/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          ACPI emulation.
 *
 *
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *          Tiseno100
 *
 *          Copyright 2020 Miran Grca.
 *          Copyright 2022 Tiseno100.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdbool.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/io.h>
#include <86box/pci.h>
#include <86box/pic.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/keyboard.h>
#include <86box/nvr.h>
#include <86box/pit.h>
#include <86box/apm.h>
#include <86box/tco.h>
#include <86box/acpi.h>
#include <86box/machine.h>
#include <86box/i2c.h>
#include <86box/video.h>

int acpi_rtc_status = 0;

static double cpu_to_acpi;

#ifdef ENABLE_ACPI_LOG
int acpi_do_log = ENABLE_ACPI_LOG;

static void
acpi_log(const char *fmt, ...)
{
    va_list ap;

    if (acpi_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define acpi_log(fmt, ...)
#endif

static uint64_t
acpi_clock_get()
{
    return tsc * cpu_to_acpi;
}

static uint32_t
acpi_timer_get(acpi_t *dev)
{
    uint64_t clock = acpi_clock_get();
    if (dev->regs.timer32)
        return clock & 0xffffffff;
    else
        return clock & 0xffffff;
}

static double
acpi_get_overflow_period(acpi_t *dev)
{
    uint64_t timer = acpi_clock_get();
    uint64_t overflow_time;

    if (dev->regs.timer32) {
        overflow_time = (timer + 0x80000000LL) & ~0x7fffffffLL;
    } else {
        overflow_time = (timer + 0x800000LL) & ~0x7fffffLL;
    }

    uint64_t time_to_overflow = overflow_time - timer;

    return ((double) time_to_overflow / (double) ACPI_TIMER_FREQ) * 1000000.0;
}

static void
acpi_timer_overflow(void *priv)
{
    acpi_t *dev    = (acpi_t *) priv;
    int     sci_en = dev->regs.pmcntrl & 1;

    dev->regs.pmsts |= TMROF_STS;

    if (dev->regs.pmen & 1) /* Timer Overflow Interrupt Enable */
    {
        acpi_log("ACPI: Overflow detected. Provoking an %s\n", sci_en ? "SCI" : "SMI");

        if (sci_en) /* Trigger an SCI or SMI depending on the status of the SCI_EN register */
            acpi_update_irq(dev);
        else
            acpi_raise_smi(dev, 1);
    }
}

static void
acpi_timer_update(acpi_t *dev, bool enable)
{
    if (enable) {
        timer_on_auto(&dev->timer, acpi_get_overflow_period(dev));
    } else {
        timer_stop(&dev->timer);
    }
}

void
acpi_update_irq(acpi_t *dev)
{
    int sci_level = (dev->regs.pmsts & dev->regs.pmen) & (RTC_EN | PWRBTN_EN | GBL_EN | TMROF_EN);
    if (dev->vendor == VEN_SMC)
        sci_level |= (dev->regs.pmsts & BM_STS);

    if (sci_level) {
        if (dev->irq_mode == 1)
            pci_set_irq(dev->slot, dev->irq_pin);
        else if (dev->irq_mode == 2)
            pci_set_mirq(5, dev->mirq_is_level);
        else
            pci_set_mirq(0xf0 | dev->irq_line, 1);
    } else {
        if (dev->irq_mode == 1)
            pci_clear_irq(dev->slot, dev->irq_pin);
        else if (dev->irq_mode == 2)
            pci_clear_mirq(5, dev->mirq_is_level);
        else
            pci_clear_mirq(0xf0 | dev->irq_line, 1);
    }

    acpi_timer_update(dev, (dev->regs.pmen & TMROF_EN) && !(dev->regs.pmsts & TMROF_STS));
}

void
acpi_raise_smi(void *priv, int do_smi)
{
    acpi_t *dev = (acpi_t *) priv;

    if (dev->regs.glbctl & 0x01) {
        if ((dev->vendor == VEN_VIA) || (dev->vendor == VEN_VIA_596B)) {
            if ((!dev->regs.smi_lock || !dev->regs.smi_active)) {
                if (do_smi)
                    smi_raise();
                dev->regs.smi_active = 1;
            }
        } else if ((dev->vendor == VEN_INTEL) || (dev->vendor == VEN_ALI)) {
            if (do_smi)
                smi_raise();
            /* Clear bit 16 of GLBCTL. */
            if (dev->vendor == VEN_INTEL)
                dev->regs.glbctl &= ~0x00010000;
            else
                dev->regs.ali_soft_smi = 1;
        } else if (dev->vendor == VEN_SMC) {
            if (do_smi)
                smi_raise();
        }
    } else if ((dev->vendor == VEN_INTEL_ICH2) && do_smi && (dev->regs.smi_en & 1))
        smi_raise();
}

static uint32_t
acpi_reg_read_common_regs(int size, uint16_t addr, void *p)
{
    acpi_t  *dev = (acpi_t *) p;
    uint32_t ret = 0x00000000;
    int      shift16, shift32;

    addr &= 0x3f;
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x00:
        case 0x01:
            /* PMSTS - Power Management Status Register (IO) */
            ret = (dev->regs.pmsts >> shift16) & 0xff;
            if (addr == 0x01)
                ret |= (acpi_rtc_status << 2);
            break;
        case 0x02:
        case 0x03:
            /* PMEN - Power Management Resume Enable Register (IO) */
            ret = (dev->regs.pmen >> shift16) & 0xff;
            break;
        case 0x04:
        case 0x05:
            /* PMCNTRL - Power Management Control Register (IO) */
            ret = (dev->regs.pmcntrl >> shift16) & 0xff;
            if (addr == 0x05)
                ret = (ret & 0xdf); /* Bit 5 is write-only. */
            break;
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
            /* PMTMR - Power Management Timer Register (IO) */
            ret = (acpi_timer_get(dev) >> shift32) & 0xff;
#ifdef USE_DYNAREC
            if (cpu_use_dynarec)
                update_tsc();
#endif
            break;
    }

#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
#endif
    return ret;
}

static uint32_t
acpi_reg_read_ali(int size, uint16_t addr, void *p)
{
    acpi_t  *dev = (acpi_t *) p;
    uint32_t ret = 0x00000000;
    int      shift16, shift32;

    addr &= 0x3f;
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            /* PCNTRL - Processor Control Register (IO) */
            ret = (dev->regs.pcntrl >> shift16) & 0xff;
            break;
        case 0x14:
            /* LVL2 - Processor Level 2 Register */
            ret = dev->regs.plvl2;
            break;
        case 0x15:
            /* LVL3 - Processor Level 3 Register */
            ret = dev->regs.plvl3;
            break;
        case 0x18:
        case 0x19:
            /* GPE0_STS - General Purpose Event0 Status Register */
            ret = (dev->regs.gpsts >> shift16) & 0xff;
            break;
        case 0x1a:
        case 0x1b:
            /* GPE0_EN - General Purpose Event0 Enable Register */
            ret = (dev->regs.gpen >> shift16) & 0xff;
            break;
        case 0x1d:
        case 0x1c:
            /* GPE1_STS - General Purpose Event1 Status Register */
            ret = (dev->regs.gpsts1 >> shift16) & 0xff;
            break;
        case 0x1f:
        case 0x1e:
            /* GPE1_EN - General Purpose Event1 Enable Register */
            ret = (dev->regs.gpen1 >> shift16) & 0xff;
            break;
        case 0x20 ... 0x27:
            /* GPE1_CTL - General Purpose Event1 Control Register */
            ret = (dev->regs.gpcntrl >> shift32) & 0xff;
            break;
        case 0x30:
            /* PM2_CNTRL - Power Management 2 Control Register( */
            ret = dev->regs.pmcntrl;
            break;
        default:
            ret = acpi_reg_read_common_regs(size, addr, p);
            break;
    }

#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
#endif
    return ret;
}

static uint32_t
acpi_reg_read_intel(int size, uint16_t addr, void *p)
{
    acpi_t  *dev = (acpi_t *) p;
    uint32_t ret = 0x00000000;
    int      shift16, shift32;

    addr &= 0x3f;
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x0c:
        case 0x0d:
            /* GPSTS - General Purpose Status Register (IO) */
            ret = (dev->regs.gpsts >> shift16) & 0xff;
            break;
        case 0x0e:
        case 0x0f:
            /* GPEN - General Purpose Enable Register (IO) */
            ret = (dev->regs.gpen >> shift16) & 0xff;
            break;
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            /* PCNTRL - Processor Control Register (IO) */
            ret = (dev->regs.pcntrl >> shift32) & 0xff;
            break;
        case 0x18:
        case 0x19:
            /* GLBSTS - Global Status Register (IO) */
            ret = (dev->regs.glbsts >> shift16) & 0xff;
            if (addr == 0x18) {
                ret &= 0x27;
                if (dev->regs.gpsts != 0x0000)
                    ret |= 0x80;
                if (dev->regs.pmsts != 0x0000)
                    ret |= 0x40;
                if (dev->regs.devsts != 0x00000000)
                    ret |= 0x10;
            }
            break;
        case 0x1c:
        case 0x1d:
        case 0x1e:
        case 0x1f:
            /* DEVSTS - Device Status Register (IO) */
            ret = (dev->regs.devsts >> shift32) & 0xff;
            break;
        case 0x20:
        case 0x21:
            /* GLBEN - Global Enable Register (IO) */
            ret = (dev->regs.glben >> shift16) & 0xff;
            break;
        case 0x28:
        case 0x29:
        case 0x2a:
        case 0x2b:
            /* GLBCTL - Global Control Register (IO) */
            ret = (dev->regs.glbctl >> shift32) & 0xff;
            break;
        case 0x2c:
        case 0x2d:
        case 0x2e:
        case 0x2f:
            /* DEVCTL - Device Control Register (IO) */
            ret = (dev->regs.devctl >> shift32) & 0xff;
            break;
        case 0x30:
        case 0x31:
        case 0x32:
            /* GPIREG - General Purpose Input Register (IO) */
            if (size == 1)
                ret = dev->regs.gpireg[addr & 3];
            break;
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            /* GPOREG - General Purpose Output Register (IO) */
            if (size == 1)
                ret = dev->regs.gporeg[addr & 3];
            break;
        default:
            ret = acpi_reg_read_common_regs(size, addr, p);
            break;
    }

#ifdef ENABLE_ACPI_LOG
        // if (size != 1)
        // acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
#endif
    return ret;
}

static uint32_t
acpi_reg_read_intel_ich2(int size, uint16_t addr, void *p)
{
    acpi_t  *dev = (acpi_t *) p;
    uint32_t ret = 0x00000000;
    int      shift16, shift32;

    addr &= 0x7f;
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            /* PROC_CNT - Processor Control Register */
            ret = (dev->regs.pcntrl >> shift32) & 0xff;
            break;

        case 0x28:
        case 0x29:
            /* GPE0_STS - General Purpose Event 0 Status Register */
            ret = (dev->regs.gpsts >> shift16) & 0xff;
            break;

        case 0x2a:
        case 0x2b:
            /* GPE0_EN - General Purpose Event 0 Enables Register */
            ret = (dev->regs.gpen >> shift16) & 0xff;
            break;

        case 0x2c:
        case 0x2d:
            /* GPE1_STS - General Purpose Event 1 Status Register */
            ret = (dev->regs.gpsts1 >> shift16) & 0xff;
            break;

        case 0x2e:
        case 0x2f:
            /* GPE1_EN - General Purpose Event 1 Enable Register */
            ret = (dev->regs.gpen1 >> shift16) & 0xff;
            break;

        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
            /* SMI_EN - SMI Control and Enable Register */
            ret = (dev->regs.smi_en >> shift32) & 0xff;
            break;

        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            /* SMI_STS - SMI Status Register */
            ret = (dev->regs.smi_sts >> shift32) & 0xff;
            break;

        case 0x40:
        case 0x41:
            /* MON_SMI - Device Monitor SMI Status and Enable Register */
            ret = (dev->regs.mon_smi >> shift16) & 0xff;
            break;

        case 0x44:
        case 0x45:
            /* DEVACT_STS - Device Activity Status Register */
            ret = (dev->regs.devact_sts >> shift16) & 0xff;
            break;

        case 0x48:
        case 0x49:
            /* DEVTRAP_EN - Device Trap Enable Register */
            ret = (dev->regs.devtrap_en >> shift16) & 0xff;
            break;

        case 0x4c ... 0x4d:
            /* BUS_ADDR_TRACK - Bus Address Tracker Register */
            ret = (dev->regs.bus_addr_track >> shift16) & 0xff;
            break;

        case 0x4e:
            /* BUS_CYC_TRACK - Bus Cycle Tracker Register */
            ret = dev->regs.bus_cyc_track;
            break;

        case 0x60 ... 0x70:
            /* TCO Registers */
            ret = tco_read(addr, dev->tco);
            break;

        default:
            ret = acpi_reg_read_common_regs(size, addr, p);
            break;
    }

#ifdef ENABLE_ACPI_LOG
        // if (size != 1)
        // acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
#endif
    return ret;
}

static uint32_t
acpi_reg_read_via_common(int size, uint16_t addr, void *p)
{
    acpi_t  *dev = (acpi_t *) p;
    uint32_t ret = 0x00000000;
    int      shift16, shift32;

    addr &= 0xff;
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            /* PCNTRL - Processor Control Register (IO) */
            ret = (dev->regs.pcntrl >> shift32) & 0xff;
            break;
        case 0x20:
        case 0x21:
            /* GPSTS - General Purpose Status Register (IO) */
            ret = (dev->regs.gpsts >> shift16) & 0xff;
            break;
        case 0x22:
        case 0x23:
            /* General Purpose SCI Enable */
            ret = (dev->regs.gpscien >> shift16) & 0xff;
            break;
        case 0x24:
        case 0x25:
            /* General Purpose SMI Enable */
            ret = (dev->regs.gpsmien >> shift16) & 0xff;
            break;
        case 0x26:
        case 0x27:
            /* Power Supply Control */
            ret = (dev->regs.pscntrl >> shift16) & 0xff;
            break;
        case 0x28:
        case 0x29:
            /* GLBSTS - Global Status Register (IO) */
            ret = (dev->regs.glbsts >> shift16) & 0xff;
            break;
        case 0x2a:
        case 0x2b:
            /* GLBEN - Global Enable Register (IO) */
            ret = (dev->regs.glben >> shift16) & 0xff;
            break;
        case 0x2c:
        case 0x2d:
            /* GLBCTL - Global Control Register (IO) */
            ret = (dev->regs.glbctl >> shift16) & 0xff;
            ret &= ~0x0110;
            ret |= (dev->regs.smi_lock ? 0x10 : 0x00);
            ret |= (dev->regs.smi_active ? 0x01 : 0x00);
            break;
        case 0x2f:
            /* SMI Command */
            if (size == 1)
                ret = dev->regs.smicmd & 0xff;
            break;
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
            /* Primary Activity Detect Status */
            ret = (dev->regs.padsts >> shift32) & 0xff;
            break;
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            /* Primary Activity Detect Enable */
            ret = (dev->regs.paden >> shift32) & 0xff;
            break;
        case 0x38:
        case 0x39:
        case 0x3a:
        case 0x3b:
            /* GP Timer Reload Enable */
            ret = (dev->regs.gptren >> shift32) & 0xff;
            break;
        default:
            ret = acpi_reg_read_common_regs(size, addr, p);
            break;
    }

#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
#endif
    return ret;
}

static uint32_t
acpi_reg_read_via(int size, uint16_t addr, void *p)
{
    acpi_t  *dev = (acpi_t *) p;
    uint32_t ret = 0x00000000;
    int      shift16;

    addr &= 0xff;
    shift16 = (addr & 1) << 3;

    switch (addr) {
        case 0x40:
            /* GPIO Direction Control */
            if (size == 1)
                ret = dev->regs.gpio_dir & 0xff;
            break;
        case 0x42:
            /* GPIO port Output Value */
            if (size == 1)
                ret = dev->regs.gpio_val & 0x13;
            break;
        case 0x44:
            /* GPIO port Input Value */
            if (size == 1) {
                ret = dev->regs.extsmi_val & 0xff;

                if (dev->i2c) {
                    ret &= 0xf9;
                    if (!(dev->regs.gpio_dir & 0x02) && i2c_gpio_get_scl(dev->i2c))
                        ret |= 0x02;
                    if (!(dev->regs.gpio_dir & 0x04) && i2c_gpio_get_sda(dev->i2c))
                        ret |= 0x04;
                }
            }
            break;
        case 0x46:
        case 0x47:
            /* GPO Port Output Value */
            ret = (dev->regs.gpo_val >> shift16) & 0xff;
            break;
        case 0x48:
        case 0x49:
            /* GPO Port Input Value */
            ret = (dev->regs.gpi_val >> shift16) & 0xff;
            break;
        default:
            ret = acpi_reg_read_via_common(size, addr, p);
            break;
    }

#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
#endif
    return ret;
}

static uint32_t
acpi_reg_read_via_596b(int size, uint16_t addr, void *p)
{
    acpi_t  *dev = (acpi_t *) p;
    uint32_t ret = 0x00000000;
    int      shift16, shift32;

    addr &= 0x7f;
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x40: /* Extended I/O Trap Status (686A/B) */
            ret = dev->regs.extiotrapsts;
            break;
        case 0x42: /* Extended I/O Trap Enable (686A/B) */
            ret = dev->regs.extiotrapen;
            break;
        case 0x44:
        case 0x45:
            /* External SMI Input Value */
            ret = (dev->regs.extsmi_val >> shift16) & 0xff;
            break;
        case 0x48:
        case 0x49:
        case 0x4a:
        case 0x4b:
            /* GPI Port Input Value */
            ret = (dev->regs.gpi_val >> shift32) & 0xff;
            break;
        case 0x4c:
        case 0x4d:
        case 0x4e:
        case 0x4f:
            /* GPO Port Output Value */
            ret = (dev->regs.gpo_val >> shift32) & 0xff;
            break;
        default:
            ret = acpi_reg_read_via_common(size, addr, p);
            break;
    }

#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
#endif
    return ret;
}

static uint32_t
acpi_reg_read_smc(int size, uint16_t addr, void *p)
{
    uint32_t ret = 0x00000000;

    addr &= 0x0f;

    ret = acpi_reg_read_common_regs(size, addr, p);

#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
#endif
    return ret;
}

static uint32_t
acpi_aux_reg_read_smc(int size, uint16_t addr, void *p)
{
    acpi_t  *dev = (acpi_t *) p;
    uint32_t ret = 0x00000000;
    int      shift16;

    addr &= 0x07;
    shift16 = (addr & 1) << 3;

    switch (addr) {
        case 0x00:
        case 0x01:
            /* SCI Status Register */
            ret = (dev->regs.pcntrl >> shift16) & 0xff;
            break;
        case 0x02:
        case 0x03:
            /* SCI Enable Register */
            ret = (dev->regs.gpscien >> shift16) & 0xff;
            break;
        case 0x04:
        case 0x05:
            /* Miscellaneous Status Register */
            ret = (dev->regs.glbsts >> shift16) & 0xff;
            break;
        case 0x06:
            /* Miscellaneous Enable Register */
            ret = dev->regs.glben & 0xff;
            break;
        case 0x07:
            /* Miscellaneous Control Register */
            ret = dev->regs.glbctl & 0xff;
            break;
    }

    acpi_log("(%i) ACPI Read  (%i) %02X: %02X\n", in_smm, size, addr, ret);
    return ret;
}

static void
acpi_reg_write_common_regs(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    int     shift16, sus_typ;

    addr &= 0x3f;
#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);
#endif
    shift16 = (addr & 1) << 3;

    switch (addr) {
        case 0x00:
        case 0x01:
            /* PMSTS - Power Management Status Register (IO) */
            dev->regs.pmsts &= ~((val << shift16) & 0x8d31);
            if ((addr == 0x01) && (val & 0x04))
                acpi_rtc_status = 0;
            acpi_update_irq(dev);
            break;
        case 0x02:
        case 0x03:
            /* PMEN - Power Management Resume Enable Register (IO) */
            dev->regs.pmen = ((dev->regs.pmen & ~(0xff << shift16)) | (val << shift16)) & 0x0521;
            acpi_update_irq(dev);
            break;
        case 0x04:
        case 0x05:
            /* PMCNTRL - Power Management Control Register (IO) */
            if ((addr == 0x05) && !!(val & 0x20) && !!(val & 4) && !!(dev->regs.smi_en & 0x00000010) && (dev->vendor == VEN_INTEL_ICH2)) {
                dev->regs.smi_sts |= 0x00000010; /* ICH2 Specific. Trigger an SMI if SLP_SMI_EN bit is set instead of transistioning to a Sleep State. */
                acpi_raise_smi(dev, 1);
            } else if ((addr == 0x05) && (val & 0x20)) {
                sus_typ = dev->suspend_types[(val >> 2) & 7];

                if (sus_typ & SUS_POWER_OFF) {
                    /* Soft power off. */
                    plat_power_off();
                    return;
                }

                if (sus_typ & SUS_SUSPEND) {
                    if (sus_typ & SUS_NVR) {
                        /* Suspend to RAM. */
                        nvr_reg_write(0x000f, 0xff, dev->nvr);
                    }

                    if (sus_typ & SUS_RESET_PCI)
                        device_reset_all_pci();

                    if (sus_typ & SUS_RESET_CPU)
                        cpu_alt_reset = 0;

                    if (sus_typ & SUS_RESET_PCI) {
                        pci_reset();
                        keyboard_at_reset();

                        mem_a20_alt = 0;
                        mem_a20_recalc();
                    }

                    if (sus_typ & (SUS_RESET_CPU | SUS_RESET_CACHE))
                        flushmmucache();

                    if (sus_typ & SUS_RESET_CPU)
                        resetx86();

                    /* Since the UI doesn't have a power button at the moment, pause emulation,
                       then trigger a resume event so that the system resumes after unpausing. */
                    plat_pause(1);
                    timer_set_delay_u64(&dev->resume_timer, 50 * TIMER_USEC);
                }
            }
            dev->regs.pmcntrl = ((dev->regs.pmcntrl & ~(0xff << shift16)) | (val << shift16)) & 0x3f07 /* 0x3c07 */;
            break;
    }
}

static void
acpi_reg_write_ali(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    int     shift16, shift32;

    addr &= 0x3f;
#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);
#endif
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            /* PCNTRL - Processor Control Register (IO) */
            dev->regs.pcntrl = ((dev->regs.pcntrl & ~(0xff << shift32)) | (val << shift32)) & 0x00023e1e;
            break;
        case 0x14:
            /* LVL2 - Processor Level 2 Register */
            dev->regs.plvl2 = val;
            break;
        case 0x15:
            /* LVL3 - Processor Level 3 Register */
            dev->regs.plvl3 = val;
            break;
        case 0x18:
        case 0x19:
            /* GPE0_STS - General Purpose Event0 Status Register */
            dev->regs.gpsts &= ~((val << shift16) & 0x0d07);
            break;
        case 0x1a:
        case 0x1b:
            /* GPE0_EN - General Purpose Event0 Enable Register */
            dev->regs.gpen = ((dev->regs.gpen & ~(0xff << shift16)) | (val << shift16)) & 0x0d07;
            break;
        case 0x1d:
        case 0x1c:
            /* GPE1_STS - General Purpose Event1 Status Register */
            dev->regs.gpsts1 &= ~((val << shift16) & 0x0c01);
            break;
        case 0x1f:
        case 0x1e:
            /* GPE1_EN - General Purpose Event1 Enable Register */
            dev->regs.gpen1 = ((dev->regs.gpen & ~(0xff << shift16)) | (val << shift16)) & 0x0c01;
            break;
        case 0x20 ... 0x27:
            /* GPE1_CTL - General Purpose Event1 Control Register */
            dev->regs.gpcntrl = ((dev->regs.gpcntrl & ~(0xff << shift32)) | (val << shift32)) & 0x00000001;
            break;
        case 0x30:
            /* PM2_CNTRL - Power Management 2 Control Register( */
            dev->regs.pmcntrl = val & 1;
            break;
        default:
            acpi_reg_write_common_regs(size, addr, val, p);
            /* Setting GBL_RLS also sets BIOS_STS and generates SMI. */
            if ((addr == 0x00) && !(dev->regs.pmsts & 0x20))
                dev->regs.gpcntrl &= ~0x0002;
            else if ((addr == 0x04) && (dev->regs.pmcntrl & 0x0004)) {
                dev->regs.gpsts1 |= 0x01;
                if (dev->regs.gpen1 & 0x01)
                    acpi_raise_smi(dev, 1);
            }
    }
}

static void
acpi_reg_write_intel(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    int     shift16, shift32;

    addr &= 0x3f;
#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);
#endif
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x0c:
        case 0x0d:
            /* GPSTS - General Purpose Status Register (IO) */
            dev->regs.gpsts &= ~((val << shift16) & 0x0f81);
            break;
        case 0x0e:
        case 0x0f:
            /* GPEN - General Purpose Enable Register (IO) */
            dev->regs.gpen = ((dev->regs.gpen & ~(0xff << shift16)) | (val << shift16)) & 0x0f01;
            break;
        case 0x10:
        case 0x11:
        case 0x13:
            /* PCNTRL - Processor Control Register (IO) */
            dev->regs.pcntrl = ((dev->regs.pcntrl & ~(0xff << shift32)) | (val << shift32)) & 0x00023e1e;
            break;
        case 0x12:
            /* PCNTRL - Processor Control Register (IO) */
            dev->regs.pcntrl = ((dev->regs.pcntrl & ~(0xfd << shift32)) | (val << shift32)) & 0x00023e1e;
            break;
        case 0x18:
        case 0x19:
            /* GLBSTS - Global Status Register (IO) */
            dev->regs.glbsts &= ~((val << shift16) & 0x0d27);
            break;
        case 0x1c:
        case 0x1d:
        case 0x1e:
        case 0x1f:
            /* DEVSTS - Device Status Register (IO) */
            dev->regs.devsts &= ~((val << shift32) & 0x3fff0fff);
            break;
        case 0x20:
        case 0x21:
            /* GLBEN - Global Enable Register (IO) */
            dev->regs.glben = ((dev->regs.glben & ~(0xff << shift16)) | (val << shift16)) & 0x8d1f;
            break;
        case 0x28:
        case 0x29:
        case 0x2a:
        case 0x2b:
            /* GLBCTL - Global Control Register (IO) */
            dev->regs.glbctl = ((dev->regs.glbctl & ~(0xff << shift32)) | (val << shift32)) & 0x0701ff07;
            /* Setting BIOS_RLS also sets GBL_STS and generates SMI. */
            if (dev->regs.glbctl & 0x00000002) {
                dev->regs.pmsts |= 0x20;
                if (dev->regs.pmen & 0x20)
                    acpi_update_irq(dev);
            }
            break;
        case 0x2c:
        case 0x2d:
        case 0x2e:
        case 0x2f:
            /* DEVCTL - Device Control Register (IO) */
            dev->regs.devctl = ((dev->regs.devctl & ~(0xff << shift32)) | (val << shift32)) & 0x0fffffff;
            if (dev->trap_update)
                dev->trap_update(dev->trap_priv);
            break;
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            /* GPOREG - General Purpose Output Register (IO) */
            if (size == 1)
                dev->regs.gporeg[addr & 3] = val;
            break;
        default:
            acpi_reg_write_common_regs(size, addr, val, p);
            /* Setting GBL_RLS also sets BIOS_STS and generates SMI. */
            if ((addr == 0x00) && !(dev->regs.pmsts & 0x20))
                dev->regs.glbctl &= ~0x0002;
            else if ((addr == 0x04) && (dev->regs.pmcntrl & 0x0004)) {
                dev->regs.glbsts |= 0x01;
                if (dev->regs.glben & 0x02)
                    acpi_raise_smi(dev, 1);
            }
            break;
    }
}

static void
acpi_reg_write_intel_ich2(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    int     shift16, shift32;

    addr &= 0x7f;
#ifdef ENABLE_ACPI_LOG
    if (size != 1)
        acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);
#endif
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            /* PROC_CNT - Processor Control Register */
            dev->regs.pcntrl = ((dev->regs.pcntrl & ~(0xff << shift32)) | (val << shift32)) & 0x000201fe;
            break;

        case 0x28:
        case 0x29:
            /* GPE0_STS - General Purpose Event 0 Status Register */
            dev->regs.gpsts &= ~((val << shift16) & 0x09fb);
            break;

        case 0x2a:
        case 0x2b:
            /* GPE0_EN - General Purpose Event 0 Enables Register */
            dev->regs.gpen = ((dev->regs.gpen & ~(0xff << shift16)) | (val << shift16)) & 0x097d;
            break;

        case 0x2c:
        case 0x2d:
            /* GPE1_STS - General Purpose Event 1 Status Register */
            dev->regs.gpsts1 &= ~((val << shift16) & 0x09fb);
            break;

        case 0x2e:
        case 0x2f:
            /* GPE1_EN - General Purpose Event 1 Enable Register */
            dev->regs.gpen1 = ((dev->regs.gpen & ~(0xff << shift16)) | (val << shift16)) & 0x097d;
            break;

        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
            /* SMI_EN - SMI Control and Enable Register */
            dev->regs.smi_en = ((dev->regs.smi_en & ~(0xff << shift32)) | (val << shift32)) & 0x0000867f;

            if (addr == 0x30) {
                apm_set_do_smi(dev->apm, !!(val & 0x20));

                if (val & 0x80) {
                    dev->regs.glbsts |= 0x0020;
                    acpi_update_irq(dev);
                }
            }
            break;

        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            /* SMI_STS - SMI Status Register */
            dev->regs.smi_sts &= ~((val << shift32) & 0x0001ff7c);
            break;

        case 0x40:
        case 0x41:
            /* MON_SMI - Device Monitor SMI Status and Enable Register */
            dev->regs.mon_smi = ((dev->regs.mon_smi & ~(0xff << shift16)) | (val << shift16)) & 0x097d;
            break;

        case 0x44:
        case 0x45:
            /* DEVACT_STS - Device Activity Status Register */
            dev->regs.devact_sts &= ~((val << shift16) & 0x3fef);
            break;

        case 0x48:
        case 0x49:
            /* DEVTRAP_EN - Device Trap Enable Register */
            dev->regs.devtrap_en = ((dev->regs.devtrap_en & ~(0xff << shift16)) | (val << shift16)) & 0x3c2f;
            if (dev->trap_update)
                dev->trap_update(dev->trap_priv);
            break;

        case 0x4c ... 0x4d:
            /* BUS_ADDR_TRACK - Bus Address Tracker Register */
            dev->regs.bus_addr_track = ((dev->regs.bus_addr_track & ~(0xff << shift16)) | (val << shift16)) & 0x097d;
            break;

        case 0x4e:
            /* BUS_CYC_TRACK - Bus Cycle Tracker Register */
            dev->regs.bus_cyc_track = val;
            break;

        case 0x60 ... 0x70:
            /* TCO Registers */
            tco_write(addr, val, dev->tco);
            break;

        default:
            acpi_reg_write_common_regs(size, addr, val, p);
            if ((addr == 0x04) && !!(val & 4) && !!(dev->regs.smi_en & 4)) {
                dev->regs.smi_sts = 0x00000004;
                acpi_raise_smi(dev, 1);
            }

            if ((addr == 0x02) || !!(val & 0x20) || !!(dev->regs.glbsts & 0x0020))
                acpi_update_irq(dev);
            break;
    }
}

static void
acpi_reg_write_via_common(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    int     shift16, shift32;

    addr &= 0xff;
    acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            /* PCNTRL - Processor Control Register (IO) */
            dev->regs.pcntrl = ((dev->regs.pcntrl & ~(0xff << shift32)) | (val << shift32)) & 0x0000001e;
            break;
        case 0x20:
        case 0x21:
            /* GPSTS - General Purpose Status Register (IO) */
            dev->regs.gpsts &= ~((val << shift16) & 0x03ff);
            break;
        case 0x22:
        case 0x23:
            /* General Purpose SCI Enable */
            dev->regs.gpscien = ((dev->regs.gpscien & ~(0xff << shift16)) | (val << shift16)) & 0x03ff;
            break;
        case 0x24:
        case 0x25:
            /* General Purpose SMI Enable */
            dev->regs.gpsmien = ((dev->regs.gpsmien & ~(0xff << shift16)) | (val << shift16)) & 0x03ff;
            break;
        case 0x26:
        case 0x27:
            /* Power Supply Control */
            dev->regs.pscntrl = ((dev->regs.pscntrl & ~(0xff << shift16)) | (val << shift16)) & 0x0701;
            break;
        case 0x2c:
            /* GLBCTL - Global Control Register (IO) */
            dev->regs.glbctl   = (dev->regs.glbctl & ~0xff) | (val & 0xff);
            dev->regs.smi_lock = !!(dev->regs.glbctl & 0x0010);
            /* Setting BIOS_RLS also sets GBL_STS and generates SMI. */
            if (dev->regs.glbctl & 0x0002) {
                dev->regs.pmsts |= 0x20;
                if (dev->regs.pmen & 0x20)
                    acpi_update_irq(dev);
            }
            break;
        case 0x2d:
            /* GLBCTL - Global Control Register (IO) */
            dev->regs.glbctl &= ~((val << 8) & 0x0100);
            if (val & 0x01)
                dev->regs.smi_active = 0;
            break;
        case 0x2f:
            /* SMI Command */
            if (size == 1) {
                dev->regs.smicmd = val & 0xff;
                dev->regs.glbsts |= 0x40;
                if (dev->regs.glben & 0x40)
                    acpi_raise_smi(dev, 1);
            }
            break;
        case 0x38:
        case 0x39:
        case 0x3a:
        case 0x3b:
            /* GP Timer Reload Enable */
            dev->regs.gptren = ((dev->regs.gptren & ~(0xff << shift32)) | (val << shift32)) & 0x000000d9;
            break;
        default:
            acpi_reg_write_common_regs(size, addr, val, p);
            /* Setting GBL_RLS also sets BIOS_STS and generates SMI. */
            if ((addr == 0x00) && !(dev->regs.pmsts & 0x20))
                dev->regs.glbctl &= ~0x0002;
            else if ((addr == 0x04) && (dev->regs.pmcntrl & 0x0004)) {
                dev->regs.glbsts |= 0x20;
                if (dev->regs.glben & 0x20)
                    acpi_raise_smi(dev, 1);
            }
            break;
    }
}

static void
acpi_i2c_set(acpi_t *dev)
{
    if (dev->i2c)
        i2c_gpio_set(dev->i2c, !(dev->regs.gpio_dir & 0x02) || (dev->regs.gpio_val & 0x02), !(dev->regs.gpio_dir & 0x04) || (dev->regs.gpio_val & 0x04));
}

static void
acpi_reg_write_via(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    int     shift16, shift32;

    addr &= 0xff;
    acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x28:
        case 0x29:
            /* GLBSTS - Global Status Register (IO) */
            dev->regs.glbsts &= ~((val << shift16) & 0x007f);
            break;
        case 0x2a:
        case 0x2b:
            /* GLBEN - Global Enable Register (IO) */
            dev->regs.glben = ((dev->regs.glben & ~(0xff << shift16)) | (val << shift16)) & 0x007f;
            break;
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
            /* Primary Activity Detect Status */
            dev->regs.padsts &= ~((val << shift32) & 0x000000fd);
            break;
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            /* Primary Activity Detect Enable */
            dev->regs.paden = ((dev->regs.paden & ~(0xff << shift32)) | (val << shift32)) & 0x000000fd;
            if (dev->trap_update)
                dev->trap_update(dev->trap_priv);
            break;
        case 0x40:
            /* GPIO Direction Control */
            if (size == 1) {
                dev->regs.gpio_dir = val & 0x7f;
                acpi_i2c_set(dev);
            }
            break;
        case 0x42:
            /* GPIO port Output Value */
            if (size == 1) {
                dev->regs.gpio_val = val & 0x13;
                acpi_i2c_set(dev);
            }
            break;
        case 0x46:
        case 0x47:
            /* GPO Port Output Value */
            dev->regs.gpo_val = ((dev->regs.gpo_val & ~(0xff << shift16)) | (val << shift16)) & 0xffff;
            break;
        default:
            acpi_reg_write_via_common(size, addr, val, p);
            break;
    }
}

static void
acpi_reg_write_via_596b(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    int     shift16, shift32;

    addr &= 0x7f;
    acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);
    shift16 = (addr & 1) << 3;
    shift32 = (addr & 3) << 3;

    switch (addr) {
        case 0x28:
        case 0x29:
            /* GLBSTS - Global Status Register (IO) */
            dev->regs.glbsts &= ~((val << shift16) & 0xfdff);
            break;
        case 0x2a:
        case 0x2b:
            /* GLBEN - Global Enable Register (IO) */
            dev->regs.glben = ((dev->regs.glben & ~(0xff << shift16)) | (val << shift16)) & 0xfdff;
            break;
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
            /* Primary Activity Detect Status */
            dev->regs.padsts &= ~((val << shift32) & 0x000007ff);
            break;
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
            /* Primary Activity Detect Enable */
            dev->regs.paden = ((dev->regs.paden & ~(0xff << shift32)) | (val << shift32)) & 0x000007ff;
            if (dev->trap_update)
                dev->trap_update(dev->trap_priv);
            break;
        case 0x40: /* Extended I/O Trap Status (686A/B) */
            dev->regs.extiotrapsts &= ~(val & 0x13);
            break;
        case 0x42: /* Extended I/O Trap Enable (686A/B) */
            dev->regs.extiotrapen = val & 0x13;
            break;
        case 0x4c:
        case 0x4d:
        case 0x4e:
        case 0x4f:
            /* GPO Port Output Value */
            dev->regs.gpo_val = ((dev->regs.gpo_val & ~(0xff << shift32)) | (val << shift32)) & 0x7fffffff;
            break;
        default:
            acpi_reg_write_via_common(size, addr, val, p);
            break;
    }
}

static void
acpi_reg_write_smc(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;

    addr &= 0x0f;
    acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);

    acpi_reg_write_common_regs(size, addr, val, p);
    /* Setting GBL_RLS also sets BIOS_STS and generates SMI. */
    if ((addr == 0x00) && !(dev->regs.pmsts & 0x20))
        dev->regs.glbctl &= ~0x0001;
    else if ((addr == 0x04) && (dev->regs.pmcntrl & 0x0004)) {
        dev->regs.glbsts |= 0x01;
        if (dev->regs.glben & 0x01)
            acpi_raise_smi(dev, 1);
    }
}

static void
acpi_aux_reg_write_smc(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    int     shift16;

    addr &= 0x07;
    acpi_log("(%i) ACPI Write (%i) %02X: %02X\n", in_smm, size, addr, val);
    shift16 = (addr & 1) << 3;

    switch (addr) {
        case 0x00:
        case 0x01:
            /* SCI Status Register */
            dev->regs.gpscists &= ~((val << shift16) & 0x000c);
            break;
        case 0x02:
        case 0x03:
            /* SCI Enable Register */
            dev->regs.gpscien = ((dev->regs.gpscien & ~(0xff << shift16)) | (val << shift16)) & 0x3fff;
            break;
        case 0x04:
        case 0x05:
            /* Miscellanous Status Register */
            dev->regs.glbsts &= ~((val << shift16) & 0x001f);
            break;
        case 0x06:
            /* Miscellaneous Enable Register */
            dev->regs.glben = (uint16_t) (val & 0x03);
            break;
        case 0x07:
            /* Miscellaneous Control Register */
            dev->regs.glbctl = (uint16_t) (val & 0x03);
            /* Setting BIOS_RLS also sets GBL_STS and generates SMI. */
            if (dev->regs.glbctl & 0x0001) {
                dev->regs.pmsts |= 0x20;
                if (dev->regs.pmen & 0x20)
                    acpi_update_irq(dev);
            }
            if (dev->regs.glbctl & 0x0002) {
                dev->regs.pmsts |= 0x10;
                if (dev->regs.pmcntrl & 0x02)
                    acpi_update_irq(dev);
            }
            break;
    }
}

static uint32_t
acpi_reg_read_common(int size, uint16_t addr, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    uint8_t ret = 0xff;

    if (dev->vendor == VEN_ALI)
        ret = acpi_reg_read_ali(size, addr, p);
    else if (dev->vendor == VEN_VIA)
        ret = acpi_reg_read_via(size, addr, p);
    else if (dev->vendor == VEN_VIA_596B)
        ret = acpi_reg_read_via_596b(size, addr, p);
    else if (dev->vendor == VEN_INTEL)
        ret = acpi_reg_read_intel(size, addr, p);
    else if (dev->vendor == VEN_INTEL_ICH2)
        ret = acpi_reg_read_intel_ich2(size, addr, p);
    else if (dev->vendor == VEN_SMC)
        ret = acpi_reg_read_smc(size, addr, p);

    return ret;
}

static void
acpi_reg_write_common(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;

    if (dev->vendor == VEN_ALI)
        acpi_reg_write_ali(size, addr, val, p);
    else if (dev->vendor == VEN_VIA)
        acpi_reg_write_via(size, addr, val, p);
    else if (dev->vendor == VEN_VIA_596B)
        acpi_reg_write_via_596b(size, addr, val, p);
    else if (dev->vendor == VEN_INTEL)
        acpi_reg_write_intel(size, addr, val, p);
    else if (dev->vendor == VEN_INTEL_ICH2)
        acpi_reg_write_intel_ich2(size, addr, val, p);
    else if (dev->vendor == VEN_SMC)
        acpi_reg_write_smc(size, addr, val, p);
}

static uint32_t
acpi_aux_reg_read_common(int size, uint16_t addr, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    uint8_t ret = 0xff;

    if (dev->vendor == VEN_SMC)
        ret = acpi_aux_reg_read_smc(size, addr, p);

    return ret;
}

static void
acpi_aux_reg_write_common(int size, uint16_t addr, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;

    if (dev->vendor == VEN_SMC)
        acpi_aux_reg_write_smc(size, addr, val, p);
}

static uint32_t
acpi_reg_readl(uint16_t addr, void *p)
{
    uint32_t ret = 0x00000000;

    ret = acpi_reg_read_common(4, addr, p);
    ret |= (acpi_reg_read_common(4, addr + 1, p) << 8);
    ret |= (acpi_reg_read_common(4, addr + 2, p) << 16);
    ret |= (acpi_reg_read_common(4, addr + 3, p) << 24);

    acpi_log("ACPI: Read L %08X from %04X\n", ret, addr);

    return ret;
}

static uint16_t
acpi_reg_readw(uint16_t addr, void *p)
{
    uint16_t ret = 0x0000;

    ret = acpi_reg_read_common(2, addr, p);
    ret |= (acpi_reg_read_common(2, addr + 1, p) << 8);

    acpi_log("ACPI: Read W %08X from %04X\n", ret, addr);

    return ret;
}

static uint8_t
acpi_reg_read(uint16_t addr, void *p)
{
    uint8_t ret = 0x00;

    ret = acpi_reg_read_common(1, addr, p);

    acpi_log("ACPI: Read B %02X from %04X\n", ret, addr);

    return ret;
}

static uint32_t
acpi_aux_reg_readl(uint16_t addr, void *p)
{
    uint32_t ret = 0x00000000;

    ret = acpi_aux_reg_read_common(4, addr, p);
    ret |= (acpi_aux_reg_read_common(4, addr + 1, p) << 8);
    ret |= (acpi_aux_reg_read_common(4, addr + 2, p) << 16);
    ret |= (acpi_aux_reg_read_common(4, addr + 3, p) << 24);

    acpi_log("ACPI: Read Aux L %08X from %04X\n", ret, addr);

    return ret;
}

static uint16_t
acpi_aux_reg_readw(uint16_t addr, void *p)
{
    uint16_t ret = 0x0000;

    ret = acpi_aux_reg_read_common(2, addr, p);
    ret |= (acpi_aux_reg_read_common(2, addr + 1, p) << 8);

    acpi_log("ACPI: Read Aux W %04X from %04X\n", ret, addr);

    return ret;
}

static uint8_t
acpi_aux_reg_read(uint16_t addr, void *p)
{
    uint8_t ret = 0x00;

    ret = acpi_aux_reg_read_common(1, addr, p);

    acpi_log("ACPI: Read Aux B %02X from %04X\n", ret, addr);

    return ret;
}

static void
acpi_reg_writel(uint16_t addr, uint32_t val, void *p)
{
    acpi_log("ACPI: Write L %08X to %04X\n", val, addr);

    acpi_reg_write_common(4, addr, val & 0xff, p);
    acpi_reg_write_common(4, addr + 1, (val >> 8) & 0xff, p);
    acpi_reg_write_common(4, addr + 2, (val >> 16) & 0xff, p);
    acpi_reg_write_common(4, addr + 3, (val >> 24) & 0xff, p);
}

static void
acpi_reg_writew(uint16_t addr, uint16_t val, void *p)
{
    acpi_log("ACPI: Write W %04X to %04X\n", val, addr);

    acpi_reg_write_common(2, addr, val & 0xff, p);
    acpi_reg_write_common(2, addr + 1, (val >> 8) & 0xff, p);
}

static void
acpi_reg_write(uint16_t addr, uint8_t val, void *p)
{
    acpi_log("ACPI: Write B %02X to %04X\n", val, addr);

    acpi_reg_write_common(1, addr, val, p);
}

static void
acpi_aux_reg_writel(uint16_t addr, uint32_t val, void *p)
{
    acpi_log("ACPI: Write Aux L %08X to %04X\n", val, addr);

    acpi_aux_reg_write_common(4, addr, val & 0xff, p);
    acpi_aux_reg_write_common(4, addr + 1, (val >> 8) & 0xff, p);
    acpi_aux_reg_write_common(4, addr + 2, (val >> 16) & 0xff, p);
    acpi_aux_reg_write_common(4, addr + 3, (val >> 24) & 0xff, p);
}

static void
acpi_aux_reg_writew(uint16_t addr, uint16_t val, void *p)
{
    acpi_log("ACPI: Write Aux W %04X to %04X\n", val, addr);

    acpi_aux_reg_write_common(2, addr, val & 0xff, p);
    acpi_aux_reg_write_common(2, addr + 1, (val >> 8) & 0xff, p);
}

static void
acpi_aux_reg_write(uint16_t addr, uint8_t val, void *p)
{
    acpi_log("ACPI: Write Aux B %02X to %04X\n", val, addr);

    acpi_aux_reg_write_common(1, addr, val, p);
}

void
acpi_update_io_mapping(acpi_t *dev, uint32_t base, int chipset_en)
{
    int size;

    switch (dev->vendor) {
        case VEN_ALI:
        case VEN_INTEL:
        default:
            size = 0x040;
            break;
        case VEN_SMC:
            size = 0x010;
            break;
        case VEN_VIA:
            size = 0x100;
            break;
        case VEN_INTEL_ICH2:
        case VEN_VIA_596B:
            size = 0x0080;
            break;
    }

    acpi_log("ACPI: Update I/O %04X to %04X (%sabled)\n", dev->io_base, base, chipset_en ? "en" : "dis");

    if (dev->io_base != 0x0000) {
        io_removehandler(dev->io_base, size,
                         acpi_reg_read, acpi_reg_readw, acpi_reg_readl,
                         acpi_reg_write, acpi_reg_writew, acpi_reg_writel, dev);
    }

    dev->io_base = base;

    if (chipset_en && (dev->io_base != 0x0000)) {
        io_sethandler(dev->io_base, size,
                      acpi_reg_read, acpi_reg_readw, acpi_reg_readl,
                      acpi_reg_write, acpi_reg_writew, acpi_reg_writel, dev);
    }
}

void
acpi_update_aux_io_mapping(acpi_t *dev, uint32_t base, int chipset_en)
{
    int size;

    switch (dev->vendor) {
        case VEN_SMC:
            size = 0x008;
            break;
        default:
            size = 0x000;
            break;
    }

    acpi_log("ACPI: Update Aux I/O %04X to %04X (%sabled)\n", dev->aux_io_base, base, chipset_en ? "en" : "dis");

    if (dev->aux_io_base != 0x0000) {
        io_removehandler(dev->aux_io_base, size,
                         acpi_aux_reg_read, acpi_aux_reg_readw, acpi_aux_reg_readl,
                         acpi_aux_reg_write, acpi_aux_reg_writew, acpi_aux_reg_writel, dev);
    }

    dev->aux_io_base = base;

    if (chipset_en && (dev->aux_io_base != 0x0000)) {
        io_sethandler(dev->aux_io_base, size,
                      acpi_aux_reg_read, acpi_aux_reg_readw, acpi_aux_reg_readl,
                      acpi_aux_reg_write, acpi_aux_reg_writew, acpi_aux_reg_writel, dev);
    }
}

static void
acpi_timer_resume(void *priv)
{
    acpi_t *dev = (acpi_t *) priv;

    dev->regs.pmsts |= 0x8000;

    /* Nasty workaround for ASUS P2B-LS and potentially others, where the PMCNTRL
       SMI trap handler clears the resume bit before returning control to the OS. */
    if (in_smm)
        timer_set_delay_u64(&dev->resume_timer, 50 * TIMER_USEC);
}

void
acpi_init_gporeg(acpi_t *dev, uint8_t val0, uint8_t val1, uint8_t val2, uint8_t val3)
{
    dev->regs.gporeg[0] = dev->gporeg_default[0] = val0;
    dev->regs.gporeg[1] = dev->gporeg_default[1] = val1;
    dev->regs.gporeg[2] = dev->gporeg_default[2] = val2;
    dev->regs.gporeg[3] = dev->gporeg_default[3] = val3;
    acpi_log("acpi_init_gporeg(): %02X %02X %02X %02X\n", dev->regs.gporeg[0], dev->regs.gporeg[1], dev->regs.gporeg[2], dev->regs.gporeg[3]);
}

void
acpi_set_timer32(acpi_t *dev, uint8_t timer32)
{
    dev->regs.timer32 = timer32;
}

void
acpi_set_slot(acpi_t *dev, int slot)
{
    dev->slot = slot;
}

void
acpi_set_irq_mode(acpi_t *dev, int irq_mode)
{
    dev->irq_mode = irq_mode;
}

void
acpi_set_irq_pin(acpi_t *dev, int irq_pin)
{
    dev->irq_pin = irq_pin;
}

void
acpi_set_irq_line(acpi_t *dev, int irq_line)
{
    dev->irq_line = irq_line;
}

void
acpi_set_mirq_is_level(acpi_t *dev, int mirq_is_level)
{
    dev->mirq_is_level = mirq_is_level;
}

void
acpi_set_gpireg2_default(acpi_t *dev, uint8_t gpireg2_default)
{
    dev->gpireg2_default = gpireg2_default;
    dev->regs.gpireg[2]  = dev->gpireg2_default;
}

void
acpi_set_nvr(acpi_t *dev, nvr_t *nvr)
{
    dev->nvr = nvr;
}

void
acpi_set_tco(acpi_t *dev, tco_t *tco)
{
    dev->tco = tco;
}

void
acpi_set_trap_update(acpi_t *dev, void (*update)(void *priv), void *priv)
{
    dev->trap_update = update;
    dev->trap_priv   = priv;
}

uint8_t
acpi_ali_soft_smi_status_read(acpi_t *dev)
{
    return dev->regs.ali_soft_smi = 1;
}

void
acpi_ali_soft_smi_status_write(acpi_t *dev, uint8_t soft_smi)
{
    dev->regs.ali_soft_smi = soft_smi;
}

static void
acpi_apm_out(uint16_t port, uint8_t val, void *p)
{
    acpi_t *dev = (acpi_t *) p;

    acpi_log("[%04X:%08X] APM write: %04X = %02X (AX = %04X, BX = %04X, CX = %04X)\n", CS, cpu_state.pc, port, val, AX, BX, CX);

    port &= 0x0001;

    if (dev->vendor == VEN_ALI) {
        if (port == 0x0001) {
            acpi_log("ALi SOFT SMI# status set (%i)\n", dev->apm->do_smi);
            dev->apm->cmd = val;
            // acpi_raise_smi(dev, dev->apm->do_smi);
            if (dev->apm->do_smi)
                smi_raise();
            dev->regs.ali_soft_smi = 1;
        } else if (port == 0x0003)
            dev->apm->stat = val;
    } else {
        if (port == 0x0000) {
            dev->apm->cmd = val;
            if (dev->vendor == VEN_INTEL)
                dev->regs.glbsts |= 0x20;
            else if (dev->vendor == VEN_INTEL_ICH2)
                if (dev->apm->do_smi)
                    dev->regs.smi_sts |= 0x00000020;
            acpi_raise_smi(dev, dev->apm->do_smi);
        } else
            dev->apm->stat = val;
    }
}

static uint8_t
acpi_apm_in(uint16_t port, void *p)
{
    acpi_t *dev = (acpi_t *) p;
    uint8_t ret = 0xff;

    port &= 0x0001;

    if (dev->vendor == VEN_ALI) {
        if (port == 0x0001)
            ret = dev->apm->cmd;
        else if (port == 0x0003)
            ret = dev->apm->stat;
    } else {
        if (port == 0x0000)
            ret = dev->apm->cmd;
        else
            ret = dev->apm->stat;
    }

    acpi_log("[%04X:%08X] APM read: %04X = %02X\n", CS, cpu_state.pc, port, ret);

    return ret;
}

static void
acpi_reset(void *priv)
{
    acpi_t *dev = (acpi_t *) priv;
    int     i;

    memset(&dev->regs, 0x00, sizeof(acpi_regs_t));
    dev->regs.gpireg[0] = 0xff;
    dev->regs.gpireg[1] = 0xff;
    /* A-Trend ATC7020BXII:
       - Bit 3: 80-conductor cable on secondary IDE channel (active low)
       - Bit 2: 80-conductor cable on primary IDE channel (active low)
       Gigabyte GA-686BX:
       - Bit 1: CMOS battery low (active high) */
    dev->regs.gpireg[2] = dev->gpireg2_default;
    for (i = 0; i < 4; i++)
        dev->regs.gporeg[i] = dev->gporeg_default[i];
    if (dev->vendor == VEN_VIA_596B) {
        dev->regs.gpo_val = 0x7fffffff;
        /* FIC VA-503A:
           - Bit 11: ATX power (active high)
           - Bit  4: 80-conductor cable on primary IDE channel (active low)
           - Bit  3: 80-conductor cable on secondary IDE channel (active low)
           - Bit  2: password cleared (active low)
           ASUS P3V4X:
           - Bit 15: 80-conductor cable on secondary IDE channel (active low)
           - Bit  5: 80-conductor cable on primary IDE channel (active low)
           BCM GT694VA:
           - Bit 19: 80-conductor cable on secondary IDE channel (active low)
           - Bit 17: 80-conductor cable on primary IDE channel (active low)
           ASUS CUV4X-LS:
           - Bit  2: 80-conductor cable on secondary IDE channel (active low)
           - Bit  1: 80-conductor cable on primary IDE channel (active low)
           Acorp 6VIA90AP:
           - Bit  3: 80-conductor cable on secondary IDE channel (active low)
           - Bit  1: 80-conductor cable on primary IDE channel (active low) */
        dev->regs.gpi_val = 0xfff57fc1;
        if (!strcmp(machine_get_internal_name(), "ficva503a") || !strcmp(machine_get_internal_name(), "6via90ap"))
            dev->regs.gpi_val |= 0x00000004;
    }

    /* Power on always generates a resume event. */
    dev->regs.pmsts |= 0x8000;

    acpi_rtc_status = 0;
}

static void
acpi_speed_changed(void *priv)
{
    acpi_t *dev        = (acpi_t *) priv;
    cpu_to_acpi        = ACPI_TIMER_FREQ / cpuclock;
    bool timer_enabled = timer_is_enabled(&dev->timer);
    timer_stop(&dev->timer);

    if (timer_enabled)
        timer_on_auto(&dev->timer, acpi_get_overflow_period(dev));
}

static void
acpi_close(void *priv)
{
    acpi_t *dev = (acpi_t *) priv;

    if (dev->i2c) {
        if (i2c_smbus == i2c_gpio_get_bus(dev->i2c))
            i2c_smbus = NULL;
        i2c_gpio_close(dev->i2c);
    }

    timer_stop(&dev->timer);

    free(dev);
}

static void *
acpi_init(const device_t *info)
{
    acpi_t *dev;

    dev = (acpi_t *) malloc(sizeof(acpi_t));
    if (dev == NULL)
        return (NULL);
    memset(dev, 0x00, sizeof(acpi_t));

    cpu_to_acpi = ACPI_TIMER_FREQ / cpuclock;
    dev->vendor = info->local;

    dev->irq_line = 9;

    if ((dev->vendor == VEN_INTEL) || (dev->vendor == VEN_ALI) || (dev->vendor == VEN_INTEL_ICH2)) {
        if (dev->vendor == VEN_ALI)
            dev->irq_mode = 2;
        dev->apm = device_add(&apm_pci_acpi_device);
        if (dev->vendor == VEN_ALI) {
            acpi_log("Setting I/O handler at port B1\n");
            io_sethandler(0x00b1, 0x0003, acpi_apm_in, NULL, NULL, acpi_apm_out, NULL, NULL, dev);
        } else
            io_sethandler(0x00b2, 0x0002, acpi_apm_in, NULL, NULL, acpi_apm_out, NULL, NULL, dev);
    } else if (dev->vendor == VEN_VIA) {
        dev->i2c  = i2c_gpio_init("smbus_vt82c586b");
        i2c_smbus = i2c_gpio_get_bus(dev->i2c);
    }

    switch (dev->vendor) {
        case VEN_ALI:
            dev->suspend_types[0] = SUS_POWER_OFF;
            dev->suspend_types[1] = SUS_POWER_OFF;
            dev->suspend_types[2] = SUS_SUSPEND | SUS_NVR | SUS_RESET_CPU | SUS_RESET_PCI;
            dev->suspend_types[3] = SUS_SUSPEND;
            break;

        case VEN_VIA:
            dev->suspend_types[0] = SUS_POWER_OFF;
            dev->suspend_types[2] = SUS_SUSPEND;
            break;

        case VEN_VIA_596B:
            dev->suspend_types[1] = SUS_SUSPEND | SUS_NVR | SUS_RESET_CPU | SUS_RESET_PCI;
            dev->suspend_types[2] = SUS_POWER_OFF;
            dev->suspend_types[4] = SUS_SUSPEND;
            dev->suspend_types[5] = SUS_SUSPEND | SUS_RESET_CPU;
            dev->suspend_types[6] = SUS_SUSPEND | SUS_RESET_CPU | SUS_RESET_PCI;
            break;

        case VEN_INTEL:
            dev->suspend_types[0] = SUS_POWER_OFF;
            dev->suspend_types[1] = SUS_SUSPEND | SUS_NVR | SUS_RESET_CPU | SUS_RESET_PCI;
            dev->suspend_types[2] = SUS_SUSPEND | SUS_RESET_CPU;
            dev->suspend_types[3] = SUS_SUSPEND | SUS_RESET_CACHE;
            dev->suspend_types[4] = SUS_SUSPEND;
            break;

        case VEN_INTEL_ICH2:
            dev->suspend_types[1] = SUS_SUSPEND | SUS_RESET_CPU;
            dev->suspend_types[5] = SUS_SUSPEND | SUS_NVR | SUS_RESET_CPU | SUS_RESET_PCI;
            dev->suspend_types[6] = SUS_POWER_OFF;
            dev->suspend_types[7] = SUS_POWER_OFF;
            break;
    }

    timer_add(&dev->timer, acpi_timer_overflow, dev, 0);
    timer_add(&dev->resume_timer, acpi_timer_resume, dev, 0);

    acpi_reset(dev);

    return dev;
}

const device_t acpi_ali_device = {
    .name          = "ALi M7101 ACPI",
    .internal_name = "acpi_ali",
    .flags         = DEVICE_PCI,
    .local         = VEN_ALI,
    .init          = acpi_init,
    .close         = acpi_close,
    .reset         = acpi_reset,
    { .available = NULL },
    .speed_changed = acpi_speed_changed,
    .force_redraw  = NULL,
    .config        = NULL
};

const device_t acpi_intel_device = {
    .name          = "Intel ACPI",
    .internal_name = "acpi_intel",
    .flags         = DEVICE_PCI,
    .local         = VEN_INTEL,
    .init          = acpi_init,
    .close         = acpi_close,
    .reset         = acpi_reset,
    { .available = NULL },
    .speed_changed = acpi_speed_changed,
    .force_redraw  = NULL,
    .config        = NULL
};

const device_t acpi_intel_ich2_device = {
    .name          = "Intel ICH2 ACPI",
    .internal_name = "acpi_intel_ich2",
    .flags         = DEVICE_PCI,
    .local         = VEN_INTEL_ICH2,
    .init          = acpi_init,
    .close         = acpi_close,
    .reset         = acpi_reset,
    { .available = NULL },
    .speed_changed = acpi_speed_changed,
    .force_redraw  = NULL,
    .config        = NULL
};

const device_t acpi_via_device = {
    .name          = "VIA ACPI",
    .internal_name = "acpi_via",
    .flags         = DEVICE_PCI,
    .local         = VEN_VIA,
    .init          = acpi_init,
    .close         = acpi_close,
    .reset         = acpi_reset,
    { .available = NULL },
    .speed_changed = acpi_speed_changed,
    .force_redraw  = NULL,
    .config        = NULL
};

const device_t acpi_via_596b_device = {
    .name          = "VIA VT82C596 ACPI",
    .internal_name = "acpi_via_596b",
    .flags         = DEVICE_PCI,
    .local         = VEN_VIA_596B,
    .init          = acpi_init,
    .close         = acpi_close,
    .reset         = acpi_reset,
    { .available = NULL },
    .speed_changed = acpi_speed_changed,
    .force_redraw  = NULL,
    .config        = NULL
};

const device_t acpi_smc_device = {
    .name          = "SMC FDC73C931APM ACPI",
    .internal_name = "acpi_smc",
    .flags         = DEVICE_PCI,
    .local         = VEN_SMC,
    .init          = acpi_init,
    .close         = acpi_close,
    .reset         = acpi_reset,
    { .available = NULL },
    .speed_changed = acpi_speed_changed,
    .force_redraw  = NULL,
    .config        = NULL
};
