#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/signal.h> //for signal constants; these seem to match gdb's
#include <stdarg.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <psp2/net/net_syscalls.h>
#include <psp2/kernel/threadmgr/msgpipe.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <kubridge.h>
#include "uvdb.h"

#define UVDB_DEFAULT_PORT 1234
#define UVDB_DEFAULT_MAX_BUFFER (256 * 1024)
#define UVDB_MIN_BUFFER 4096
#define UVDB_MAX_BUFFER (16 * 1024 * 1024)

//we prefer to use raw syscalls to avoid issues with signal safety
void _sceKernelExitProcessForUser(int);
int _sceKernelSendMsgPipeVector(SceUID, const SceKernelAddrPair*, unsigned int, uint32_t* rest);
int _sceKernelReceiveMsgPipeVector(SceUID, const SceKernelAddrPair*, unsigned int, uint32_t* rest);
extern char __executable_start[];
extern char __init_array_start[];

#define UVDB_MAX_THREADS 32
#define UVDB_THREAD_NAME_MAX 32

struct uvdb_thread_entry
{
    SceUID id;
    char name[UVDB_THREAD_NAME_MAX];
    uint8_t active;
};

static struct uvdb_thread_entry uvdb_threads[UVDB_MAX_THREADS];
static volatile SceUID uvdb_stopped_thread = -1;
static SceUID uvdb_general_thread = -1;
static SceUID uvdb_continue_thread = -1;

static int uvdb_lock_state;

static void uvdb_lock(void)
{
    for(;;)
    {
        int old_value = 0;
        if(__atomic_compare_exchange_n(&uvdb_lock_state, &old_value, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
    }
}

static void uvdb_unlock(void)
{
    __atomic_store_n(&uvdb_lock_state, 0, __ATOMIC_SEQ_CST);
}

static struct uvdb_thread_entry* uvdb_find_thread(SceUID id)
{
    for(size_t i = 0; i < UVDB_MAX_THREADS; ++i)
        if(uvdb_threads[i].active && uvdb_threads[i].id == id)
            return &uvdb_threads[i];
    return NULL;
}

int uvdb_register_thread(const char* name)
{
    SceUID id = sceKernelGetThreadId();
    uvdb_lock();
    struct uvdb_thread_entry* entry = uvdb_find_thread(id);
    if(!entry)
    {
        for(size_t i = 0; i < UVDB_MAX_THREADS; ++i)
            if(!uvdb_threads[i].active)
            {
                entry = &uvdb_threads[i];
                memset(entry, 0, sizeof(*entry));
                entry->id = id;
                entry->active = 1;
                break;
            }
    }
    if(entry && name)
    {
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = 0;
    }
    uvdb_unlock();
    return entry ? 0 : -1;
}

int uvdb_unregister_thread(void)
{
    SceUID id = sceKernelGetThreadId();
    uvdb_lock();
    struct uvdb_thread_entry* entry = uvdb_find_thread(id);
    if(entry)
        memset(entry, 0, sizeof(*entry));
    uvdb_unlock();
    return entry ? 0 : -1;
}

static int uvdb_socket = -1;
static int uvdb_listen_socket = -1;
static SceUID uvdb_pipe = -1;
static size_t uvdb_max_buffer = UVDB_DEFAULT_MAX_BUFFER;
static unsigned short uvdb_port = UVDB_DEFAULT_PORT;
static volatile enum uvdb_state uvdb_state = UVDB_STATE_IDLE;
static int uvdb_io_failed;
static unsigned int uvdb_handler_mask;
static volatile int uvdb_target_stopped;
static volatile int uvdb_async_stop_pending;
static volatile int uvdb_server_stop;
static SceUID uvdb_server_thread = -1;
static struct uvdb_fault_info uvdb_last_fault = {
    .exception_type = UVDB_EXCEPTION_NONE,
};

static void breakpoint_remove_all(void);
static int uvdb_server_main(SceSize args, void* argp);

struct buffer
{
    SceUID memblock_uid;
    char* buf;
    size_t size;
    size_t cap;
    size_t packet_start;
};

static void buffer_popleft(struct buffer* buf, size_t cnt)
{
    if(cnt > buf->size)
        cnt = buf->size;
    memmove(buf->buf, buf->buf+cnt, buf->size-cnt);
    buf->size -= cnt;
}

static size_t buffer_getspace(struct buffer* buf, char** pos)
{
    if(buf->size == buf->cap)
    {
        size_t cap2 = buf->cap * 2;
        if(!cap2)
            cap2 = UVDB_MIN_BUFFER;
        if(cap2 > uvdb_max_buffer)
            cap2 = uvdb_max_buffer;
        if(cap2 <= buf->cap)
        {
            *pos = NULL;
            return 0;
        }
        SceUID memblock2 = sceKernelAllocMemBlock("gdb socket buffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, cap2, NULL);
        if(memblock2 < 0)
        {
            *pos = NULL;
            return 0;
        }
        void* base = NULL;
        if(sceKernelGetMemBlockBase(memblock2, &base) < 0 || !base)
        {
            sceKernelFreeMemBlock(memblock2);
            *pos = NULL;
            return 0;
        }
        if(buf->size)
            memcpy(base, buf->buf, buf->size);
        if(buf->memblock_uid >= 0)
            sceKernelFreeMemBlock(buf->memblock_uid);
        buf->memblock_uid = memblock2;
        buf->buf = base;
        buf->cap = cap2;
    }
    *pos = buf->buf + buf->size;
    return buf->cap - buf->size;
}

static size_t buffer_poll(struct buffer* buf, char** pos)
{
    size_t chk_size = buffer_getspace(buf, pos);
    if(!chk_size)
    {
        uvdb_io_failed = 1;
        return 0;
    }
    uint32_t args[6] = {uvdb_socket, (uint32_t)*pos, chk_size, 0, 0, 0};
    ssize_t ans = sceNetSyscallRecvfrom((void*)args);
    if(ans <= 0)
    {
        uvdb_io_failed = 1;
        return 0;
    }
    buf->size += ans;
    return ans;
}

static void buffer_write(struct buffer* buf, const void* source, size_t sz)
{
    const char* data = source;
    while(sz)
    {
        char* pos;
        size_t chk = buffer_getspace(buf, &pos);
        if(!chk)
        {
            uvdb_io_failed = 1;
            return;
        }
        if(chk > sz)
            chk = sz;
        memcpy(pos, data, chk);
        data += chk;
        buf->size += chk;
        sz -= chk;
    }
}

static void buffer_start_packet(struct buffer* buf)
{
    buffer_write(buf, "$", 1);
    buf->packet_start = buf->size;
}

static char int2hex(int value)
{
    if(value < 10)
        return value + '0';
    return value - 10 + 'a';
}

static void buffer_end_packet(struct buffer* buf)
{
    uint8_t cksum = 0;
    for(size_t i = buf->packet_start; i < buf->size; i++)
        cksum += (uint8_t)buf->buf[i];
    uint8_t footer[3] = {'#', int2hex(cksum>>4), int2hex(cksum&15)};
    buffer_write(buf, footer, 3);
}

static void buffer_flush(struct buffer* buf)
{
    size_t pos = 0;
    while(pos < buf->size)
    {
        uint32_t args[6] = {uvdb_socket, (uint32_t)(buf->buf+pos), buf->size-pos, 0, 0, 0};
        ssize_t chk = sceNetSyscallSendto((void*)args);
        if(chk <= 0)
        {
            uvdb_io_failed = 1;
            break;
        }
        pos += chk;
    }
    buf->size = 0;
}

static struct buffer in_buf = {.memblock_uid = -1};
static struct buffer out_buf = {.memblock_uid = -1};

static void buffer_release(struct buffer* buf)
{
    if(buf->memblock_uid >= 0)
        sceKernelFreeMemBlock(buf->memblock_uid);
    memset(buf, 0, sizeof(*buf));
    buf->memblock_uid = -1;
}

static void uvdb_close_socket(int* socket)
{
    if(*socket >= 0)
    {
        sceNetSyscallShutdown(*socket, SHUT_RDWR);
        sceNetSyscallClose(*socket);
        *socket = -1;
    }
}

static void uvdb_release_handlers(void)
{
    if(uvdb_handler_mask & (1u << KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT))
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT);
    if(uvdb_handler_mask & (1u << KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT))
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT);
    if(uvdb_handler_mask & (1u << KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION))
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION);
    uvdb_handler_mask = 0;
}

