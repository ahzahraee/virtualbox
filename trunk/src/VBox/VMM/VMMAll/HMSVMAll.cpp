/* $Id$ */
/** @file
 * HM SVM (AMD-V) - All contexts.
 */

/*
 * Copyright (C) 2017 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_HM
#define VMCPU_INCL_CPUM_GST_CTX
#include "HMInternal.h"
#include <VBox/vmm/apic.h>
#include <VBox/vmm/gim.h>
#include <VBox/vmm/hm.h>
#include <VBox/vmm/iem.h>
#include <VBox/vmm/vm.h>
#include <VBox/vmm/hm_svm.h>


#ifndef IN_RC
/**
 * Emulates a simple MOV TPR (CR8) instruction.
 *
 * Used for TPR patching on 32-bit guests. This simply looks up the patch record
 * at EIP and does the required.
 *
 * This VMMCALL is used a fallback mechanism when mov to/from cr8 isn't exactly
 * like how we want it to be (e.g. not followed by shr 4 as is usually done for
 * TPR). See hmR3ReplaceTprInstr() for the details.
 *
 * @returns VBox status code.
 * @retval VINF_SUCCESS if the access was handled successfully.
 * @retval VERR_NOT_FOUND if no patch record for this RIP could be found.
 * @retval VERR_SVM_UNEXPECTED_PATCH_TYPE if the found patch type is invalid.
 *
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pCtx                Pointer to the guest-CPU context.
 * @param   pfUpdateRipAndRF    Whether the guest RIP/EIP has been updated as
 *                              part of the TPR patch operation.
 */
static int hmSvmEmulateMovTpr(PVMCPU pVCpu, PCPUMCTX pCtx, bool *pfUpdateRipAndRF)
{
    Log4(("Emulated VMMCall TPR access replacement at RIP=%RGv\n", pCtx->rip));

    /*
     * We do this in a loop as we increment the RIP after a successful emulation
     * and the new RIP may be a patched instruction which needs emulation as well.
     */
    bool fUpdateRipAndRF = false;
    bool fPatchFound     = false;
    PVM  pVM = pVCpu->CTX_SUFF(pVM);
    for (;;)
    {
        bool    fPending;
        uint8_t u8Tpr;

        PHMTPRPATCH pPatch = (PHMTPRPATCH)RTAvloU32Get(&pVM->hm.s.PatchTree, (AVLOU32KEY)pCtx->eip);
        if (!pPatch)
            break;

        fPatchFound = true;
        switch (pPatch->enmType)
        {
            case HMTPRINSTR_READ:
            {
                int rc = APICGetTpr(pVCpu, &u8Tpr, &fPending, NULL /* pu8PendingIrq */);
                AssertRC(rc);

                rc = DISWriteReg32(CPUMCTX2CORE(pCtx), pPatch->uDstOperand, u8Tpr);
                AssertRC(rc);
                pCtx->rip += pPatch->cbOp;
                pCtx->eflags.Bits.u1RF = 0;
                fUpdateRipAndRF = true;
                break;
            }

            case HMTPRINSTR_WRITE_REG:
            case HMTPRINSTR_WRITE_IMM:
            {
                if (pPatch->enmType == HMTPRINSTR_WRITE_REG)
                {
                    uint32_t u32Val;
                    int rc = DISFetchReg32(CPUMCTX2CORE(pCtx), pPatch->uSrcOperand, &u32Val);
                    AssertRC(rc);
                    u8Tpr = u32Val;
                }
                else
                    u8Tpr = (uint8_t)pPatch->uSrcOperand;

                int rc2 = APICSetTpr(pVCpu, u8Tpr);
                AssertRC(rc2);
                HMCPU_CF_SET(pVCpu, HM_CHANGED_SVM_GUEST_APIC_STATE);

                pCtx->rip += pPatch->cbOp;
                pCtx->eflags.Bits.u1RF = 0;
                fUpdateRipAndRF = true;
                break;
            }

            default:
            {
                AssertMsgFailed(("Unexpected patch type %d\n", pPatch->enmType));
                pVCpu->hm.s.u32HMError = pPatch->enmType;
                *pfUpdateRipAndRF = fUpdateRipAndRF;
                return VERR_SVM_UNEXPECTED_PATCH_TYPE;
            }
        }
    }

    *pfUpdateRipAndRF = fUpdateRipAndRF;
    if (fPatchFound)
        return VINF_SUCCESS;
    return VERR_NOT_FOUND;
}


