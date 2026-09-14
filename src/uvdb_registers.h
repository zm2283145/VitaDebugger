#pragma once

#include <stdint.h>

#define UVDB_ARM_REGISTER_BANK_COUNT 2u
#define UVDB_ARM_CPSR_MODE_MASK 0x1fu
#define UVDB_ARM_CPSR_MODE_USER 0x10u

/*
 * The Vita kernel exposes two raw ARM register banks whose meaning depends on
 * where the suspended thread was executing.  Keep the selection policy
 * independent of the private kernel ABI so it can be tested on the host.
 */
struct uvdb_arm_register_bank_state {
    uint32_t sp;
    uint32_t pc;
    uint32_t cpsr;
};

/*
 * Return the current/resumable user-mode bank index. Entry 0 is the current
 * context and therefore wins when both banks look usable. Return -1 rather
 * than exposing a zero or privileged context when neither bank is valid.
 */
int uvdb_select_user_arm_register_bank(
    const struct uvdb_arm_register_bank_state
        banks[UVDB_ARM_REGISTER_BANK_COUNT]);