int uvdb_configure(const struct uvdb_config* config)
{
    if(uvdb_state != UVDB_STATE_IDLE || uvdb_socket >= 0 || uvdb_listen_socket >= 0)
        return -1;

    unsigned short port = UVDB_DEFAULT_PORT;
    size_t max_buffer = UVDB_DEFAULT_MAX_BUFFER;
    if(config)
    {
        port = config->port;
        max_buffer = config->max_packet_buffer;
        if(!port || max_buffer < UVDB_MIN_BUFFER || max_buffer > UVDB_MAX_BUFFER)
            return -1;
    }
    uvdb_port = port;
    uvdb_max_buffer = max_buffer;
    return 0;
}

enum uvdb_state uvdb_get_state(void)
{
    return uvdb_state;
}

int uvdb_get_last_fault(struct uvdb_fault_info* info)
{
    if(!info)
        return -1;
    *info = uvdb_last_fault;
    return uvdb_last_fault.exception_type == UVDB_EXCEPTION_NONE ? 0 : 1;
}

int uvdb_stop_server(void)
{
    SceUID thread = uvdb_server_thread;
    if(thread < 0)
        return 0;
    if(thread == sceKernelGetThreadId())
        return -1;

    __atomic_store_n(&uvdb_server_stop, 1, __ATOMIC_SEQ_CST);
    // Closing a socket is what wakes a service thread blocked in accept/recv.
    // Do not take uvdb_lock here: the blocked service or exception path may be
    // holding it while waiting for network input.
    uvdb_close_socket(&uvdb_socket);
    uvdb_close_socket(&uvdb_listen_socket);

    int status = 0;
    int result = sceKernelWaitThreadEnd(thread, &status, NULL);
    if(result >= 0)
        result = sceKernelDeleteThread(thread);
    uvdb_server_thread = -1;
    return result < 0 ? -1 : 0;
}

void uvdb_shutdown(void)
{
    uvdb_stop_server();
    uvdb_lock();
    breakpoint_remove_all();
    uvdb_close_socket(&uvdb_socket);
    uvdb_close_socket(&uvdb_listen_socket);
    uvdb_release_handlers();
    if(uvdb_pipe >= 0)
    {
        sceKernelDeleteMsgPipe(uvdb_pipe);
        uvdb_pipe = -1;
    }
    buffer_release(&in_buf);
    buffer_release(&out_buf);
    memset(uvdb_threads, 0, sizeof(uvdb_threads));
    uvdb_stopped_thread = -1;
    uvdb_general_thread = -1;
    uvdb_continue_thread = -1;
    uvdb_io_failed = 0;
    uvdb_target_stopped = 0;
    uvdb_async_stop_pending = 0;
    uvdb_server_stop = 0;
    uvdb_state = UVDB_STATE_IDLE;
    uvdb_unlock();
}

