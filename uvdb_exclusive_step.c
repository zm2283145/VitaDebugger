#include "uvdb_exclusive_step.h"

#include <limits.h>

enum exclusive_family
{
    EXCLUSIVE_NONE,
    EXCLUSIVE_WORD,
    EXCLUSIVE_BYTE,
    EXCLUSIVE_HALF,
    EXCLUSIVE_DOUBLE
};

enum exclusive_kind
{
    EXCLUSIVE_OTHER,
    EXCLUSIVE_LOAD,
    EXCLUSIVE_STORE,
    EXCLUSIVE_CLEAR
};

struct exclusive_instruction
{
    enum exclusive_kind kind;
    enum exclusive_family family;
    unsigned int base;
    unsigned int condition;
    int valid;
};

static int add_u32(uint32_t value, unsigned int increment, uint32_t* result)
{
    if(value > UINT32_MAX - increment)
        return 0;
    *result = value + increment;
    return 1;
}

static int condition_passed(unsigned int condition, uint32_t cpsr)
{
    int n = (int)((cpsr >> 31) & 1u);
    int z = (int)((cpsr >> 30) & 1u);
    int c = (int)((cpsr >> 29) & 1u);
    int v = (int)((cpsr >> 28) & 1u);
    int passed;

    if(condition >= 0xfu)
        return 0;
    switch(condition >> 1)
    {
        case 0: passed = z; break;
        case 1: passed = c; break;
        case 2: passed = n; break;
        case 3: passed = v; break;
        case 4: passed = c && !z; break;
        case 5: passed = n == v; break;
        case 6: passed = !z && n == v; break;
        default: passed = condition == 0xeu; break;
    }
    return (condition & 1u) ? !passed : passed;
}

static unsigned int thumb_itstate(uint32_t cpsr)
{
    return ((cpsr >> 8) & 0xfcu) | ((cpsr >> 25) & 3u);
}

static uint16_t load_le16(const unsigned char bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8));
}

static uint32_t load_le32(const unsigned char bytes[4])
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int arm_pc_writer(uint32_t instruction)
{
    unsigned int condition = instruction >> 28;

    if((instruction & UINT32_C(0xfe50ffff)) == UINT32_C(0xf8100a00) ||
       (condition != 0xfu &&
        (instruction & UINT32_C(0x0fffffff)) == UINT32_C(0x0160006e)))
        return 1;
    if((instruction & UINT32_C(0xfe000000)) == UINT32_C(0xfa000000) ||
       (condition != 0xfu &&
        (instruction & UINT32_C(0x0e000000)) == UINT32_C(0x0a000000)) ||
       (condition != 0xfu &&
        ((instruction & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff10) ||
         (instruction & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff20) ||
         (instruction & UINT32_C(0x0ffffff0)) == UINT32_C(0x012fff30))))
        return 1;
    if(condition != 0xfu &&
       (instruction & UINT32_C(0x0e100000)) == UINT32_C(0x08100000) &&
       (instruction & UINT32_C(0x00008000)))
        return 1;
    if(condition != 0xfu &&
       (instruction & UINT32_C(0x0c10f000)) == UINT32_C(0x0410f000))
        return 1;
    if(condition != 0xfu &&
       (instruction & UINT32_C(0x0000f000)) == UINT32_C(0x0000f000))
    {
        unsigned int opcode_class = (instruction >> 25) & 7u;
        if(opcode_class == 1u ||
           (opcode_class == 0u &&
            (!(instruction & UINT32_C(0x10)) ||
             !(instruction & UINT32_C(0x80)))))
            return 1;
    }
    return 0;
}

static int arm_may_block(uint32_t instruction)
{
    return (((instruction >> 28) != 0xfu) &&
            (instruction & UINT32_C(0x0f000000)) ==
                UINT32_C(0x0f000000)) ||
           (instruction & UINT32_C(0x0fffffff)) ==
                UINT32_C(0x0320f002) ||
           (instruction & UINT32_C(0x0fffffff)) ==
                UINT32_C(0x0320f003);
}

