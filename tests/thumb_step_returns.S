.syntax unified
.thumb
.text

.global thumb_step_pop_fixture
.global thumb_step_pop_site
.type thumb_step_pop_fixture, %function
.thumb_func
thumb_step_pop_fixture:
    push {r4, lr}
    movs r4, #7
thumb_step_pop_site:
    pop {r4, pc}
.size thumb_step_pop_fixture, .-thumb_step_pop_fixture

.global thumb_step_mov_fixture
.global thumb_step_mov_site
.global thumb_step_mov_target
.type thumb_step_mov_fixture, %function
.thumb_func
thumb_step_mov_fixture:
    ldr r0, .Lmov_target_address
thumb_step_mov_site:
    mov pc, r0
    udf #1
thumb_step_mov_target:
    bx lr
    .align 2
.Lmov_target_address:
    .word thumb_step_mov_target + 1
.size thumb_step_mov_fixture, .-thumb_step_mov_fixture

.global thumb_step_tbb_fixture
.global thumb_step_tbb_site
.global thumb_step_tbb_target
.type thumb_step_tbb_fixture, %function
.thumb_func
thumb_step_tbb_fixture:
    ldr r0, .Ltbb_table_address
    movs r1, #0
    .balign 4
thumb_step_tbb_site:
    tbb [r0, r1]
    udf #2
thumb_step_tbb_target:
    bx lr
    .balign 4
.Ltbb_table:
    .byte (thumb_step_tbb_target - (thumb_step_tbb_site + 4)) / 2
    .balign 4
.Ltbb_table_address:
    .word .Ltbb_table
.size thumb_step_tbb_fixture, .-thumb_step_tbb_fixture

.global thumb_step_tbh_fixture
.global thumb_step_tbh_site
.global thumb_step_tbh_target
.type thumb_step_tbh_fixture, %function
.thumb_func
thumb_step_tbh_fixture:
    ldr r0, .Ltbh_table_address
    movs r1, #0
    .balign 4