#define POLL() while(cur == end) { size_t sz = buffer_poll(&in_buf, &cur); if(!sz && uvdb_io_failed) return 0; end = cur + sz; }

static size_t recv_packet(char** data)
{
    char* cur = in_buf.buf;
    char* end = cur + in_buf.size;
retry:;
    char c = 0;
    while(c != '$')
    {
        POLL();
        c = *cur++;
    }
    size_t start_packet = cur - in_buf.buf;
    while(c != '#')
    {
        POLL();
        c = *cur++;
    }
    size_t end_packet = cur - in_buf.buf - 1;
    uint8_t cksum = 0;
    for(size_t i = start_packet; i < end_packet; i++)
        cksum += (uint8_t)in_buf.buf[i];
    POLL();
    char c1 = *cur++;
    POLL();
    char c2 = *cur++;
    if(c1 != int2hex(cksum>>4) || c2 != int2hex(cksum&15))
        goto retry;
    buffer_write(&out_buf, "+", 1);
    //we better not do this here, so that this + and the reply can be merged into a single packet
    //UPD: it seems that this way the communication is a bit more reliable, so let's keep it
    buffer_flush(&out_buf); 
    *data = in_buf.buf + start_packet;
    in_buf.buf[end_packet] = 0;
    return end_packet - start_packet;
}

static void discard_packet(char* data, size_t sz)
{
    buffer_popleft(&in_buf, data - in_buf.buf + sz + 3);
}

static int send_packet(void)
{
    buffer_end_packet(&out_buf);
    buffer_flush(&out_buf);
    if(uvdb_io_failed)
        return -1;
    char* cur = in_buf.buf;
    char* end = cur + in_buf.size;
    char c = 0;
    while(c != '+')
    {
        POLL();
        c = *cur++;
    }
    buffer_popleft(&in_buf, cur - in_buf.buf);
    return 0;
}

#undef POLL
#define IS(s) (sz == sizeof(s) - 1 && !memcmp(pkt, s, sizeof(s) - 1))
#define STARTSWITH(s) (sz >= sizeof(s) - 1 && !memcmp(pkt, s, sizeof(s) - 1))
#define STRING(s) s, sizeof(s) - 1

struct stream
{
    uint64_t cur;
    uint64_t start;
    uint64_t end;
};

#define PARSE_HEX(type, name, cond) static type name(char** s)\
{\
    type ans = 0;\
    for(cond)\
    {\
        char c = *(*s)++;\
        if(c >= '0' && c <= '9')\
            ans = 16 * ans + (c - '0');\
        else\
        {\
            c &= -33;\
            if(c >= 'A' && c <= 'F')\
                ans = 16 * ans + 10 + (c - 'A');\
            else\
                return ans;\
        }\
    }\
    return ans;\
}

PARSE_HEX(uint64_t, parse_hex, ;;)
PARSE_HEX(uint8_t, parse_hex_byte, int c = 0; c < 2; c++)

#undef PARSE_HEX

static struct stream parse_stream(char* s)
{
    struct stream ans = {};
    ans.start = parse_hex(&s);
    uint64_t length = parse_hex(&s);
    ans.end = ans.start + length;
    if(ans.end < ans.start)
        ans.end = UINT64_MAX;
    return ans;
}

static void stream_write(struct stream* st, char* buf, size_t sz)
{
    if(st->cur < st->start)
    {
        size_t chk = st->start - st->cur;
        if(sz <= chk)
        {
            st->cur += sz;
            return;
        }
    }
    if(st->cur < st->end)
    {
        size_t chk = st->end - st->cur;
        if(sz < chk)
            chk = sz;
        if(chk && st->cur == st->start)
            buffer_write(&out_buf, "m", 1);
        buffer_write(&out_buf, buf, chk);
        st->cur += chk;
    }
}

static void stream_close(struct stream* st)
{
    if(st->cur <= st->start)
        buffer_write(&out_buf, "l", 1);
}

static void write_hex(char* start, size_t sz)
{
    while(sz--)
    {
        uint8_t c = *start++;
        uint8_t q[2] = {int2hex(c>>4), int2hex(c&15)};
        buffer_write(&out_buf, q, 2);
    }
}

static void read_hex(char** p, char* start, size_t sz)
{
    while(sz--)
    {
        if(**p == 'x' && (*p)[1] == 'x')
        {
            *p += 2;
            start++;
        }
        else if(!**p || !(*p)[1])
            *start++ = 0;
        else
            *start++ = parse_hex_byte(p);
    }
}

static void skip_hex(char** p, size_t cnt)
{
    *p += strnlen(*p, 2*cnt);
}

static void write_x(size_t sz)
{
    while(sz--)
        buffer_write(&out_buf, "xx", 2);
}

static void write_hex_uint32(uint32_t value)
{
    char digits[8];
    size_t count = 0;
    do
    {
        digits[count++] = int2hex(value & 15);
        value >>= 4;
    }
    while(value && count < sizeof(digits));
    while(count)
        buffer_write(&out_buf, &digits[--count], 1);
}

static SceUID parse_thread_id(char* text)
{
    if(!strcmp(text, "-1"))
        return -1;
    return (SceUID)parse_hex(&text);
}

static int uvdb_thread_is_visible(SceUID id)
{
    return id == uvdb_stopped_thread || uvdb_find_thread(id) != NULL;
}