/**
 * Notification callback for when a \#VMEXIT happens outside SVM R0 code (e.g.
 * in IEM).
 *
 * @param   pVCpu           The cross context virtual CPU structure.
 * @param   pCtx            Pointer to the guest-CPU context.
 *
 * @sa      hmR0SvmVmRunCacheVmcb.
 */
VMM_INT_DECL(void) HMSvmNstGstVmExitNotify(PVMCPU pVCpu, PCPUMCTX pCtx)
{
    /*
     * Restore the nested-guest VMCB fields which have been modified for executing
     * the nested-guest under SVM R0.
     */
    if (pCtx->hwvirt.svm.fHMCachedVmcb)
    {
        PSVMVMCB            pVmcbNstGst      = pCtx->hwvirt.svm.CTX_SUFF(pVmcb);
        PSVMVMCBCTRL        pVmcbNstGstCtrl  = &pVmcbNstGst->ctrl;
        PSVMVMCBSTATESAVE   pVmcbNstGstState = &pVmcbNstGst->guest;
        PSVMNESTEDVMCBCACHE pNstGstVmcbCache = &pVCpu->hm.s.svm.NstGstVmcbCache;

        /*
         * The fields that are guaranteed to be read-only during SVM guest execution
         * can safely be restored from our VMCB cache. Other fields like control registers
         * are already updated by hardware-assisted SVM or by IEM. We only restore those
         * fields that are potentially modified by hardware-assisted SVM.
         */
        pVmcbNstGstCtrl->u16InterceptRdCRx             = pNstGstVmcbCache->u16InterceptRdCRx;
        pVmcbNstGstCtrl->u16InterceptWrCRx             = pNstGstVmcbCache->u16InterceptWrCRx;
        pVmcbNstGstCtrl->u16InterceptRdDRx             = pNstGstVmcbCache->u16InterceptRdDRx;
        pVmcbNstGstCtrl->u16InterceptWrDRx             = pNstGstVmcbCache->u16InterceptWrDRx;
        pVmcbNstGstCtrl->u32InterceptXcpt              = pNstGstVmcbCache->u32InterceptXcpt;
        pVmcbNstGstCtrl->u64InterceptCtrl              = pNstGstVmcbCache->u64InterceptCtrl;
        pVmcbNstGstState->u64DBGCTL                    = pNstGstVmcbCache->u64DBGCTL;
        pVmcbNstGstCtrl->u32VmcbCleanBits              = pNstGstVmcbCache->u32VmcbCleanBits;
        pVmcbNstGstCtrl->u64IOPMPhysAddr               = pNstGstVmcbCache->u64IOPMPhysAddr;
        pVmcbNstGstCtrl->u64MSRPMPhysAddr              = pNstGstVmcbCache->u64MSRPMPhysAddr;
        pVmcbNstGstCtrl->u64TSCOffset                  = pNstGstVmcbCache->u64TSCOffset;
        pVmcbNstGstCtrl->IntCtrl.n.u1VIntrMasking      = pNstGstVmcbCache->fVIntrMasking;
        pVmcbNstGstCtrl->TLBCtrl                       = pNstGstVmcbCache->TLBCtrl;
        pVmcbNstGstCtrl->NestedPaging.n.u1NestedPaging = pNstGstVmcbCache->u1NestedPaging;
        pVmcbNstGstCtrl->LbrVirt.n.u1LbrVirt           = pNstGstVmcbCache->u1LbrVirt;
        pCtx->hwvirt.svm.fHMCachedVmcb = false;
    }

    /*
     * Currently, VMRUN, #VMEXIT transitions involves trips to ring-3 that would flag a full
     * CPU state change. However, if we exit to ring-3 in response to receiving a physical
     * interrupt, we skip signaling any CPU state change as normally no change
     * is done to the execution state (see VINF_EM_RAW_INTERRUPT handling in hmR0SvmExitToRing3).
     * However, with nested-guests, the state can change for e.g., we might perform a
     * SVM_EXIT_INTR #VMEXIT for the nested-guest in ring-3. Hence we signal a full CPU
     * state change here.
     */
    HMCPU_CF_SET(pVCpu, HM_CHANGED_ALL_GUEST);
}


/**
 * Checks if the Virtual GIF (Global Interrupt Flag) feature is supported and
 * enabled for the VM.
 *
 * @returns @c true if VGIF is enabled, @c false otherwise.
 * @param   pVM         The cross context VM structure.
 *
 * @remarks This value returned by this functions is expected by the callers not
 *          to change throughout the lifetime of the VM.
 */
