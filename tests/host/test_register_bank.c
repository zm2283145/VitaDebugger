#include <stdio.h>

#include "uvdb_registers.h"

static int failures;

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

int main(void)
{
    struct uvdb_arm_register_bank_state
        banks[UVDB_ARM_REGISTER_BANK_COUNT] = {0};

    check(uvdb_select_user_arm_register_bank(NULL) < 0,
          "NULL banks rejected");
    check(uvdb_select_user_arm_register_bank(banks) < 0,
          "zero banks rejected");

    banks[0] = (struct uvdb_arm_register_bank_state) {
        .sp = 0x81130fc0u,
        .pc = 0x8105891cu,
        .cpsr = 0x60010030u,
    };
    check(uvdb_select_user_arm_register_bank(banks) == 0,
          "runnable user context selects entry 0");

    banks[0].cpsr = 0x60000113u;
    banks[1] = (struct uvdb_arm_register_bank_state) {
        .sp = 0x81168fc8u,
        .pc = 0x81078bf8u,
        .cpsr = 0x20010010u,
    };
    check(uvdb_select_user_arm_register_bank(banks) == 1,
          "syscall return context selects entry 1");

    banks[0] = (struct uvdb_arm_register_bank_state) {
        .sp = 0x81130fc0u,
        .pc = 0x8105891cu,
        .cpsr = 0x60010030u,
    };
    check(uvdb_select_user_arm_register_bank(banks) == 0,
          "entry 0 wins when both banks are user contexts");

    banks[0].pc = 0;
    check(uvdb_select_user_arm_register_bank(banks) == 1,
          "zero entry 0 PC falls back to valid entry 1");
    banks[0].pc = 0x8105891cu;
    banks[0].sp = 0;
    check(uvdb_select_user_arm_register_bank(banks) == 1,
          "zero entry 0 SP falls back to valid entry 1");

    banks[0].sp = 0x81130fc0u;
    banks[0].cpsr = 0x6000011fu;
    banks[1].cpsr = 0x20010013u;
    check(uvdb_select_user_arm_register_bank(banks) < 0,
          "privileged banks rejected");

    banks[0].cpsr = 0x60010030u;
    banks[0].pc = 0;
    banks[1].cpsr = 0x20010010u;
    banks[1].sp = 0;
    check(uvdb_select_user_arm_register_bank(banks) < 0,
          "user-mode banks with invalid PC or SP rejected");

    if(failures)
        return 1;
    puts("PASS: ARM raw register-bank selection");
    return 0;
}