static size_t safe_memcpy(char* dst, const char* src, size_t sz);

#define UVDB_MAX_BREAKPOINTS 32

struct uvdb_breakpoint
{
    uintptr_t address;
    uint8_t original[4];
    uint8_t size;
    uint8_t active;
    uint8_t temporary;
};

static struct uvdb_breakpoint uvdb_breakpoints[UVDB_MAX_BREAKPOINTS];

static struct uvdb_breakpoint* breakpoint_find(uintptr_t address)
{
    address &= ~(uintptr_t)1;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].active && uvdb_breakpoints[i].address == address)
            return &uvdb_breakpoints[i];
    return NULL;
}

static int breakpoint_insert_internal(uintptr_t address, size_t size, int temporary)
{
    address &= ~(uintptr_t)1;
    if(size != 2 && size != 4)
        return -1;
    if(breakpoint_find(address))
        return 0;

    struct uvdb_breakpoint* bp = NULL;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(!uvdb_breakpoints[i].active)
        {
            bp = &uvdb_breakpoints[i];
            break;
        }
    if(!bp || safe_memcpy((char*)bp->original, (const char*)address, size) != size)
        return -1;

    static const uint8_t thumb_udf[2] = {0x00, 0xde};
    static const uint8_t arm_udf[4] = {0xf0, 0x00, 0xf0, 0xe7};
    const void* trap = size == 2 ? (const void*)thumb_udf : (const void*)arm_udf;
    kuKernelCpuUnrestrictedMemcpy((void*)address, trap, size);
    kuKernelFlushCaches((void*)address, size);
    bp->address = address;
    bp->size = (uint8_t)size;
    bp->temporary = temporary != 0;
    bp->active = 1;
    return 0;
}

static int breakpoint_insert(uintptr_t address, size_t size)
{
    return breakpoint_insert_internal(address, size, 0);
}

static int breakpoint_remove(uintptr_t address)
{
    struct uvdb_breakpoint* bp = breakpoint_find(address);
    if(!bp)
        return 0;
    kuKernelCpuUnrestrictedMemcpy((void*)bp->address, bp->original, bp->size);
    kuKernelFlushCaches((void*)bp->address, bp->size);
    memset(bp, 0, sizeof(*bp));
    return 0;
}

static void breakpoint_remove_all(void)
{
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].active)
            breakpoint_remove(uvdb_breakpoints[i].address);
}

static void breakpoint_remove_temporary(void)
{
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].active && uvdb_breakpoints[i].temporary)
            breakpoint_remove(uvdb_breakpoints[i].address);
}