VMM_INT_DECL(bool) HMSvmIsVGifActive(PVM pVM)
{
    bool const fVGif    = RT_BOOL(pVM->hm.s.svm.u32Features & X86_CPUID_SVM_FEATURE_EDX_VGIF);
    bool const fUseVGif = fVGif && pVM->hm.s.svm.fVGif;

    return HMIsEnabled(pVM) && fVGif && fUseVGif;
}


/**
 * Applies the TSC offset of an SVM nested-guest if any and returns the new TSC
 * value for the nested-guest.
 *
 * @returns The TSC offset after applying any nested-guest TSC offset.
 * @param   pVCpu       The cross context virtual CPU structure of the calling EMT.
 * @param   uTicks      The guest TSC.
 *
 * @remarks This function looks at the VMCB cache rather than directly at the
 *          nested-guest VMCB. The latter may have been modified for executing
 *          using hardware-assisted SVM.
 *
 * @sa      CPUMApplyNestedGuestTscOffset.
 */
VMM_INT_DECL(uint64_t) HMSvmNstGstApplyTscOffset(PVMCPU pVCpu, uint64_t uTicks)
{
    PCCPUMCTX pCtx = &pVCpu->cpum.GstCtx;
    Assert(CPUMIsGuestInSvmNestedHwVirtMode(pCtx));
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb);
    NOREF(pCtx);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;
    return uTicks + pVmcbNstGstCache->u64TSCOffset;
}
#endif /* !IN_RC */


/**
 * Performs the operations necessary that are part of the vmmcall instruction
 * execution in the guest.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_SUCCESS on successful handling, no \#UD needs to be thrown,
 *          update RIP and eflags.RF depending on @a pfUpdatedRipAndRF and
 *          continue guest execution.
 * @retval  VINF_GIM_HYPERCALL_CONTINUING continue hypercall without updating
 *          RIP.
 * @retval  VINF_GIM_R3_HYPERCALL re-start the hypercall from ring-3.
 *
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pCtx                Pointer to the guest-CPU context.
 * @param   pfUpdatedRipAndRF   Whether the guest RIP/EIP has been updated as
 *                              part of handling the VMMCALL operation.
 */
VMM_INT_DECL(VBOXSTRICTRC) HMSvmVmmcall(PVMCPU pVCpu, PCPUMCTX pCtx, bool *pfUpdatedRipAndRF)
{
#ifndef IN_RC
    /*
     * TPR patched instruction emulation for 32-bit guests.
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (pVM->hm.s.fTprPatchingAllowed)
    {
        int rc = hmSvmEmulateMovTpr(pVCpu, pCtx, pfUpdatedRipAndRF);
        if (RT_SUCCESS(rc))
            return VINF_SUCCESS;

        if (rc != VERR_NOT_FOUND)
        {
            Log(("hmSvmExitVmmCall: hmSvmEmulateMovTpr returns %Rrc\n", rc));
            return rc;
        }
    }
#endif

    /*
     * Paravirtualized hypercalls.
     */
    *pfUpdatedRipAndRF = false;
    if (pVCpu->hm.s.fHypercallsEnabled)
        return GIMHypercall(pVCpu, pCtx);

    return VERR_NOT_AVAILABLE;
}


/**
 * Converts an SVM event type to a TRPM event type.
 *
 * @returns The TRPM event type.
 * @retval  TRPM_32BIT_HACK if the specified type of event isn't among the set
 *          of recognized trap types.
 *
 * @param   pEvent       Pointer to the SVM event.
 */
VMM_INT_DECL(TRPMEVENT) HMSvmEventToTrpmEventType(PCSVMEVENT pEvent)
{
    uint8_t const uType = pEvent->n.u3Type;
    switch (uType)
    {
        case SVM_EVENT_EXTERNAL_IRQ:    return TRPM_HARDWARE_INT;
        case SVM_EVENT_SOFTWARE_INT:    return TRPM_SOFTWARE_INT;
        case SVM_EVENT_EXCEPTION:
        case SVM_EVENT_NMI:             return TRPM_TRAP;
        default:
            break;
    }
    AssertMsgFailed(("HMSvmEventToTrpmEvent: Invalid pending-event type %#x\n", uType));
    return TRPM_32BIT_HACK;
}


