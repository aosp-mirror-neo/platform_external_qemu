/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ARM Generic Interrupt Controller using HVF platform support
 *
 * Copyright (c) 2025 Mohamed Mediouni
 * Based on vGICv3 KVM code by Pavel Fedin
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/intc/arm_gicv3_common.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "system/runstate.h"
#include "system/whpx.h"
#include "system/whpx-internal.h"
#include "gicv3_internal.h"
#include "vgic_common.h"
#include "migration/blocker.h"
#include "qom/object.h"
#include "target/arm/cpregs.h"

#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_ARM64_)
#define _ARM64_
#endif

#include "hw/arm/bsa.h"
#include <winhvplatform.h>
#include <winhvplatformdefs.h>
#include <winnt.h>

#define WHPX_GICV3_REDIST_BASE_GPA 0x080A0000ULL

struct WHPXARMGICv3Class {
    ARMGICv3CommonClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

OBJECT_DECLARE_TYPE(GICv3State, WHPXARMGICv3Class, WHPX_GICV3)

static WHV_ARM64_GLOBAL_INTERRUPT_CONTROLLER_STATE *
whpx_gicv3_read_global_state(const char *action, const char *remedy,
                             UINT32 *bytes_needed_out,
                             uint32_t *max_entries_out)
{
    const size_t header_size =
        offsetof(WHV_ARM64_GLOBAL_INTERRUPT_CONTROLLER_STATE, Interrupts);
    struct whpx_state *whpx = &whpx_global;
    WHV_ARM64_GLOBAL_INTERRUPT_CONTROLLER_STATE *state;
    UINT32 bytes_needed = 0;
    UINT32 bytes_written = 0;
    HRESULT hr;

    hr = whp_dispatch.WHvGetVirtualProcessorState(
        whpx->partition, WHV_ANY_VP,
        WHvVirtualProcessorStateTypeGlobalInterruptState,
        NULL, 0, &bytes_needed);
    if (!FAILED(hr) || bytes_needed < header_size) {
        error_report("WHPX: Failed to query global GICv3 state size during %s "
                     "(hr=0x%08lx, size=%u, min=%zu); %s",
                     action, hr, bytes_needed, header_size, remedy);
        return NULL;
    }

    state = g_malloc0(bytes_needed);
    hr = whp_dispatch.WHvGetVirtualProcessorState(
        whpx->partition, WHV_ANY_VP,
        WHvVirtualProcessorStateTypeGlobalInterruptState,
        state, bytes_needed, &bytes_written);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to read global GICv3 state during %s "
                     "(hr=0x%08lx, size=%u); %s",
                     action, hr, bytes_needed, remedy);
        g_free(state);
        return NULL;
    }

    *bytes_needed_out = bytes_needed;
    *max_entries_out =
        (bytes_written >= header_size)
            ? (bytes_written - header_size) /
                  sizeof(WHV_ARM64_GLOBAL_INTERRUPT_STATE)
            : 0;
    return state;
}

static void whpx_gicv3_apply_global_irq(
    GICv3State *s, const WHV_ARM64_GLOBAL_INTERRUPT_STATE *entry,
    uint32_t slot)
{
    uint32_t irq = entry->InterruptId ? entry->InterruptId
                                      : (GIC_INTERNAL + slot);
    uint32_t vp = entry->TargetVpIndex;

    if (irq < GIC_INTERNAL || irq >= s->num_irq) {
        return;
    }

    gicv3_gicd_enabled_replace(s, irq, entry->InterruptState.Enabled);
    gicv3_gicd_edge_trigger_replace(s, irq,
                                    entry->InterruptState.EdgeTriggered);
    gicv3_gicd_level_replace(s, irq, entry->InterruptState.Asserted);
    gicv3_gicd_pending_replace(s, irq, entry->InterruptState.SetPending);
    gicv3_gicd_active_replace(s, irq, entry->InterruptState.Active);
    s->gicd_ipriority[irq] = entry->InterruptState.GicrIpriorityrConfigured;

    if (vp < s->num_cpu) {
        uint32_t tgtaff = s->cpu[vp].gicr_typer >> 32;
        s->gicd_irouter[irq] =
            (tgtaff & 0xFFFFFF) | ((uint64_t)(tgtaff >> 24) << 32);
    } else {
        s->gicd_irouter[irq] = 0;
    }
    gicv3_cache_target_cpustate(s, irq);
    gicv3_gicd_group_replace(s, irq, 1);
    gicv3_gicd_grpmod_replace(s, irq, 0);
}

