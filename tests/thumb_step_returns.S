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