/**
 * Gets the MSR permission bitmap byte and bit offset for the specified MSR.
 *
 * @returns VBox status code.
 * @param   idMsr       The MSR being requested.
 * @param   pbOffMsrpm  Where to store the byte offset in the MSR permission
 *                      bitmap for @a idMsr.
 * @param   puMsrpmBit  Where to store the bit offset starting at the byte
 *                      returned in @a pbOffMsrpm.
 */
VMM_INT_DECL(int) HMSvmGetMsrpmOffsetAndBit(uint32_t idMsr, uint16_t *pbOffMsrpm, uint32_t *puMsrpmBit)
{
    Assert(pbOffMsrpm);
    Assert(puMsrpmBit);

    /*
     * MSRPM Layout:
     * Byte offset          MSR range
     * 0x000  - 0x7ff       0x00000000 - 0x00001fff
     * 0x800  - 0xfff       0xc0000000 - 0xc0001fff
     * 0x1000 - 0x17ff      0xc0010000 - 0xc0011fff
     * 0x1800 - 0x1fff              Reserved
     *
     * Each MSR is represented by 2 permission bits (read and write).
     */
    if (idMsr <= 0x00001fff)
    {
        /* Pentium-compatible MSRs. */
        *pbOffMsrpm = 0;
        *puMsrpmBit = idMsr << 1;
        return VINF_SUCCESS;
    }

    if (   idMsr >= 0xc0000000
        && idMsr <= 0xc0001fff)
    {
        /* AMD Sixth Generation x86 Processor MSRs. */
        *pbOffMsrpm = 0x800;
        *puMsrpmBit = (idMsr - 0xc0000000) << 1;
        return VINF_SUCCESS;
    }

    if (   idMsr >= 0xc0010000
        && idMsr <= 0xc0011fff)
    {
        /* AMD Seventh and Eighth Generation Processor MSRs. */
        *pbOffMsrpm = 0x1000;
        *puMsrpmBit = (idMsr - 0xc0010000) << 1;
        return VINF_SUCCESS;
    }

    *pbOffMsrpm = 0;
    *puMsrpmBit = 0;
    return VERR_OUT_OF_RANGE;
}


/**
 * Determines whether an IOIO intercept is active for the nested-guest or not.
 *
 * @param   pvIoBitmap      Pointer to the nested-guest IO bitmap.
 * @param   u16Port         The IO port being accessed.
 * @param   enmIoType       The type of IO access.
 * @param   cbReg           The IO operand size in bytes.
 * @param   cAddrSizeBits   The address size bits (for 16, 32 or 64).
 * @param   iEffSeg         The effective segment number.
 * @param   fRep            Whether this is a repeating IO instruction (REP prefix).
 * @param   fStrIo          Whether this is a string IO instruction.
 * @param   pIoExitInfo     Pointer to the SVMIOIOEXITINFO struct to be filled.
 *                          Optional, can be NULL.
 */