static void whpx_gicv3_get_global(GICv3State *s)
{
    UINT32 bytes_needed = 0;
    uint32_t max_entries = 0;
    uint32_t num_entries;
    uint32_t n;
    g_autofree WHV_ARM64_GLOBAL_INTERRUPT_CONTROLLER_STATE *global_state =
        whpx_gicv3_read_global_state(
            "snapshot save",
            "re-save the snapshot or cold-boot if guest interrupts stall",
            &bytes_needed, &max_entries);

    if (!global_state) {
        return;
    }

    num_entries = (global_state->NumInterrupts > 0)
                      ? MIN(global_state->NumInterrupts, max_entries)
                      : max_entries;
    if (global_state->GicdCtlrEnableGrp1A) {
        s->gicd_ctlr |= GICD_CTLR_EN_GRP1NS;
    } else {
        s->gicd_ctlr &= ~GICD_CTLR_EN_GRP1NS;
    }

    for (n = 0; n < num_entries; n++) {
        whpx_gicv3_apply_global_irq(s, &global_state->Interrupts[n], n);
    }
}

static void whpx_gicv3_get_local(GICv3State *s, int cpu_idx)
{
    struct whpx_state *whpx = &whpx_global;
    GICv3CPUState *c = &s->cpu[cpu_idx];
    WHV_ARM64_LOCAL_INTERRUPT_CONTROLLER_STATE local_state;
    UINT32 bytes_written = 0;
    HRESULT hr;
    int j;
    int irq;

    hr = whp_dispatch.WHvGetVirtualProcessorState(
        whpx->partition, cpu_idx,
        WHvVirtualProcessorStateTypeInterruptControllerState,
        &local_state, sizeof(local_state), &bytes_written);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to read local GICv3 state for CPU %d "
                     "(hr=0x%08lx, size=%zu); re-save the snapshot or "
                     "cold-boot if guest interrupts stall",
                     cpu_idx, hr, sizeof(local_state));
        return;
    }

    c->icc_igrpen[GICV3_G1NS] = local_state.IccIgrpen1El1;
    if (local_state.GicrCtlrEnableLpis) {
        c->gicr_ctlr |= GICR_CTLR_ENABLE_LPIS;
    } else {
        c->gicr_ctlr &= ~GICR_CTLR_ENABLE_LPIS;
    }
    c->icc_bpr[GICV3_G1NS] = local_state.IccBpr1El1;
    c->icc_pmr_el1 = local_state.IccPmrEl1;
    c->gicr_propbaser = local_state.GicrPropbaser;
    c->gicr_pendbaser = local_state.GicrPendbaser;

    for (j = 0; j < 4; j++) {
        c->icc_apr[GICV3_G1NS][j] = local_state.IchAp1REl2[j];
    }

    c->gicr_ienabler0 = 0;
    c->edge_trigger = 0xffff;
    c->level = 0;
    c->gicr_ipendr0 = 0;
    c->gicr_iactiver0 = 0;
    c->gicr_igroupr0 = 0xffffffff;

    for (irq = 0; irq < GIC_INTERNAL; irq++) {
        if (local_state.BankedInterruptState[irq].Enabled) {
            c->gicr_ienabler0 |= (1U << irq);
        }
        if (irq >= GIC_NR_SGIS &&
            local_state.BankedInterruptState[irq].EdgeTriggered) {
            c->edge_trigger |= (1U << irq);
        } else if (irq >= GIC_NR_SGIS) {
            c->edge_trigger &= ~(1U << irq);
        }
        if (local_state.BankedInterruptState[irq].Asserted) {
            c->level |= (1U << irq);
        }
        if (local_state.BankedInterruptState[irq].SetPending) {
            c->gicr_ipendr0 |= (1U << irq);
        }
        if (local_state.BankedInterruptState[irq].Active) {
            c->gicr_iactiver0 |= (1U << irq);
        }
        c->gicr_ipriorityr[irq] =
            local_state.BankedInterruptState[irq].GicrIpriorityrConfigured;
    }
}

