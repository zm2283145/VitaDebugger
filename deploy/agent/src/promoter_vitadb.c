/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2025 Rinnegatamante and VitaDB-Downloader contributors
 *
 * The internal-PAF argument block and asynchronous PromoterUtil sequence in
 * this file are adapted from VitaDB-Downloader source/promoter.cpp and
 * source/main.cpp at commit 415033d90e08a6bc0a30e2ee6e9456db600d7b22.
 * VitaDevDeploy adds complete return checking, terminal-result validation,
 * progress callbacks, power ticks, and reverse-order cleanup.
 */

#include "promoter.h"

#include "common.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#include <stdint.h>
#include <string.h>

#define VDEV_PROMOTE_POLL_MICROSECONDS (100u * 1000u)

static int load_sce_paf(void)
{
    uint32_t option_words[0x100];
    static uint32_t arguments[] = {
        0x00400000u, 0x0000EA60u, 0x00040000u, 0u, 0u
    };

    memset(option_words, 0, sizeof(option_words));
    option_words[1] = (uint32_t)(uintptr_t)&option_words[0];
    return sceSysmoduleLoadModuleInternalWithArg(
        SCE_SYSMODULE_INTERNAL_PAF,
        (SceSize)sizeof(arguments), arguments,
        (SceSysmoduleOpt *)(void *)option_words);
}

static int unload_sce_paf(void)
{
    SceSysmoduleOpt option;
    memset(&option, 0, sizeof(option));
    return sceSysmoduleUnloadModuleInternalWithArg(
        SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, &option);
}

static void save_cleanup_error(VdevPromoteReport *report, int result)
{
    if (result < 0 && report->cleanup_code == 0) {
        report->cleanup_code = result;
    }
}

int vdev_promote_package(const char *package_directory,
                         VdevPromoteReadyCallback ready_callback,
                         void *ready_context,
                         VdevPromoteProgressCallback progress_callback,
                         void *progress_context,
                         VdevPromoteReport *report)
{
    uint64_t start_time;
    int result;
    int operation_result = VDEV_ERR_PROMOTE_UNKNOWN;
    int state = 1;
    int paf_loaded = 0;
    int promoter_loaded = 0;
    int promoter_initialized = 0;

    if (package_directory == NULL || report == NULL) {
        return VDEV_ERR_INTERNAL;
    }
    memset(report, 0, sizeof(*report));
    report->operation_code = VDEV_ERR_PROMOTE_UNKNOWN;
    report->last_state = -1;

    result = load_sce_paf();
    if (result < 0) {
        report->operation_code = result;
        report->outcome_known = 1;
        goto cleanup;
    }
    paf_loaded = 1;

    result = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (result < 0) {
        report->operation_code = result;
        report->outcome_known = 1;
        goto cleanup;
    }
    promoter_loaded = 1;

    result = scePromoterUtilityInit();
    if (result < 0) {
        report->operation_code = result;
        report->outcome_known = 1;
        goto cleanup;
    }
    promoter_initialized = 1;

    /*
     * Load and initialize the system service before the caller's final hash.
     * Once that callback succeeds, dispatch is the next operation, minimizing
     * the unavoidable window in which the FTP-visible tree could be changed.
     */
    if (ready_callback != NULL) {
        result = ready_callback(ready_context);
        if (result < 0) {
            report->operation_code = result;
            report->outcome_known = 1;
            goto cleanup;
        }
    }

    result = scePromoterUtilityPromotePkg(package_directory, 0);
    if (result != 0) {
        report->operation_code = result < 0 ? result : VDEV_ERR_PROMOTE_RESULT;
        report->outcome_known = 1;
        goto cleanup;
    }
    report->dispatched = 1;
    start_time = sceKernelGetProcessTimeWide();

    for (;;) {
        const uint64_t now = sceKernelGetProcessTimeWide();
        const uint64_t elapsed_ms = (now - start_time) / UINT64_C(1000);

        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        result = scePromoterUtilityGetState(&state);
        if (result < 0) {
            /*
             * There is no documented cancellation or ownership-transfer API.
             * Once PromotePkg accepts the request, returning an ordinary
             * failure (and unloading PromoterUtil) could make a host retry an
             * installation that is still active.  Keep the service alive and
             * retry status reads until a terminal state can be proven.
             */
            if (progress_callback != NULL) {
                progress_callback(VDEV_PROMOTE_PROGRESS_STATE_UNAVAILABLE,
                                  elapsed_ms, progress_context);
            }
            sceKernelDelayThread(VDEV_PROMOTE_POLL_MICROSECONDS);
            continue;
        }
        report->last_state = state;
        if (progress_callback != NULL) {
            progress_callback(state, elapsed_ms, progress_context);
        }
        if (state == 0) break;
        sceKernelDelayThread(VDEV_PROMOTE_POLL_MICROSECONDS);
    }

    for (;;) {
        const uint64_t now = sceKernelGetProcessTimeWide();
        const uint64_t elapsed_ms = (now - start_time) / UINT64_C(1000);

        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        result = scePromoterUtilityGetResult(&operation_result);
        if (result >= 0) break;
        if (progress_callback != NULL) {
            progress_callback(VDEV_PROMOTE_PROGRESS_RESULT_UNAVAILABLE,
                              elapsed_ms, progress_context);
        }
        sceKernelDelayThread(VDEV_PROMOTE_POLL_MICROSECONDS);
    }
    report->outcome_known = 1;
    report->operation_code = operation_result;
    if (operation_result == 0) {
        result = VDEV_OK;
    } else if (operation_result < 0) {
        result = operation_result;
    } else {
        result = VDEV_ERR_PROMOTE_RESULT;
    }

cleanup:
    if (promoter_initialized) {
        save_cleanup_error(report, scePromoterUtilityExit());
    }
    if (promoter_loaded) {
        save_cleanup_error(report, sceSysmoduleUnloadModuleInternal(
            SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL));
    }
    if (paf_loaded) {
        save_cleanup_error(report, unload_sce_paf());
    }

    if (report->operation_code == 0 && report->outcome_known) {
        return VDEV_OK;
    }
    if (report->dispatched && !report->outcome_known) {
        return VDEV_ERR_PROMOTE_UNKNOWN;
    }
    if (report->operation_code > 0) {
        return VDEV_ERR_PROMOTE_RESULT;
    }
    return report->operation_code;
}
