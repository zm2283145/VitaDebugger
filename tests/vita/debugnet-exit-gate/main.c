#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"
#include "uvdb.h"

#ifndef DEBUGNET_GATE_HOST
#error "DEBUGNET_GATE_HOST must name the receiver IPv4 address"
#endif

#ifndef DEBUGNET_GATE_PORT
#define DEBUGNET_GATE_PORT 18194
#endif

#define GATE_NET_MEMORY_SIZE (1024 * 1024)
#define GATE_FLOOD_MESSAGES 2048u

static unsigned char gate_net_memory[GATE_NET_MEMORY_SIZE]
    __attribute__((aligned(64)));
static int gate_net_module_loaded;
static int gate_net_initialized;
static int gate_netctl_initialized;

static int start_network(char ip_address[16])
{
    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if(result < 0)
        return result;
    gate_net_module_loaded = 1;

    SceNetInitParam init = {
        .memory = gate_net_memory,
        .size = sizeof(gate_net_memory),
        .flags = 0,
    };
    result = sceNetInit(&init);
    if(result < 0)
        return result;
    gate_net_initialized = 1;

    result = sceNetCtlInit();
    if(result < 0)
        return result;
    gate_netctl_initialized = 1;

    int state = SCE_NETCTL_STATE_DISCONNECTED;
    for(unsigned int attempt = 0; attempt < 1500; ++attempt)
    {
        result = sceNetCtlInetGetState(&state);
        if(result >= 0 && state == SCE_NETCTL_STATE_CONNECTED)
            break;
        sceKernelDelayThread(10000);
    }
    if(state != SCE_NETCTL_STATE_CONNECTED)
        return -1;

    SceNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    result = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
    if(result < 0)
        return result;
    memcpy(ip_address, info.ip_address, 15);
    ip_address[15] = '\0';
    return 0;
}

static void stop_network(void)
{
    if(gate_netctl_initialized)
    {
        sceNetCtlTerm();
        gate_netctl_initialized = 0;
    }
    if(gate_net_initialized)
    {
        sceNetTerm();
        gate_net_initialized = 0;
    }
    if(gate_net_module_loaded)
    {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        gate_net_module_loaded = 0;
    }
}

static void print_stats(const char* label)
{
    struct uvdb_debugnet_stats stats;
    if(uvdb_debugnet_get_stats(&stats) < 0)
    {
        psvDebugScreenPrintf("%s stats unavailable\n", label);
        return;
    }
    psvDebugScreenPrintf(
        "%s sent=%u queued=%u drop=%u trunc=%u err=%u last=%08X\n",
        label, stats.sent, stats.queued, stats.dropped, stats.truncated,
        stats.send_errors, (uint32_t)stats.last_send_error);
}

static void flood_queue(uint32_t run_id)
{
    char payload[768];
    for(size_t index = 0; index + 1 < sizeof(payload); ++index)
        payload[index] = (char)('A' + (index % 26));
    payload[sizeof(payload) - 1] = '\0';

    uvdb_debugnet_printf(
        UVDB_LOG_INFO,
        "DEBUGNET_EXIT_GATE run=%08X path=QUEUE_FILL begin count=%u\n",
        run_id, GATE_FLOOD_MESSAGES);
    /* Give the path marker a chance to leave before deliberately saturating. */
    sceKernelDelayThread(50000);

    unsigned int accepted = 0;
    unsigned int rejected = 0;
    for(unsigned int index = 0; index < GATE_FLOOD_MESSAGES; ++index)
    {
        int result = uvdb_debugnet_printf(
            UVDB_LOG_TRACE,
            "DEBUGNET_EXIT_GATE run=%08X path=QUEUE_FILL seq=%04u %s\n",
            run_id, index, payload);
        if(result == 0 || result == 2)
            ++accepted;
        else
            ++rejected;
    }
    psvDebugScreenPrintf("Flood complete: accepted=%u rejected=%u\n",
                         accepted, rejected);
    print_stats("Before owner exit:");
}

