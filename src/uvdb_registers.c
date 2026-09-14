#include "uvdb_registers.h"

static int user_bank_is_valid(
    const struct uvdb_arm_register_bank_state* bank)
{
    return bank &&
           (bank->cpsr & UVDB_ARM_CPSR_MODE_MASK) ==
               UVDB_ARM_CPSR_MODE_USER &&
           bank->pc != 0 && bank->sp != 0;
}

int uvdb_select_user_arm_register_bank(
    const struct uvdb_arm_register_bank_state
        banks[UVDB_ARM_REGISTER_BANK_COUNT])
{
    if(!banks)
        return -1;
    for(unsigned int index = 0; index < UVDB_ARM_REGISTER_BANK_COUNT;
        ++index)
        if(user_bank_is_valid(&banks[index]))
            return (int)index;
    return -1;
}