static struct exclusive_instruction decode_arm_exclusive(uint32_t instruction)
{
    struct exclusive_instruction decoded = {
        EXCLUSIVE_OTHER, EXCLUSIVE_NONE, 0, instruction >> 28, 0
    };
    uint32_t load_shape = instruction & UINT32_C(0x0ff00fff);
    uint32_t store_shape = instruction & UINT32_C(0x0ff00ff0);
    unsigned int rn = (instruction >> 16) & 0xfu;
    unsigned int rt = (instruction >> 12) & 0xfu;

    if(instruction == UINT32_C(0xf57ff01f))
    {
        decoded.kind = EXCLUSIVE_CLEAR;
        decoded.valid = 1;
        return decoded;
    }

    if(load_shape == UINT32_C(0x01900f9f) ||
       load_shape == UINT32_C(0x01d00f9f) ||
       load_shape == UINT32_C(0x01f00f9f) ||
       load_shape == UINT32_C(0x01b00f9f))
    {
        decoded.kind = EXCLUSIVE_LOAD;
        decoded.base = rn;
        if(load_shape == UINT32_C(0x01900f9f))
            decoded.family = EXCLUSIVE_WORD;
        else if(load_shape == UINT32_C(0x01d00f9f))
            decoded.family = EXCLUSIVE_BYTE;
        else if(load_shape == UINT32_C(0x01f00f9f))
            decoded.family = EXCLUSIVE_HALF;
        else
            decoded.family = EXCLUSIVE_DOUBLE;
        decoded.valid = decoded.condition != 0xfu && rn != 15u &&
                        rt != 15u &&
                        (decoded.family != EXCLUSIVE_DOUBLE ||
                         ((rt & 1u) == 0 && rt != 14u));
        return decoded;
    }

    if(store_shape == UINT32_C(0x01800f90) ||
       store_shape == UINT32_C(0x01c00f90) ||
       store_shape == UINT32_C(0x01e00f90) ||
       store_shape == UINT32_C(0x01a00f90))
    {
        unsigned int rd = rt;
        unsigned int data = instruction & 0xfu;
        decoded.kind = EXCLUSIVE_STORE;
        decoded.base = rn;
        if(store_shape == UINT32_C(0x01800f90))
            decoded.family = EXCLUSIVE_WORD;
        else if(store_shape == UINT32_C(0x01c00f90))
            decoded.family = EXCLUSIVE_BYTE;
        else if(store_shape == UINT32_C(0x01e00f90))
            decoded.family = EXCLUSIVE_HALF;
        else
            decoded.family = EXCLUSIVE_DOUBLE;
        decoded.valid = decoded.condition != 0xfu && rn != 15u &&
                        rd != 15u && data != 15u && rd != rn &&
                        rd != data;
        if(decoded.family == EXCLUSIVE_DOUBLE)
            decoded.valid = decoded.valid && (data & 1u) == 0 &&
                            data != 14u && rd != data + 1u;
        return decoded;
    }
    return decoded;
}

static int thumb16_pc_writer(uint16_t instruction)
{
    if(((instruction & UINT16_C(0xf000)) == UINT16_C(0xd000) &&
        (instruction & UINT16_C(0x0f00)) < UINT16_C(0x0e00)) ||
       (instruction & UINT16_C(0xf800)) == UINT16_C(0xe000) ||
       (instruction & UINT16_C(0xff07)) == UINT16_C(0x4700) ||
       (instruction & UINT16_C(0xff00)) == UINT16_C(0xbd00) ||
       (instruction & UINT16_C(0xf500)) == UINT16_C(0xb100))
        return 1;
    if((instruction & UINT16_C(0xff00)) == UINT16_C(0x4400) ||
       (instruction & UINT16_C(0xff00)) == UINT16_C(0x4600))
    {
        unsigned int rd = (instruction & 7u) |
                          ((instruction >> 4) & 8u);
        return rd == 15u;
    }
    return 0;
}

