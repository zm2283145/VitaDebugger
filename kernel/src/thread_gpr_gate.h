#pragma once

#include "thread_mutation_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VD_THREAD_GPR_GATE_COMPILED
#define VD_THREAD_GPR_GATE_COMPILED 0
#endif

#define VD_THREAD_GPR_GATE_REGISTER_R4 4u
#define VD_THREAD_GPR_GATE_REGISTER_R5 5u

struct vd_thread_gpr_gate {
    struct vd_thread_mutation_session transaction;
    struct vd_kernel_thread_mutation_handle handle;
    struct vd_thread_mutation_provider* provider;
    unsigned int register_index;
    unsigned int register_bank;
    unsigned int initialized;
};

int vdThreadGprGateInit(struct vd_thread_gpr_gate* gate);

int vdThreadGprGateCompiled(void);

int vdThreadGprGateBegin(
    struct vd_thread_gpr_gate* gate,
    const struct vd_thread_mutation_identity* identity,
    unsigned int register_index,
    struct vd_thread_mutation_provider* provider);

int vdThreadGprGateApply(
    struct vd_thread_gpr_gate* gate,
    const struct vd_thread_mutation_identity* identity,
    unsigned int value);

int vdThreadGprGateRestore(
    struct vd_thread_gpr_gate* gate,
    const struct vd_thread_mutation_identity* identity);

int vdThreadGprGateCleanup(
    struct vd_thread_gpr_gate* gate,
    SceUID owner_pid,
    unsigned int stop_token);

int vdThreadGprGateIsActive(const struct vd_thread_gpr_gate* gate);

#ifdef __cplusplus
}
#endif
