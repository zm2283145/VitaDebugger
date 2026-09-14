.syntax unified
.thumb
.text
.global thumb2_control_flow_fixture
.type thumb2_control_flow_fixture, %function
.thumb_func
thumb2_control_flow_fixture:
    nop
    beq.w .Lconditional_target
    b.w .Lforward_target
.Lconditional_target:
    bl .Lcall_target
.Lforward_target:
    b.w thumb2_control_flow_fixture
.Lcall_target:
    bx lr
.size thumb2_control_flow_fixture, .-thumb2_control_flow_fixture