static int thumb32_pc_writer(uint16_t first, uint16_t second)
{
    if((first & UINT16_C(0xf800)) == UINT16_C(0xf000) &&
       (second & UINT16_C(0x8000)) &&
       ((second & UINT16_C(0x1000)) != 0 ||
        (second & UINT16_C(0xd001)) == UINT16_C(0xc000) ||
        ((second & UINT16_C(0xd000)) == UINT16_C(0x8000) &&
         (first & UINT16_C(0x0380)) != UINT16_C(0x0380))))
        return 1;
    if((first & UINT16_C(0xfff0)) == UINT16_C(0xe8d0) &&
       (second & UINT16_C(0xffe0)) == UINT16_C(0xf000))
        return 1;
    if((first & UINT16_C(0xfe50)) == UINT16_C(0xe810))
    {
        int increment = (first & UINT16_C(0x0080)) != 0;
        int before = (first & UINT16_C(0x0100)) != 0;
        if(increment == before || (second & UINT16_C(0x8000)))
            return 1;
    }
    if((first & UINT16_C(0xff70)) == UINT16_C(0xf850) &&
       (second & UINT16_C(0xf000)) == UINT16_C(0xf000))
        return 1;
    if((first & UINT16_C(0xffef)) == UINT16_C(0xea4f) &&
       (second & UINT16_C(0xfff0)) == UINT16_C(0x0f00))
        return 1;
    if((first & UINT16_C(0xfff0)) == UINT16_C(0xf3c0) &&
       second == UINT16_C(0x8f00))
        return 1;
    if(first == UINT16_C(0xf3de) &&
       (second & UINT16_C(0xff00)) == UINT16_C(0x8f00))
        return 1;
    return 0;
}

static int thumb_may_block(uint16_t first, uint16_t second)
{
    return (first & UINT16_C(0xff00)) == UINT16_C(0xdf00) ||
           first == UINT16_C(0xbf20) || first == UINT16_C(0xbf30) ||
           (first == UINT16_C(0xf3af) &&
            (second == UINT16_C(0x8002) ||
             second == UINT16_C(0x8003)));
}

static struct exclusive_instruction decode_thumb_exclusive(
    uint16_t first,
    uint16_t second,
    unsigned int size)
{
    struct exclusive_instruction decoded = {
        EXCLUSIVE_OTHER, EXCLUSIVE_NONE, 0, 0xeu, 0
    };
    unsigned int rn = first & 0xfu;
    unsigned int rt = second >> 12;

    if(size != 4u)
        return decoded;
    if(first == UINT16_C(0xf3bf) && second == UINT16_C(0x8f2f))
    {
        decoded.kind = EXCLUSIVE_CLEAR;
        decoded.valid = 1;
        return decoded;
    }
    if((first & UINT16_C(0xfff0)) == UINT16_C(0xe850) &&
       (second & UINT16_C(0x0f00)) == UINT16_C(0x0f00))
    {
        decoded.kind = EXCLUSIVE_LOAD;
        decoded.family = EXCLUSIVE_WORD;
        decoded.base = rn;
        decoded.valid = rn != 15u && rt != 13u && rt != 15u;
        return decoded;
    }
    if((first & UINT16_C(0xfff0)) == UINT16_C(0xe8d0))
    {
        decoded.kind = EXCLUSIVE_LOAD;
        decoded.base = rn;
        if((second & UINT16_C(0x0fff)) == UINT16_C(0x0f4f))
            decoded.family = EXCLUSIVE_BYTE;
        else if((second & UINT16_C(0x0fff)) == UINT16_C(0x0f5f))
            decoded.family = EXCLUSIVE_HALF;
        else if((second & UINT16_C(0x00ff)) == UINT16_C(0x007f))
            decoded.family = EXCLUSIVE_DOUBLE;
        else
        {
            decoded.kind = EXCLUSIVE_OTHER;
            return decoded;
        }
        decoded.valid = rn != 15u && rt != 13u && rt != 15u;
        if(decoded.family == EXCLUSIVE_DOUBLE)
        {
            unsigned int rt2 = (second >> 8) & 0xfu;
            decoded.valid = decoded.valid && rt != 13u &&
                            rt2 != 13u && rt2 != 15u && rt != rt2;
        }
        return decoded;
    }
    if((first & UINT16_C(0xfff0)) == UINT16_C(0xe840))
    {
        unsigned int rd = (second >> 8) & 0xfu;
        decoded.kind = EXCLUSIVE_STORE;
        decoded.family = EXCLUSIVE_WORD;
        decoded.base = rn;
        decoded.valid = rn != 15u && rt != 13u && rt != 15u &&
                        rd != 13u && rd != 15u &&
                        rd != rn && rd != rt;
        return decoded;
    }
    if((first & UINT16_C(0xfff0)) == UINT16_C(0xe8c0))
    {
        unsigned int rd = second & 0xfu;
        decoded.kind = EXCLUSIVE_STORE;
        decoded.base = rn;
        if((second & UINT16_C(0x0ff0)) == UINT16_C(0x0f40))
            decoded.family = EXCLUSIVE_BYTE;
        else if((second & UINT16_C(0x0ff0)) == UINT16_C(0x0f50))
            decoded.family = EXCLUSIVE_HALF;
        else if((second & UINT16_C(0x00f0)) == UINT16_C(0x0070))
            decoded.family = EXCLUSIVE_DOUBLE;
        else
        {
            decoded.kind = EXCLUSIVE_OTHER;
            return decoded;
        }
        decoded.valid = rn != 15u && rt != 13u && rt != 15u && rd != 13u &&
                        rd != 15u && rd != rn && rd != rt;
        if(decoded.family == EXCLUSIVE_DOUBLE)
        {
            unsigned int rt2 = (second >> 8) & 0xfu;
            decoded.valid = decoded.valid && rt != 13u &&
                            rt2 != 13u && rt2 != 15u && rt != rt2 &&
                            rd != rt2;
        }
        return decoded;
    }
    return decoded;
}