static int breakpoint_insert_step(KuKernelExceptionContext* ctx)
{
    uintptr_t pc = ctx->pc;
    if(ctx->SPSR & 32)
    {
        uint16_t instruction;
        if(safe_memcpy((char*)&instruction, (const char*)pc, sizeof(instruction)) != sizeof(instruction))
            return -1;
        unsigned int prefix = instruction >> 11;
        size_t instruction_size = (prefix == 0x1d || prefix == 0x1e || prefix == 0x1f) ? 4 : 2;

        // 16-bit conditional branch. Plant traps on both possible paths so the
        // CPU's condition flags, rather than the debugger, choose the result.
        if((instruction & 0xf000) == 0xd000 && (instruction & 0x0f00) < 0x0e00)
        {
            intptr_t offset = (intptr_t)(int8_t)(instruction & 0xff) * 2;
            uintptr_t target = pc + 4 + offset;
            if(breakpoint_insert_internal(pc + 2, 2, 1) < 0 ||
               breakpoint_insert_internal(target, 2, 1) < 0)
            {
                breakpoint_remove_temporary();
                return -1;
            }
            return 0;
        }

        // 16-bit unconditional branch.
        if((instruction & 0xf800) == 0xe000)
        {
            int32_t offset = (int32_t)(instruction & 0x07ff) << 21;
            offset >>= 20;
            return breakpoint_insert_internal(pc + 4 + offset, 2, 1);
        }

        // CBZ/CBNZ. As above, let the processor select between both traps.
        if((instruction & 0xf500) == 0xb100)
        {
            uintptr_t target = pc + 4 + ((instruction & 0x0200) >> 3) +
                               ((instruction & 0x00f8) >> 2);
            if(breakpoint_insert_internal(pc + 2, 2, 1) < 0 ||
               breakpoint_insert_internal(target, 2, 1) < 0)
            {
                breakpoint_remove_temporary();
                return -1;
            }
            return 0;
        }

        // BX/BLX register.
        if((instruction & 0xff00) == 0x4700)
        {
            unsigned int rm = (instruction >> 3) & 0xf;
            const uint32_t* registers = &ctx->r0;
            uintptr_t target = registers[rm];
            return breakpoint_insert_internal(target, (target & 1) ? 2 : 4, 1);
        }

        if(instruction_size == 4)
        {
            uint16_t second;
            if(safe_memcpy((char*)&second, (const char*)(pc + 2), sizeof(second)) != sizeof(second))
                return -1;

            if((instruction & 0xf800) == 0xf000 && (second & 0x8000) == 0x8000)
            {
                // Thumb-2 B.W, BL and BLX immediate. The encoded J bits are
                // complements of I1/I2 XOR the sign bit.
                if((second & 0x1000) != 0 || (second & 0xd001) == 0xc000)
                {
                    uint32_t sign = (instruction >> 10) & 1;
                    uint32_t j1 = (second >> 13) & 1;
                    uint32_t j2 = (second >> 11) & 1;
                    uint32_t i1 = !(j1 ^ sign);
                    uint32_t i2 = !(j2 ^ sign);
                    uint32_t encoded = (sign << 24) | (i1 << 23) | (i2 << 22) |
                                       ((instruction & 0x03ff) << 12) |
                                       ((second & 0x07ff) << 1);
                    int32_t offset = (int32_t)(encoded << 7) >> 7;
                    uintptr_t target = pc + 4 + offset;
                    if((second & 0x1000) == 0)
                        target &= ~(uintptr_t)3;
                    return breakpoint_insert_internal(target, (second & 0x1000) ? 2 : 4, 1);
                }

                // Thumb-2 conditional B.W. Plant both targets and let CPSR
                // choose which path executes.
                if((second & 0xd000) == 0x8000 && (instruction & 0x0380) != 0x0380)
                {
                    uint32_t encoded = (((instruction >> 10) & 1) << 20) |
                                       (((second >> 11) & 1) << 19) |
                                       (((second >> 13) & 1) << 18) |
                                       ((instruction & 0x003f) << 12) |
                                       ((second & 0x07ff) << 1);
                    int32_t offset = (int32_t)(encoded << 11) >> 11;
                    uintptr_t target = pc + 4 + offset;
                    if(breakpoint_insert_internal(pc + 4, 2, 1) < 0 ||
                       breakpoint_insert_internal(target, 2, 1) < 0)
                    {
                        breakpoint_remove_temporary();
                        return -1;
                    }
                    return 0;
                }
            }

            // SUBS PC, LR, #imm8 exception return form.
            if(instruction == 0xf3de && (second & 0xff00) == 0x3f00)
                return breakpoint_insert_internal(ctx->lr - (second & 0xff),
                                                  (ctx->lr & 1) ? 2 : 4, 1);
        }

        return breakpoint_insert_internal(pc + instruction_size, 2, 1);
    }

    uint32_t instruction;
    if(safe_memcpy((char*)&instruction, (const char*)pc, sizeof(instruction)) != sizeof(instruction))
        return -1;

    // ARM B/BL immediate. Conditional forms get traps on both possible paths.
    if((instruction & 0x0e000000) == 0x0a000000)
    {
        int32_t offset = (int32_t)(instruction & 0x00ffffff) << 8;
        offset >>= 6;
        uintptr_t target = pc + 8 + offset;
        if((instruction >> 28) == 0x0e)
            return breakpoint_insert_internal(target, 4, 1);
        if(breakpoint_insert_internal(pc + 4, 4, 1) < 0 ||
           breakpoint_insert_internal(target, 4, 1) < 0)
        {
            breakpoint_remove_temporary();
            return -1;
        }
        return 0;
    }

    // ARM BX/BLX register.
    if((instruction & 0x0ffffff0) == 0x012fff10 ||
       (instruction & 0x0ffffff0) == 0x012fff30)
    {
        const uint32_t* registers = &ctx->r0;
        uintptr_t target = registers[instruction & 0xf];
        return breakpoint_insert_internal(target, (target & 1) ? 2 : 4, 1);
    }

    return breakpoint_insert_internal(pc + 4, 4, 1);
}

static size_t safe_memcpy(char* dst, const char* src, size_t sz)
{
    size_t ans = 0;
    while(sz)
    {
        size_t chk;
        uint32_t rest[3] = {1, (uint32_t)&chk, 0};
        SceKernelAddrPair q = {(uint32_t)src, sz};
        if(_sceKernelSendMsgPipeVector(uvdb_pipe, &q, 1, rest))
            break;
        if(!chk)
            break;
        ans += chk;
        src += chk;
        sz -= chk;
        while(chk)
        {
            size_t chk2;
            uint32_t rest[3] = {1, (uint32_t)&chk2, 0};
            SceKernelAddrPair q = {(uint32_t)dst, chk};
            if(_sceKernelReceiveMsgPipeVector(uvdb_pipe, &q, 1, rest))
                return ans;
            if(!chk2)
                return ans;
            dst += chk2;
            chk -= chk2;
        }
    }
    return ans;
}

static int send_stop_reply(int signal)
{
    buffer_start_packet(&out_buf);
    uint8_t prefix[3] = {'T', int2hex(signal >> 4), int2hex(signal & 15)};
    buffer_write(&out_buf, prefix, sizeof(prefix));
    buffer_write(&out_buf, STRING("thread:"));
    write_hex_uint32((uint32_t)uvdb_stopped_thread);
    buffer_write(&out_buf, STRING(";"));
    return send_packet();
}