static void whpx_gicv3_get(GICv3State *s)
{
    struct whpx_state *whpx = &whpx_global;
    int i;

    if (!whpx->partition || !whp_dispatch.WHvGetVirtualProcessorState) {
        return;
    }

    whpx_gicv3_get_global(s);
    for (i = 0; i < s->num_cpu; i++) {
        whpx_gicv3_get_local(s, i);
    }
}

static void whpx_set_reg(CPUState *cpu, WHV_REGISTER_NAME reg, WHV_REGISTER_VALUE val)
{
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;

    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(whpx->partition, cpu->cpu_index,
         &reg, 1, &val);

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set register 0x%08x for CPU %d "
                     "(hr=0x%08lx); cold-boot the emulator if guest "
                     "interrupts stall",
                     reg, cpu->cpu_index, hr);
    }
}

static void whpx_gicv3_fill_local_state(
    WHV_ARM64_LOCAL_INTERRUPT_CONTROLLER_STATE *local_state,
    const WHV_ARM64_LOCAL_INTERRUPT_CONTROLLER_STATE *initial_state,
    const GICv3State *s,
    const GICv3CPUState *c)
{
    int j;
    int irq;

    *local_state = *initial_state;
    local_state->Version = WHV_ARM64_INTERRUPT_CONTROLLER_STATE_VERSION_CURRENT;
    local_state->GicVersion = 3;
    memset(local_state->Reserved0, 0, sizeof(local_state->Reserved0));

    local_state->IccIgrpen1El1 = c->icc_igrpen[GICV3_G1NS] & 1;
    local_state->GicrCtlrEnableLpis =
        (s->lpi_enable && (c->gicr_ctlr & GICR_CTLR_ENABLE_LPIS)) ? 1 : 0;

    if (c->icc_bpr[GICV3_G1NS] < initial_state->IccBpr1El1) {
        local_state->IccBpr1El1 = initial_state->IccBpr1El1;
    } else {
        local_state->IccBpr1El1 = c->icc_bpr[GICV3_G1NS] & 0x7;
    }

    local_state->IccPmrEl1 = c->icc_pmr_el1 & 0xff;
    if (!s->lpi_enable) {
        local_state->GicrPropbaser = initial_state->GicrPropbaser;
        local_state->GicrPendbaser = initial_state->GicrPendbaser;
    } else {
        local_state->GicrPropbaser = c->gicr_propbaser;
        local_state->GicrPendbaser = c->gicr_pendbaser;
    }

    for (j = 0; j < 4; j++) {
        local_state->IchAp1REl2[j] = (uint32_t)c->icc_apr[GICV3_G1NS][j];
    }

    for (irq = 0; irq < GIC_INTERNAL; irq++) {
        uint8_t is_direct = initial_state->BankedInterruptState[irq].Direct ||
                            irq == VIRTUAL_PMU_IRQ ||
                            irq == ARCH_TIMER_VIRT_IRQ;
        uint8_t enabled = (c->gicr_ienabler0 >> irq) & 1;
        uint8_t edge = (irq < GIC_NR_SGIS) ? 1 : ((c->edge_trigger >> irq) & 1);
        uint8_t asserted = (c->level >> irq) & 1;
        uint8_t pending = (c->gicr_ipendr0 >> irq) & 1;
        uint8_t active = (c->gicr_iactiver0 >> irq) & 1;
        uint8_t prio = c->gicr_ipriorityr[irq];

        if (is_direct && irq >= GIC_NR_SGIS) {
            edge = initial_state->BankedInterruptState[irq].EdgeTriggered;
        }
        if (edge) {
            pending |= asserted;
            asserted = 0;
        }

        local_state->BankedInterruptState[irq].Enabled = enabled;
        local_state->BankedInterruptState[irq].EdgeTriggered = edge;
        local_state->BankedInterruptState[irq].Asserted = asserted;
        local_state->BankedInterruptState[irq].SetPending = pending;
        local_state->BankedInterruptState[irq].Active = active;
        local_state->BankedInterruptState[irq].Direct =
            initial_state->BankedInterruptState[irq].Direct;
        local_state->BankedInterruptState[irq].Reserved0 = 0;
        local_state->BankedInterruptState[irq].GicrIpriorityrConfigured = prio;
        local_state->BankedInterruptState[irq].GicrIpriorityrActive =
            active ? prio : 0;
        local_state->BankedInterruptState[irq].Reserved1 = 0;
    }
}