static int limits_valid(const struct uvdb_exclusive_step_limits* limits)
{
    return limits && limits->instruction_limit != 0u &&
           limits->instruction_limit <=
               UVDB_EXCLUSIVE_STEP_MAX_INSTRUCTIONS &&
           limits->byte_limit != 0u &&
           limits->byte_limit <= UVDB_EXCLUSIVE_STEP_MAX_BYTES;
}

static int set_target(
    struct uvdb_exclusive_step_target* target,
    uint32_t address,
    unsigned int breakpoint_size,
    unsigned int instructions,
    unsigned int bytes)
{
    struct uvdb_exclusive_step_target planned;
    planned.address = address;
    planned.breakpoint_size = breakpoint_size;
    planned.scanned_instructions = instructions;
    planned.scanned_bytes = bytes;
    *target = planned;
    return 1;
}

static int scan_arm(
    uvdb_exclusive_step_read_fn read_memory,
    void* read_context,
    uint32_t pc,
    uint32_t cpsr,
    const struct uvdb_exclusive_step_limits* limits,
    struct uvdb_exclusive_step_target* target)
{
    enum exclusive_family family = EXCLUSIVE_NONE;
    unsigned int base = 0;
    unsigned int instructions = 0;
    unsigned int bytes = 0;

    while(instructions < limits->instruction_limit)
    {
        unsigned char encoded[4];
        uint32_t next;
        struct exclusive_instruction decoded;

        if(limits->byte_limit - bytes < 4u ||
           read_memory(read_context, pc, encoded, sizeof(encoded)) != 4)
            return 0;
        decoded = decode_arm_exclusive(load_le32(encoded));
        instructions++;
        bytes += 4u;

        if(instructions == 1u)
        {
            if(decoded.kind != EXCLUSIVE_LOAD || !decoded.valid ||
               !condition_passed(decoded.condition, cpsr))
                return 0;
            family = decoded.family;
            base = decoded.base;
        }
        else if(decoded.kind == EXCLUSIVE_LOAD)
            return 0;
        else if(decoded.kind == EXCLUSIVE_STORE)
        {
            /* A later conditional STREX is unsafe because intervening
             * instructions may have changed APSR flags. */
            if(!decoded.valid || decoded.condition != 0xeu ||
               decoded.family != family || decoded.base != base ||
               !add_u32(pc, 4u, &next))
                return 0;
            return set_target(target, next, 4u, instructions, bytes);
        }
        else if(decoded.kind == EXCLUSIVE_CLEAR)
        {
            if(!add_u32(pc, 4u, &next))
                return 0;
            return set_target(target, next, 4u, instructions, bytes);
        }
        else
        {
            uint32_t instruction = load_le32(encoded);
            if(arm_pc_writer(instruction) || arm_may_block(instruction))
                return 0;
        }

        if(!add_u32(pc, 4u, &pc))
            return 0;
    }
    return 0;
}