static void uvdb_main_loop(KuKernelExceptionContext* ctx, int stop_signal)
{
    for(;;)
    {
        char* pkt;
        size_t sz = recv_packet(&pkt);
        if(uvdb_io_failed)
        {
            breakpoint_remove_all();
            uvdb_close_socket(&uvdb_socket);
            uvdb_state = UVDB_STATE_ERROR;
            __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_SEQ_CST);
            return;
        }
        buffer_start_packet(&out_buf);
        if(STARTSWITH("qSupported:"))
            buffer_write(&out_buf, STRING("qXfer:features:read+"));
        else if(STARTSWITH("qXfer:features:read:target.xml:"))
        {
            struct stream st = parse_stream(pkt + sizeof("qXfer:features:read:target.xml:") - 1);
            stream_write(&st, STRING("<?xml version=\"1.0\"?>\n<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n<target>\n<architecture>armv7</architecture>\n<osabi>GNU/Linux</osabi>\n</target>\n"));
            stream_close(&st);
        }
        else if(IS("?"))
        {
            out_buf.size--;
            if(send_stop_reply(stop_signal) < 0)
                __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_SEQ_CST);
            discard_packet(pkt, sz);
            continue;
        }
        else if(IS("qfThreadInfo"))
        {
            int first = 1;
            buffer_write(&out_buf, STRING("m"));
            for(size_t i = 0; i < UVDB_MAX_THREADS; ++i)
                if(uvdb_threads[i].active)
                {
                    if(!first)
                        buffer_write(&out_buf, STRING(","));
                    write_hex_uint32((uint32_t)uvdb_threads[i].id);
                    first = 0;
                }
            if(uvdb_stopped_thread >= 0 && !uvdb_find_thread(uvdb_stopped_thread))
            {
                if(!first)
                    buffer_write(&out_buf, STRING(","));
                write_hex_uint32((uint32_t)uvdb_stopped_thread);
                first = 0;
            }
            if(first)
            {
                out_buf.size--;
                buffer_write(&out_buf, STRING("l"));
            }
        }
        else if(IS("qsThreadInfo"))
            buffer_write(&out_buf, STRING("l"));
        else if(IS("qC"))
        {
            buffer_write(&out_buf, STRING("QC"));
            write_hex_uint32((uint32_t)uvdb_stopped_thread);
        }
        else if(IS("qAttached"))
            buffer_write(&out_buf, STRING("1"));
        else if(STARTSWITH("qThreadExtraInfo,"))
        {
            SceUID id = parse_thread_id(pkt + sizeof("qThreadExtraInfo,") - 1);
            struct uvdb_thread_entry* entry = uvdb_find_thread(id);
            if(entry && entry->name[0])
                write_hex(entry->name, strlen(entry->name));
            else if(id == uvdb_stopped_thread)
                write_hex("stopped thread", sizeof("stopped thread") - 1);
            else
                buffer_write(&out_buf, STRING("E16"));
        }
        else if(sz >= 2 && pkt[0] == 'H' && (pkt[1] == 'g' || pkt[1] == 'c'))
        {
            SceUID id = parse_thread_id(pkt + 2);
            if(id != 0 && id != -1 && !uvdb_thread_is_visible(id))
                buffer_write(&out_buf, STRING("E16"));
            else
            {
                if(pkt[1] == 'g')
                    uvdb_general_thread = id;
                else
                    uvdb_continue_thread = id;
                buffer_write(&out_buf, STRING("OK"));
            }
        }
        else if(sz > 1 && pkt[0] == 'T')
        {
            SceUID id = parse_thread_id(pkt + 1);
            buffer_write(&out_buf, uvdb_thread_is_visible(id) ? "OK" : "E16",
                         uvdb_thread_is_visible(id) ? 2 : 3);
        }
        else if(IS("g"))
        {
            if(uvdb_general_thread > 0 && uvdb_general_thread != uvdb_stopped_thread)
                write_x(42 * 4);
            else
            {
                write_hex((void*)ctx, 16*4);
                write_x(25*4);
                write_hex((void*)&ctx->SPSR, 4);
            }
        }
        else if(STARTSWITH("m"))
        {
            char* p = pkt + 1;
            uintptr_t addr = parse_hex(&p);
            size_t size = parse_hex(&p);
            while(size)
            {
                size_t chk = size;
                if(chk > 64)
                    chk = 64;
                char buf[64];
                size_t copy_sz = safe_memcpy(buf, (void*)addr, chk);
                write_hex(buf, copy_sz);
                if(copy_sz < chk)
                    break;
                addr += chk;
                size -= chk;
            }
        }
        else if(STARTSWITH("G"))
        {
            if(uvdb_general_thread > 0 && uvdb_general_thread != uvdb_stopped_thread)
                buffer_write(&out_buf, STRING("E16"));
            else
            {
                char* p = pkt + 1;
                read_hex(&p, (void*)ctx, 16*4);
                skip_hex(&p, 25*4);
                read_hex(&p, (void*)&ctx->SPSR, 4);
                buffer_write(&out_buf, "OK", 2);
            }
        }
        else if(STARTSWITH("M"))
        {
            char* p = pkt + 1;
            uintptr_t addr = parse_hex(&p);
            size_t size = parse_hex(&p);
            while(size)
            {
                size_t chk = size;
                if(chk > 64)
                    chk = 64;
                char buf[64];
                read_hex(&p, buf, chk);
                char test[64];
                //this is racey, but should work in practice
                size_t safe_size = safe_memcpy(test, (void*)addr, chk);
                kuKernelCpuUnrestrictedMemcpy((void*)addr, buf, safe_size);
                kuKernelFlushCaches((void*)addr, safe_size);
                if(safe_size < chk)
                    break;
                addr += chk;
                size -= chk;
            }
            if(size)
                buffer_write(&out_buf, "E0e", 3);
            else
                buffer_write(&out_buf, "OK", 2);
        }
        else if(STARTSWITH("Z0,") || STARTSWITH("z0,"))
        {
            int insert = pkt[0] == 'Z';
            char* p = pkt + 3;
            uintptr_t address = parse_hex(&p);
            size_t kind = parse_hex(&p);
            int result = insert ? breakpoint_insert(address, kind) : breakpoint_remove(address);
            buffer_write(&out_buf, result < 0 ? "E16" : "OK", result < 0 ? 3 : 2);
        }
        else if(IS("k"))
            _sceKernelExitProcessForUser(1);
        else if(IS("D"))
        {
            buffer_write(&out_buf, STRING("OK"));
            discard_packet(pkt, sz);
            send_packet();
            uvdb_close_socket(&uvdb_socket);
            uvdb_state = UVDB_STATE_IDLE;
            __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_SEQ_CST);
            return;
        }
        else if(sz && (pkt[0] == 'c' || pkt[0] == 'C' || pkt[0] == 's' || pkt[0] == 'S'))
        {
            int stepping = pkt[0] == 's' || pkt[0] == 'S';
            char* address = NULL;
            if(pkt[0] == 'c' || pkt[0] == 's')
            {
                if(sz > 1)
                    address = pkt + 1;
            }
            else
            {
                char* separator = memchr(pkt, ';', sz);
                if(separator && separator + 1 < pkt + sz)
                    address = separator + 1;
            }
            if(address)
                ctx->pc = (uint32_t)parse_hex(&address);
            if(stepping && breakpoint_insert_step(ctx) < 0)
            {
                buffer_write(&out_buf, "E16", 3);
                discard_packet(pkt, sz);
                send_packet();
                continue;
            }
            memcpy(pkt, "?#3f", 4); //next invocation of uvdb_main_loop will parse it and respond with the status
            out_buf.size--; //undo buffer_start_packet
            buffer_flush(&out_buf); //see the comment in recv_packet
            __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_SEQ_CST);
            return; //no cleanup, this is intentional
        }
        else if(STARTSWITH("F"))
        {
            char* p = pkt + 1;
            ctx->r0 = parse_hex(&p);
            //see above for explanation what this does
            memcpy(pkt, "?#3f", 4);
            for(size_t i = 1; i < sz; i++)
                pkt[i+3] = 0;
            out_buf.size--;
            buffer_flush(&out_buf);
            return;
        }
        else if(IS("qOffsets"))
        {
            char packet[] = "TextSeg=........;DataSeg=........";
            uint32_t value = (uint32_t)__executable_start;
            for(int i = 0; i < 8; i++)
                packet[15-i] = int2hex((value >> (4*i)) & 15);
            value = (uint32_t)__init_array_start;
            for(int i = 0; i < 8; i++)
                packet[32-i] = int2hex((value >> (4*i)) & 15);
            buffer_write(&out_buf, packet, sizeof(packet) - 1);
        }
        discard_packet(pkt, sz);
        if(send_packet() < 0)
        {
            breakpoint_remove_all();
            uvdb_close_socket(&uvdb_socket);
            uvdb_state = UVDB_STATE_ERROR;
            __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_SEQ_CST);
            return;
        }
    }
}