static void whpx_gicv3_fill_global_irq(
    WHV_ARM64_GLOBAL_INTERRUPT_STATE *entry, GICv3State *s, uint32_t slot)
{
    uint32_t irq = entry->InterruptId;
    uint32_t vp = 0;

    /*
     * On a newly created or reset partition before SPIs have been configured,
     * Hyper-V returns InterruptId == 0 for every SPI slot; populate the
     * architectural SPI INTID (32 + slot).
     */
    if (irq == 0) {
        irq = GIC_INTERNAL + slot;
        entry->InterruptId = irq;
    }
    if (irq < GIC_INTERNAL || irq >= s->num_irq) {
        return;
    }

    entry->InterruptState.Enabled = gicv3_gicd_enabled_test(s, irq);
    entry->InterruptState.EdgeTriggered = gicv3_gicd_edge_trigger_test(s, irq);
    entry->InterruptState.Asserted = gicv3_gicd_level_test(s, irq);
    entry->InterruptState.SetPending = gicv3_gicd_pending_test(s, irq);
    entry->InterruptState.Active = gicv3_gicd_active_test(s, irq);
    if (entry->InterruptState.EdgeTriggered) {
        entry->InterruptState.SetPending |= entry->InterruptState.Asserted;
        entry->InterruptState.Asserted = 0;
    }
    entry->InterruptState.GicrIpriorityrConfigured = s->gicd_ipriority[irq];
    entry->InterruptState.GicrIpriorityrActive =
        entry->InterruptState.Active ? s->gicd_ipriority[irq] : 0;

    if (s->gicd_irouter_target[irq]) {
        vp = s->gicd_irouter_target[irq] - s->cpu;
    }
    entry->TargetVpIndex = vp;
    entry->ActiveVpIndex = entry->InterruptState.Active ? vp : 0;
}

static void whpx_gicv3_put_global(GICv3State *s)
{
    struct whpx_state *whpx = &whpx_global;
    UINT32 bytes_needed = 0;
    uint32_t max_entries = 0;
    uint32_t num_entries;
    uint32_t n;
    HRESULT hr;
    g_autofree WHV_ARM64_GLOBAL_INTERRUPT_CONTROLLER_STATE *global_state =
        whpx_gicv3_read_global_state(
            "snapshot restore",
            "cold-boot the emulator if guest interrupts stall",
            &bytes_needed, &max_entries);

    if (!global_state) {
        return;
    }

    num_entries = (global_state->NumInterrupts > 0)
                      ? MIN(global_state->NumInterrupts, max_entries)
                      : max_entries;

    global_state->Version =
        WHV_ARM64_GLOBAL_INTERRUPT_CONTROLLER_STATE_VERSION_CURRENT;
    global_state->GicVersion = 3;
    global_state->NumInterrupts = num_entries;
    global_state->GicdCtlrEnableGrp1A =
        (s->gicd_ctlr & GICD_CTLR_EN_GRP1NS) ? 1 : 0;

    for (n = 0; n < num_entries; n++) {
        whpx_gicv3_fill_global_irq(&global_state->Interrupts[n], s, n);
    }

    hr = whp_dispatch.WHvSetVirtualProcessorState(
        whpx->partition, WHV_ANY_VP,
        WHvVirtualProcessorStateTypeGlobalInterruptState,
        global_state, bytes_needed);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to restore global GICv3 state "
                     "(hr=0x%08lx, size=%u); cold-boot the emulator if guest "
                     "interrupts stall",
                     hr, bytes_needed);
    }
}