static int scan_thumb(
    uvdb_exclusive_step_read_fn read_memory,
    void* read_context,
    uint32_t pc,
    const struct uvdb_exclusive_step_limits* limits,
    struct uvdb_exclusive_step_target* target)
{
    enum exclusive_family family = EXCLUSIVE_NONE;
    unsigned int base = 0;
    unsigned int instructions = 0;
    unsigned int bytes = 0;

    while(instructions < limits->instruction_limit)
    {
        unsigned char encoded[2];
        uint16_t first;
        uint16_t second = 0;
        unsigned int size;
        uint32_t next;
        struct exclusive_instruction decoded;

        if(limits->byte_limit - bytes < 2u ||
           read_memory(read_context, pc, encoded, sizeof(encoded)) != 2)
            return 0;
        first = load_le16(encoded);
        size = ((first >> 11) == 0x1du || (first >> 11) == 0x1eu ||
                (first >> 11) == 0x1fu) ? 4u : 2u;
        if(size == 4u)
        {
            if(limits->byte_limit - bytes < 4u ||
               !add_u32(pc, 2u, &next) ||
               read_memory(read_context, next, encoded, sizeof(encoded)) != 2)
                return 0;
            second = load_le16(encoded);
        }
        instructions++;
        bytes += size;
        decoded = decode_thumb_exclusive(first, second, size);

        if(instructions == 1u)
        {
            if(decoded.kind != EXCLUSIVE_LOAD || !decoded.valid)
                return 0;
            family = decoded.family;
            base = decoded.base;
        }
        else if(decoded.kind == EXCLUSIVE_LOAD)
            return 0;
        else if(decoded.kind == EXCLUSIVE_STORE)
        {
            if(!decoded.valid || decoded.family != family ||
               decoded.base != base || !add_u32(pc, size, &next))
                return 0;
            return set_target(target, next, 2u, instructions, bytes);
        }
        else if(decoded.kind == EXCLUSIVE_CLEAR)
        {
            if(!add_u32(pc, size, &next))
                return 0;
            return set_target(target, next, 2u, instructions, bytes);
        }
        else if((first & UINT16_C(0xff00)) == UINT16_C(0xbf00) &&
                (first & UINT16_C(0x000f)) != 0)
            return 0;
        else if(thumb_may_block(first, second) ||
                (size == 2u ? thumb16_pc_writer(first)
                            : thumb32_pc_writer(first, second)))
            return 0;

        if(!add_u32(pc, size, &pc))
            return 0;
    }
    return 0;
}

int uvdb_exclusive_step_scan(
    uvdb_exclusive_step_read_fn read_memory,
    void* read_context,
    uint32_t pc,
    uint32_t cpsr,
    const struct uvdb_exclusive_step_limits* limits,
    struct uvdb_exclusive_step_target* target)
{
    int thumb;

    if(!read_memory || !target || !limits_valid(limits))
        return 0;
    if(cpsr & ((UINT32_C(1) << 24) | (UINT32_C(1) << 9)))
        return 0;
    thumb = (cpsr & (UINT32_C(1) << 5)) != 0;
    if((thumb && ((pc & 1u) || thumb_itstate(cpsr) != 0)) ||
       (!thumb && (pc & 3u)))
        return 0;
    return thumb
        ? scan_thumb(read_memory, read_context, pc, limits, target)
        : scan_arm(read_memory, read_context, pc, cpsr, limits, target);
}