thumb_step_tbh_site:
    tbh [r0, r1, lsl #1]
    udf #3
thumb_step_tbh_target:
    bx lr
    .balign 4
.Ltbh_table:
    .hword (thumb_step_tbh_target - (thumb_step_tbh_site + 4)) / 2
    .balign 4
.Ltbh_table_address:
    .word .Ltbh_table
.size thumb_step_tbh_fixture, .-thumb_step_tbh_fixture

.global thumb_step_ldm_fixture
.global thumb_step_ldm_site
.global thumb_step_ldm_target
.type thumb_step_ldm_fixture, %function
.thumb_func
thumb_step_ldm_fixture:
    ldr r0, .Lldm_table_address
thumb_step_ldm_site:
    ldmia.w r0!, {r4, pc}
    udf #4
thumb_step_ldm_target:
    bx lr
    .balign 4
.Lldm_table:
    .word 0x44556677
    .word thumb_step_ldm_target + 1
.Lldm_table_address:
    .word .Lldm_table
.size thumb_step_ldm_fixture, .-thumb_step_ldm_fixture

.global thumb_step_it_fixture
.global thumb_step_it_site
.global thumb_step_it_target
.type thumb_step_it_fixture, %function
.thumb_func
thumb_step_it_fixture:
    movs r0, #0
    cmp r0, #1
thumb_step_it_site:
    itt eq
    moveq r2, #1
    moveq r2, #2
thumb_step_it_target:
    bx lr
.size thumb_step_it_fixture, .-thumb_step_it_fixture

.global thumb_step_ldmdb_fixture
.global thumb_step_ldmdb_site
.global thumb_step_ldmdb_target
.type thumb_step_ldmdb_fixture, %function
.thumb_func
thumb_step_ldmdb_fixture:
    ldr r0, .Lldmdb_end_address
thumb_step_ldmdb_site:
    ldmdb r0!, {r4, pc}
    udf #5
thumb_step_ldmdb_target:
    bx lr
    .balign 4
.Lldmdb_table:
    .word 0x8899aabb
    .word thumb_step_ldmdb_target + 1
.Lldmdb_end:
.Lldmdb_end_address:
    .word .Lldmdb_end
.size thumb_step_ldmdb_fixture, .-thumb_step_ldmdb_fixture

.global thumb_step_ldr_pc_fixture
.global thumb_step_ldr_pc_site
.global thumb_step_ldr_pc_target
.type thumb_step_ldr_pc_fixture, %function
.thumb_func
thumb_step_ldr_pc_fixture:
    ldr r0, .Lthumb_ldr_table_address
thumb_step_ldr_pc_site:
    ldr.w pc, [r0]
    udf #8
thumb_step_ldr_pc_target:
    bx lr
    .balign 4
.Lthumb_ldr_table:
    .word thumb_step_ldr_pc_target + 1
.Lthumb_ldr_table_address:
    .word .Lthumb_ldr_table
.size thumb_step_ldr_pc_fixture, .-thumb_step_ldr_pc_fixture

.arm
.balign 4
.global arm_step_mov_fixture
.global arm_step_mov_site
.global arm_step_mov_target
.type arm_step_mov_fixture, %function
arm_step_mov_fixture:
    ldr r0, .Larm_mov_target_address
arm_step_mov_site:
    mov pc, r0
    udf #6
arm_step_mov_target:
    bx lr
.Larm_mov_target_address:
    .word arm_step_mov_target
.size arm_step_mov_fixture, .-arm_step_mov_fixture

.global arm_step_ldm_fixture
.global arm_step_ldm_site
.global arm_step_ldm_target
.type arm_step_ldm_fixture, %function
arm_step_ldm_fixture:
    ldr r0, .Larm_ldm_end_address
arm_step_ldm_site:
    ldmdb r0!, {r4, pc}
    udf #7
arm_step_ldm_target:
    bx lr
.Larm_ldm_table:
    .word 0xccddeeff
    .word arm_step_ldm_target
.Larm_ldm_end:
.Larm_ldm_end_address:
    .word .Larm_ldm_end
.size arm_step_ldm_fixture, .-arm_step_ldm_fixture

.global arm_step_ldr_pc_fixture
.global arm_step_ldr_pc_site
.global arm_step_ldr_pc_target
.type arm_step_ldr_pc_fixture, %function
arm_step_ldr_pc_fixture:
    ldr r0, .Larm_ldr_table_address
arm_step_ldr_pc_site:
    ldr pc, [r0]
    udf #9
arm_step_ldr_pc_target:
    bx lr
.Larm_ldr_table:
    .word arm_step_ldr_pc_target
.Larm_ldr_table_address:
    .word .Larm_ldr_table
.size arm_step_ldr_pc_fixture, .-arm_step_ldr_pc_fixture

/* A software trap immediately after LDREX would clear the local exclusive
 * monitor and perturb STREX. These two fixtures let the debugger validate its
 * bounded straight-line LDREX..STREX sequence step in both execution states.
 * The label immediately after STREX is the expected temporary-trap stop. */

.thumb
.balign 4
.global thumb_step_exclusive_fixture
.global thumb_step_exclusive_site
.global thumb_step_exclusive_store
.global thumb_step_exclusive_after
.type thumb_step_exclusive_fixture, %function
.thumb_func
thumb_step_exclusive_fixture:
    push {r4, lr}
    ldr r0, .Lthumb_exclusive_word_address
.Lthumb_exclusive_retry:
thumb_step_exclusive_site:
    ldrex r1, [r0]
    adds r1, #1
thumb_step_exclusive_store:
    strex r2, r1, [r0]
thumb_step_exclusive_after:
    cmp r2, #0
    bne .Lthumb_exclusive_retry
    pop {r4, pc}
    .balign 4
.Lthumb_exclusive_word_address:
    .word uvdb_thumb_exclusive_word
.size thumb_step_exclusive_fixture, .-thumb_step_exclusive_fixture

.arm
.balign 4
.global arm_step_exclusive_fixture
.global arm_step_exclusive_site
.global arm_step_exclusive_store
.global arm_step_exclusive_after
.type arm_step_exclusive_fixture, %function
arm_step_exclusive_fixture:
    push {r4, lr}
    ldr r0, .Larm_exclusive_word_address
.Larm_exclusive_retry:
arm_step_exclusive_site:
    ldrex r1, [r0]
    add r1, r1, #1
arm_step_exclusive_store:
    strex r2, r1, [r0]
arm_step_exclusive_after:
    cmp r2, #0
    bne .Larm_exclusive_retry
    pop {r4, pc}
.Larm_exclusive_word_address:
    .word uvdb_arm_exclusive_word
.size arm_step_exclusive_fixture, .-arm_step_exclusive_fixture

.thumb
