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