static void whpx_gicv3_put_local(GICv3State *s, int cpu_idx)
{
    struct whpx_state *whpx = &whpx_global;
    GICv3CPUState *c = &s->cpu[cpu_idx];
    WHV_ARM64_LOCAL_INTERRUPT_CONTROLLER_STATE initial_state = {0};
    WHV_ARM64_LOCAL_INTERRUPT_CONTROLLER_STATE local_state;
    UINT32 bytes_written = 0;
    HRESULT hr;

    hr = whp_dispatch.WHvGetVirtualProcessorState(
        whpx->partition, cpu_idx,
        WHvVirtualProcessorStateTypeInterruptControllerState,
        &initial_state, sizeof(initial_state), &bytes_written);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to read baseline local GICv3 state for "
                     "CPU %d before restore (hr=0x%08lx, size=%zu); cold-boot "
                     "the emulator if guest interrupts stall",
                     cpu_idx, hr, sizeof(initial_state));
        return;
    }

    whpx_gicv3_fill_local_state(&local_state, &initial_state, s, c);

    hr = whp_dispatch.WHvSetVirtualProcessorState(
        whpx->partition, cpu_idx,
        WHvVirtualProcessorStateTypeInterruptControllerState,
        &local_state, sizeof(local_state));
    if (FAILED(hr)) {
        error_report("WHPX: Failed to restore local GICv3 state for CPU %d "
                     "(hr=0x%08lx, size=%zu); cold-boot the emulator if guest "
                     "interrupts stall",
                     cpu_idx, hr, sizeof(local_state));
    }
}

static void whpx_gicv3_put(GICv3State *s)
{
    struct whpx_state *whpx = &whpx_global;
    int i;

    if (!whpx->partition || !whp_dispatch.WHvGetVirtualProcessorState ||
        !whp_dispatch.WHvSetVirtualProcessorState) {
        return;
    }

    /* Rebuild in-memory routing target pointers from restored gicd_irouter. */
    gicv3_cache_all_target_cpustates(s);
    whpx_gicv3_put_global(s);
    for (i = 0; i < s->num_cpu; i++) {
        whpx_gicv3_put_local(s, i);
    }
}

static void whpx_gicv3_set_irq(void *opaque, int irq, int level)
{
    struct whpx_state *whpx = &whpx_global;
    GICv3State *s = opaque;
    WHV_INTERRUPT_CONTROL interrupt_control = {
        .InterruptControl.InterruptType = WHvArm64InterruptTypeFixed,
        .RequestedVector = GIC_INTERNAL + irq,
        .InterruptControl.Asserted = level
    };

    if (irq > s->num_irq) {
        return;
    }

    whp_dispatch.WHvRequestInterrupt(whpx->partition, &interrupt_control,
         sizeof(interrupt_control));
}

static void whpx_gicv3_icc_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    GICv3CPUState *c;

    c = env->gicv3state;

    c->icc_pmr_el1 = 0;
    /*
     * Architecturally the reset value of the ICC_BPR registers
     * is UNKNOWN. We set them all to 0 here; when the kernel
     * uses these values to program the ICH_VMCR_EL2 fields that
     * determine the guest-visible ICC_BPR register values, the
     * hardware's "writing a value less than the minimum sets
     * the field to the minimum value" behaviour will result in
     * them effectively resetting to the correct minimum value
     * for the host GIC.
     */
    c->icc_bpr[GICV3_G0] = 0;
    c->icc_bpr[GICV3_G1] = 0;
    c->icc_bpr[GICV3_G1NS] = 0;

    c->icc_sre_el1 = 0x7;
    memset(c->icc_apr, 0, sizeof(c->icc_apr));
    memset(c->icc_igrpen, 0, sizeof(c->icc_igrpen));
}

static void whpx_gicv3_reset_hold(Object *obj, ResetType type)
{
    GICv3State *s = ARM_GICV3_COMMON(obj);
    WHPXARMGICv3Class *kgc = WHPX_GICV3_GET_CLASS(s);
    int i;

    if (kgc->parent_phases.hold) {
        kgc->parent_phases.hold(obj, type);
    }

    /*
     * whpx_arm_system_reset() resets the Hyper-V partition (including its
     * in-kernel vGICv3 state) via WHvResetPartition before sysbus devices run
     * reset_hold; re-apply each vCPU's redistributor base GPA afterwards.
     */
    for (i = 0; i < s->num_cpu; i++) {
        CPUState *cpu_state = qemu_get_cpu(i);
        WHV_REGISTER_VALUE val = {
            .Reg64 = WHPX_GICV3_REDIST_BASE_GPA + (GICV3_REDIST_SIZE * i)
        };
        whpx_set_reg(cpu_state, WHvArm64RegisterGicrBaseGpa, val);
    }
}