#undef WRITE
#undef STARTSWITH
#undef IS

static __attribute__((naked)) void uvdb_trap_pc(void)
{
    asm volatile("udf #0");
}

static void exception_handler(KuKernelExceptionContext* ctx)
{
    uvdb_stopped_thread = sceKernelGetThreadId();
    __atomic_store_n(&uvdb_target_stopped, 1, __ATOMIC_SEQ_CST);
    if(uvdb_general_thread <= 0 || !uvdb_thread_is_visible(uvdb_general_thread))
        uvdb_general_thread = uvdb_stopped_thread;
    int signal = SIGSEGV;
    if(ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION)
        signal = SIGILL;
    uint32_t pc = ctx->pc;
    if((ctx->SPSR & 32))
    {
        ctx->SPSR &= -33;
        pc |= 1;
    }
    if(pc == (uint32_t)uvdb_trap_pc)
    {
        pc = ctx->r0;
        signal = SIGTRAP;
    }
    else if(ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION && breakpoint_find(pc))
    {
        signal = SIGTRAP;
    }
    if((pc & 1))
    {
        ctx->SPSR |= 32;
        pc &= -2;
    }
    ctx->pc = pc;
    uvdb_last_fault.exception_type = (enum uvdb_exception_type)ctx->exceptionType;
    uvdb_last_fault.signal = signal;
    uvdb_last_fault.fault_status = ctx->FSR;
    uvdb_last_fault.fault_address = ctx->FAR;
    uvdb_last_fault.pc = pc;
    uvdb_last_fault.lr = ctx->lr;
    uvdb_last_fault.sp = ctx->sp;
    breakpoint_remove_temporary();
    uvdb_lock();
    if(__atomic_exchange_n(&uvdb_async_stop_pending, 0, __ATOMIC_SEQ_CST))
    {
        if(send_stop_reply(signal) < 0)
        {
            breakpoint_remove_all();
            uvdb_close_socket(&uvdb_socket);
            uvdb_state = UVDB_STATE_ERROR;
            __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_SEQ_CST);
            uvdb_unlock();
            return;
        }
    }
    uvdb_main_loop(ctx, signal);
    uvdb_unlock();
}

int uvdb_remote_syscall(const char* name, int nargs, ...)
{
    KuKernelExceptionContext ctx = {};
    uvdb_lock();
    if(uvdb_socket < 0)
    {
        //someone attempted to do a remote syscall before the first uvdb_enter
        //do a uvdb_enter now to avoid confusing the code
        uvdb_unlock();
        uvdb_enter();
        uvdb_lock();
    }
    char* pkt;
    size_t sz = recv_packet(&pkt);
    discard_packet(pkt, sz);
    buffer_start_packet(&out_buf);
    buffer_write(&out_buf, "F", 1);
    buffer_write(&out_buf, name, strlen(name));
    va_list va;
    va_start(va, nargs);
    for(int i = 0; i < nargs; i++)
    {
        uintptr_t value = va_arg(va, uintptr_t);
        char packet[9] = ",";
        for(int i = 0; i < 8; i++)
            packet[8-i] = int2hex((value >> (4*i)) & 15);
        buffer_write(&out_buf, packet, 9);
    }
    va_end(va);
    send_packet();
    uvdb_main_loop(&ctx, 0);
    uvdb_unlock();
    return ctx.r0;
}