VMM_INT_DECL(bool) HMSvmIsIOInterceptActive(void *pvIoBitmap, uint16_t u16Port, SVMIOIOTYPE enmIoType, uint8_t cbReg,
                                            uint8_t cAddrSizeBits, uint8_t iEffSeg, bool fRep, bool fStrIo,
                                            PSVMIOIOEXITINFO pIoExitInfo)
{
    Assert(cAddrSizeBits == 16 || cAddrSizeBits == 32 || cAddrSizeBits == 64);
    Assert(cbReg == 1 || cbReg == 2 || cbReg == 4 || cbReg == 8);

    /*
     * The IOPM layout:
     * Each bit represents one 8-bit port. That makes a total of 0..65535 bits or
     * two 4K pages.
     *
     * For IO instructions that access more than a single byte, the permission bits
     * for all bytes are checked; if any bit is set to 1, the IO access is intercepted.
     *
     * Since it's possible to do a 32-bit IO access at port 65534 (accessing 4 bytes),
     * we need 3 extra bits beyond the second 4K page.
     */
    static const uint16_t s_auSizeMasks[] = { 0, 1, 3, 0, 0xf, 0, 0, 0 };

    uint16_t const offIopm   = u16Port >> 3;
    uint16_t const fSizeMask = s_auSizeMasks[(cAddrSizeBits >> SVM_IOIO_OP_SIZE_SHIFT) & 7];
    uint8_t  const cShift    = u16Port - (offIopm << 3);
    uint16_t const fIopmMask = (1 << cShift) | (fSizeMask << cShift);

    uint8_t const *pbIopm = (uint8_t *)pvIoBitmap;
    Assert(pbIopm);
    pbIopm += offIopm;
    uint16_t const u16Iopm = *(uint16_t *)pbIopm;
    if (u16Iopm & fIopmMask)
    {
        if (pIoExitInfo)
        {
            static const uint32_t s_auIoOpSize[] =
            { SVM_IOIO_32_BIT_OP, SVM_IOIO_8_BIT_OP, SVM_IOIO_16_BIT_OP, 0, SVM_IOIO_32_BIT_OP, 0, 0, 0 };

            static const uint32_t s_auIoAddrSize[] =
            { 0, SVM_IOIO_16_BIT_ADDR, SVM_IOIO_32_BIT_ADDR, 0, SVM_IOIO_64_BIT_ADDR, 0, 0, 0 };

            pIoExitInfo->u         = s_auIoOpSize[cbReg & 7];
            pIoExitInfo->u        |= s_auIoAddrSize[(cAddrSizeBits >> 4) & 7];
            pIoExitInfo->n.u1STR   = fStrIo;
            pIoExitInfo->n.u1REP   = fRep;
            pIoExitInfo->n.u3SEG   = iEffSeg & 7;
            pIoExitInfo->n.u1Type  = enmIoType;
            pIoExitInfo->n.u16Port = u16Port;
        }
        return true;
    }

    /** @todo remove later (for debugging as VirtualBox always traps all IO
     *        intercepts). */
    AssertMsgFailed(("iemSvmHandleIOIntercept: We expect an IO intercept here!\n"));
    return false;
}


/**
 * Checks if the guest VMCB has the specified ctrl/instruction intercept active.
 *
 * @returns @c true if in intercept is set, @c false otherwise.
 * @param   pVCpu       The cross context virtual CPU structure of the calling EMT.
 * @param   pCtx        Pointer to the context.
 * @param   fIntercept  The SVM control/instruction intercept, see
 *                      SVM_CTRL_INTERCEPT_*.
 */
VMM_INT_DECL(bool) HMIsGuestSvmCtrlInterceptSet(PVMCPU pVCpu, PCPUMCTX pCtx, uint64_t fIntercept)
{
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb); NOREF(pCtx);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;
    return RT_BOOL(pVmcbNstGstCache->u64InterceptCtrl & fIntercept);
}


/**
 * Checks if the guest VMCB has the specified CR read intercept active.
 *
 * @returns @c true if in intercept is set, @c false otherwise.
 * @param   pVCpu   The cross context virtual CPU structure of the calling EMT.
 * @param   pCtx    Pointer to the context.
 * @param   uCr     The CR register number (0 to 15).
 */
VMM_INT_DECL(bool) HMIsGuestSvmReadCRxInterceptSet(PVMCPU pVCpu, PCCPUMCTX pCtx, uint8_t uCr)
{
    Assert(uCr < 16);
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb); NOREF(pCtx);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;
    return RT_BOOL(pVmcbNstGstCache->u16InterceptRdCRx & (1 << uCr));
}


/**
 * Checks if the guest VMCB has the specified CR write intercept
 * active.
 *
 * @returns @c true if in intercept is set, @c false otherwise.
 * @param   pVCpu   The cross context virtual CPU structure of the calling EMT.
 * @param   pCtx    Pointer to the context.
 * @param   uCr     The CR register number (0 to 15).
 */
VMM_INT_DECL(bool) HMIsGuestSvmWriteCRxInterceptSet(PVMCPU pVCpu, PCCPUMCTX pCtx, uint8_t uCr)
{
    Assert(uCr < 16);
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb); NOREF(pCtx);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;
    return RT_BOOL(pVmcbNstGstCache->u16InterceptWrCRx & (1 << uCr));
}


/**
 * Checks if the guest VMCB has the specified DR read intercept
 * active.
 *
 * @returns @c true if in intercept is set, @c false otherwise.
 * @param   pVCpu   The cross context virtual CPU structure of the calling EMT.
 * @param   pCtx    Pointer to the context.
 * @param   uDr     The DR register number (0 to 15).
 */
VMM_INT_DECL(bool) HMIsGuestSvmReadDRxInterceptSet(PVMCPU pVCpu, PCCPUMCTX pCtx, uint8_t uDr)
{
    Assert(uDr < 16);
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb); NOREF(pCtx);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;
    return RT_BOOL(pVmcbNstGstCache->u16InterceptRdDRx & (1 << uDr));
}