int main(void)
{
    if(psvDebugScreenInit() < 0)
        return 1;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    psvDebugScreenPrintf("VitaDebugNet owner-exit hardware gate\n");
    psvDebugScreenPrintf("Title ID: VDLG00001 (logger only)\n");
    psvDebugScreenPrintf("No GDB and no kernel companion are used.\n\n");
    psvDebugScreenPrintf("Starting Vita network...\n");

    char vita_ip[16] = {0};
    int network_result = start_network(vita_ip);
    if(network_result < 0)
    {
        psvDebugScreenPrintf("Network FAILED: %08X\n", network_result);
        psvDebugScreenPrintf("Press O to clean up and exit.\n");
        for(;;)
        {
            SceCtrlData pad;
            memset(&pad, 0, sizeof(pad));
            sceCtrlPeekBufferPositive(0, &pad, 1);
            if(pad.buttons & SCE_CTRL_CIRCLE)
                break;
            sceKernelDelayThread(16000);
        }
        stop_network();
        psvDebugScreenFinish();
        return 1;
    }

    struct uvdb_debugnet_config config = {
        .server_ip = DEBUGNET_GATE_HOST,
        .port = DEBUGNET_GATE_PORT,
        .level = UVDB_LOG_TRACE,
    };
    int log_result = uvdb_debugnet_start(&config);
    uint32_t run_id = (uint32_t)sceKernelGetSystemTimeWide();
    psvDebugScreenPrintf("Vita: %s\n", vita_ip);
    psvDebugScreenPrintf("Receiver: %s:%u\n", DEBUGNET_GATE_HOST,
                         DEBUGNET_GATE_PORT);
    psvDebugScreenPrintf("Run ID: %08X\n", run_id);
    psvDebugScreenPrintf("DebugNet start: %s (%08X)\n\n",
                         log_result == 0 ? "PASS" : "FAIL", log_result);
    if(log_result < 0)
    {
        psvDebugScreenPrintf("Press O to clean up and exit.\n");
        for(;;)
        {
            SceCtrlData pad;
            memset(&pad, 0, sizeof(pad));
            sceCtrlPeekBufferPositive(0, &pad, 1);
            if(pad.buttons & SCE_CTRL_CIRCLE)
                break;
            sceKernelDelayThread(16000);
        }
        stop_network();
        psvDebugScreenFinish();
        return 1;
    }

    uvdb_debugnet_printf(
        UVDB_LOG_INFO,
        "DEBUGNET_EXIT_GATE run=%08X path=READY vita=%s receiver=%s:%u\n",
        run_id, vita_ip, DEBUGNET_GATE_HOST, DEBUGNET_GATE_PORT);

    psvDebugScreenPrintf("X: return from main WITHOUT DebugNet stop\n");
    psvDebugScreenPrintf("[]: fill bounded queue, then return WITHOUT stop\n");
    psvDebugScreenPrintf("TRIANGLE: explicit DebugNet stop control path\n");
    psvDebugScreenPrintf("PS: leave logger running; launch another app and\n");
    psvDebugScreenPrintf("    accept Close, or peel this gate in LiveArea.\n");
    psvDebugScreenPrintf("    Do NOT press a gate button for this test.\n");
    psvDebugScreenPrintf("After X/[]/PS-close, launch another app at once.\n");

    unsigned int previous_buttons = 0;
    unsigned int heartbeat = 0;
    SceInt64 next_heartbeat = sceKernelGetSystemTimeWide() + 1000000;
    for(;;)
    {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        SceInt64 now = sceKernelGetSystemTimeWide();
        if(now >= next_heartbeat)
        {
            ++heartbeat;
            uvdb_debugnet_printf(
                UVDB_LOG_INFO,
                "DEBUGNET_EXIT_GATE run=%08X path=SHELL_CLOSE_WAIT "
                "heartbeat=%u\n",
                run_id, heartbeat);
            next_heartbeat = now + 1000000;
        }

        if(pressed & SCE_CTRL_CROSS)
        {
            uvdb_debugnet_printf(
                UVDB_LOG_INFO,
                "DEBUGNET_EXIT_GATE run=%08X path=OWNER_EXIT begin\n",
                run_id);
            psvDebugScreenPrintf("\nReturning now WITHOUT stop (run %08X).\n",
                                 run_id);
            sceKernelDelayThread(50000);
            return 0;
        }
        if(pressed & SCE_CTRL_SQUARE)
        {
            psvDebugScreenPrintf("\nFilling queue for run %08X...\n", run_id);
            flood_queue(run_id);
            psvDebugScreenPrintf("Returning now WITHOUT stop.\n");
            return 0;
        }
        if(pressed & SCE_CTRL_TRIANGLE)
        {
            uvdb_debugnet_printf(
                UVDB_LOG_INFO,
                "DEBUGNET_EXIT_GATE run=%08X path=EXPLICIT_STOP begin\n",
                run_id);
            sceKernelDelayThread(50000);
            print_stats("Before stop:");
            int stop_result = uvdb_debugnet_stop();
            psvDebugScreenPrintf("Explicit stop: %s (%08X)\n",
                                 stop_result == 0 ? "PASS" : "FAIL",
                                 stop_result);
            psvDebugScreenPrintf("Press O to close this control run.\n");
            for(;;)
            {
                memset(&pad, 0, sizeof(pad));
                sceCtrlPeekBufferPositive(0, &pad, 1);
                if(pad.buttons & SCE_CTRL_CIRCLE)
                    break;
                sceKernelDelayThread(16000);
            }
            stop_network();
            psvDebugScreenFinish();
            return stop_result == 0 ? 0 : 1;
        }
        sceKernelDelayThread(16000);
    }
}