/*
 * CPU interface registers of GIC needs to be reset on CPU reset.
 * For the calling arm_gicv3_icc_reset() on CPU reset, we register
 * below ARMCPRegInfo. As we reset the whole cpu interface under single
 * register reset, we define only one register of CPU interface instead
 * of defining all the registers.
 */
static const ARMCPRegInfo gicv3_cpuif_reginfo[] = {
    { .name = "ICC_CTLR_EL1", .state = ARM_CP_STATE_BOTH,
      .opc0 = 3, .opc1 = 0, .crn = 12, .crm = 12, .opc2 = 4,
      /*
       * If ARM_CP_NOP is used, resetfn is not called,
       * So ARM_CP_NO_RAW is appropriate type.
       */
      .type = ARM_CP_NO_RAW,
      .access = PL1_RW,
      .readfn = arm_cp_read_zero,
      .writefn = arm_cp_write_ignore,
      /*
       * We hang the whole cpu interface reset routine off here
       * rather than parcelling it out into one little function
       * per register
       */
      .resetfn = whpx_gicv3_icc_reset,
    },
};

static void whpx_gicv3_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    GICv3State *s = WHPX_GICV3(dev);
    WHPXARMGICv3Class *kgc = WHPX_GICV3_GET_CLASS(s);
    int i;

    kgc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    if (s->revision != 3) {
        error_setg(errp, "unsupported GIC revision %d for platform GIC",
                   s->revision);
        return;
    }

    if (s->security_extn) {
        error_setg(errp, "the platform vGICv3 does not implement the "
                   "security extensions");
        return;
    }

    if (s->nmi_support) {
        error_setg(errp, "NMI is not supported with the platform GIC");
        return;
    }

    if (s->nb_redist_regions > 1) {
        error_setg(errp, "Multiple VGICv3 redistributor regions are not "
                   "supported by WHPX");
        error_append_hint(errp, "A maximum of %d VCPUs can be used",
                          s->redist_region_count[0]);
        return;
    }

    gicv3_init_irqs_and_mmio(s, whpx_gicv3_set_irq, NULL);

    for (i = 0; i < s->num_cpu; i++) {
        CPUState *cpu_state = qemu_get_cpu(i);
        ARMCPU *cpu = ARM_CPU(cpu_state);
        WHV_REGISTER_VALUE val = {
            .Reg64 = WHPX_GICV3_REDIST_BASE_GPA + (GICV3_REDIST_SIZE * i)
        };
        whpx_set_reg(cpu_state, WHvArm64RegisterGicrBaseGpa, val);
        define_arm_cp_regs(cpu, gicv3_cpuif_reginfo);
    }

    if (s->maint_irq) {
        error_setg(errp, "Nested virtualisation not currently supported by WHPX.");
        return;
    }

    if (!whp_dispatch.WHvGetVirtualProcessorState ||
        !whp_dispatch.WHvSetVirtualProcessorState) {
        error_setg(&s->migration_blocker,
            "Live migration disabled because WHvGetVirtualProcessorState / "
            "WHvSetVirtualProcessorState is not supported by host WHPX");
        if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
            return;
        }
    }
}

static void whpx_gicv3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    ARMGICv3CommonClass *agcc = ARM_GICV3_COMMON_CLASS(klass);
    WHPXARMGICv3Class *kgc = WHPX_GICV3_CLASS(klass);

    agcc->pre_save = whpx_gicv3_get;
    agcc->post_load = whpx_gicv3_put;

    device_class_set_parent_realize(dc, whpx_gicv3_realize,
                                    &kgc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, whpx_gicv3_reset_hold, NULL,
                                       &kgc->parent_phases);
}

static const TypeInfo whpx_arm_gicv3_info = {
    .name = TYPE_WHPX_GICV3,
    .parent = TYPE_ARM_GICV3_COMMON,
    .instance_size = sizeof(GICv3State),
    .class_init = whpx_gicv3_class_init,
    .class_size = sizeof(WHPXARMGICv3Class),
};

static void whpx_gicv3_register_types(void)
{
    type_register_static(&whpx_arm_gicv3_info);
}

type_init(whpx_gicv3_register_types)