/**
 * Checks if the guest VMCB has the specified DR write intercept active.
 *
 * @returns @c true if in intercept is set, @c false otherwise.
 * @param   pVCpu   The cross context virtual CPU structure of the calling EMT.
 * @param   pCtx    Pointer to the context.
 * @param   uDr     The DR register number (0 to 15).
 */
VMM_INT_DECL(bool) HMIsGuestSvmWriteDRxInterceptSet(PVMCPU pVCpu, PCCPUMCTX pCtx, uint8_t uDr)
{
    Assert(uDr < 16);
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb); NOREF(pCtx);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;
    return RT_BOOL(pVmcbNstGstCache->u16InterceptWrDRx & (1 << uDr));
}


/**
 * Checks if the guest VMCB has the specified exception intercept active.
 *
 * @returns true if in intercept is active, false otherwise.
 * @param   pVCpu       The cross context virtual CPU structure of the calling EMT.
 * @param   pCtx        Pointer to the context.
 * @param   uVector     The exception / interrupt vector.
 */
VMM_INT_DECL(bool) HMIsGuestSvmXcptInterceptSet(PVMCPU pVCpu, PCCPUMCTX pCtx, uint8_t uVector)
{
    Assert(uVector < 32);
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb); NOREF(pCtx);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;
    return RT_BOOL(pVmcbNstGstCache->u32InterceptXcpt & (1 << uVector));
}


/**
 * Checks whether the SVM nested-guest is in a state to receive physical (APIC)
 * interrupts.
 *
 * @returns true if it's ready, false otherwise.
 * @param   pVCpu       The cross context virtual CPU structure of the calling EMT.
 * @param   pCtx        The guest-CPU context.
 *
 * @remarks This function looks at the VMCB cache rather than directly at the
 *          nested-guest VMCB. The latter may have been modified for executing
 *          using hardware-assisted SVM.
 *
 * @sa      CPUMCanSvmNstGstTakePhysIntr.
 */
VMM_INT_DECL(bool) HMCanSvmNstGstTakePhysIntr(PVMCPU pVCpu, PCCPUMCTX pCtx)
{
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb);
    Assert(pCtx->hwvirt.fGif);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;
    X86EFLAGS fEFlags;
    if (pVmcbNstGstCache->fVIntrMasking)
        fEFlags.u = pCtx->hwvirt.svm.HostState.rflags.u;
    else
        fEFlags.u = pCtx->eflags.u;
    return fEFlags.Bits.u1IF;
}


/**
 * Checks whether the SVM nested-guest is in a state to receive virtual (setup
 * for injection by VMRUN instruction) interrupts.
 *
 * @returns true if it's ready, false otherwise.
 * @param   pVCpu       The cross context virtual CPU structure of the calling EMT.
 * @param   pCtx        The guest-CPU context.
 *
 * @remarks This function looks at the VMCB cache rather than directly at the
 *          nested-guest VMCB. The latter may have been modified for executing
 *          using hardware-assisted SVM.
 *
 * @sa      CPUMCanSvmNstGstTakeVirtIntr.
 */
VMM_INT_DECL(bool) HMCanSvmNstGstTakeVirtIntr(PVMCPU pVCpu, PCCPUMCTX pCtx)
{
#ifdef IN_RC
    RT_NOREF2(pVCpu, pCtx);
    AssertReleaseFailedReturn(false);
#else
    Assert(pCtx->hwvirt.svm.fHMCachedVmcb);
    Assert(pCtx->hwvirt.fGif);
    PCSVMNESTEDVMCBCACHE pVmcbNstGstCache = &pVCpu->hm.s.svm.NstGstVmcbCache;

    PCSVMVMCBCTRL pVmcbCtrl = &pCtx->hwvirt.svm.CTX_SUFF(pVmcb)->ctrl;
    if (   !pVmcbCtrl->IntCtrl.n.u1IgnoreTPR
        &&  pVmcbCtrl->IntCtrl.n.u4VIntrPrio <= pVmcbCtrl->IntCtrl.n.u8VTPR)
        return false;

    X86EFLAGS fEFlags;
    if (pVmcbNstGstCache->fVIntrMasking)
        fEFlags.u = pCtx->eflags.u;
    else
        fEFlags.u = pCtx->hwvirt.svm.HostState.rflags.u;
    return fEFlags.Bits.u1IF;
#endif
}