static __attribute__((used)) uint64_t real_uvdb_enter(uintptr_t lr)
{
    uint64_t no_trap = (uint64_t)lr << 32 | lr;
    uint64_t trap = (uint64_t)(uint32_t)uvdb_trap_pc << 32 | lr;
    uvdb_register_thread(NULL);
    uvdb_lock();
    if(uvdb_socket >= 0)
    {
        uvdb_unlock();
        return trap;
    }
    uvdb_io_failed = 0;
    in_buf.size = 0;
    out_buf.size = 0;
    if(uvdb_pipe < 0)
    {
        uvdb_pipe = sceKernelCreateMsgPipe("pipe to catch efault", 0x40, 0xc, 4*4096, NULL);
        if(uvdb_pipe < 0)
        {
            uvdb_unlock();
            return no_trap;
        }
    }
    if(!uvdb_handler_mask)
    {
        struct KuKernelExceptionHandlerOpt opt = {
            .size = sizeof(opt),
        };
        KuKernelExceptionHandler old;
        const unsigned int all_handlers =
            (1u << KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT) |
            (1u << KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT) |
            (1u << KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION);
        if(kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, exception_handler, &old, &opt) >= 0)
            uvdb_handler_mask |= 1u << KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT;
        if(kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, exception_handler, &old, &opt) >= 0)
            uvdb_handler_mask |= 1u << KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT;
        if(kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION, exception_handler, &old, &opt) >= 0)
            uvdb_handler_mask |= 1u << KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION;
        if(uvdb_handler_mask != all_handlers)
        {
            uvdb_release_handlers();
            uvdb_state = UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }
    }
    uvdb_listen_socket = sceNetSyscallSocket("gdb socket", AF_INET, SOCK_STREAM, 0);
    if(uvdb_listen_socket < 0)
    {
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    int value = 1;
    uint32_t args[5] = {uvdb_listen_socket, SOL_SOCKET, SO_REUSEADDR, (uint32_t)&value, sizeof(value)};
    if(sceNetSyscallSetsockopt((void*)&args))
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    args[1] = IPPROTO_TCP;
    args[2] = TCP_NODELAY;
    if(sceNetSyscallSetsockopt((void*)&args))
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr = {},
        .sin_port = htons(uvdb_port),
    };
    if(sceNetSyscallBind(uvdb_listen_socket, &sin, sizeof(sin)))
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    uvdb_state = UVDB_STATE_LISTENING;
    if(sceNetSyscallListen(uvdb_listen_socket, 1) ||
       (uvdb_socket = sceNetSyscallAccept(uvdb_listen_socket, NULL, NULL)) < 0)
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    uvdb_close_socket(&uvdb_listen_socket);
    uvdb_state = UVDB_STATE_CONNECTED;
    uvdb_unlock();
    return trap;
}

__attribute__((naked)) void uvdb_enter(void)
{
    asm volatile(
        "mov r0, lr\n"
        "bl real_uvdb_enter\n"
        "bx r1\n"
    );
}

static int uvdb_server_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    uvdb_register_thread("uvdb server");

    while(!__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
    {
        if(uvdb_socket < 0)
        {
            uvdb_enter();
            continue;
        }

        if(__atomic_load_n(&uvdb_target_stopped, __ATOMIC_SEQ_CST))
        {
            sceKernelDelayThread(1000);
            continue;
        }

        unsigned char command = 0;
        uint32_t peek_args[6] = {
            (uint32_t)uvdb_socket,
            (uint32_t)&command,
            1,
            MSG_PEEK,
            0,
            0,
        };
        ssize_t received = sceNetSyscallRecvfrom((void*)peek_args);
        if(received <= 0)
        {
            uvdb_lock();
            uvdb_close_socket(&uvdb_socket);
            uvdb_state = UVDB_STATE_IDLE;
            uvdb_unlock();
            continue;
        }

        if(__atomic_load_n(&uvdb_target_stopped, __ATOMIC_SEQ_CST))
            continue;

        if(command == 3)
        {
            uint32_t recv_args[6] = {
                (uint32_t)uvdb_socket,
                (uint32_t)&command,
                1,
                0,
                0,
                0,
            };
            if(sceNetSyscallRecvfrom((void*)recv_args) == 1)
            {
                __atomic_store_n(&uvdb_async_stop_pending, 1, __ATOMIC_SEQ_CST);
                uvdb_enter();
            }
        }
        else
        {
            // Normal RSP packets belong to an exception handler that has just
            // stopped another thread. Leave the byte queued for that handler.
            sceKernelDelayThread(1000);
        }
    }

    uvdb_unregister_thread();
    return 0;
}

int uvdb_start_server(void)
{
    if(uvdb_server_thread >= 0)
        return 0;

    __atomic_store_n(&uvdb_server_stop, 0, __ATOMIC_SEQ_CST);
    SceUID thread = sceKernelCreateThread(
        "uvdb server",
        uvdb_server_main,
        0x10000100,
        64 * 1024,
        0,
        0,
        NULL);
    if(thread < 0)
        return -1;

    uvdb_server_thread = thread;
    if(sceKernelStartThread(thread, 0, NULL) < 0)
    {
        sceKernelDeleteThread(thread);
        uvdb_server_thread = -1;
        return -1;
    }
    return 0;
}
