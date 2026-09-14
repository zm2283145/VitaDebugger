#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"
#include "uvdb.h"

#ifndef DEMO_GDB_PORT
#define DEMO_GDB_PORT 1234
#endif
#define DEMO_NET_MEMORY_SIZE (1024 * 1024)

static unsigned char demo_net_memory[DEMO_NET_MEMORY_SIZE]
    __attribute__((aligned(64)));
static int demo_net_module_loaded;
static int demo_net_initialized;
static int demo_netctl_initialized;

/*
 * Keep this object global, volatile, and visible in the unstripped ELF. Add it
 * to VS Code's Watch panel, change 1 to 42, and Continue. The Vita screen will
 * confirm the write without rebuilding the application.
 */
volatile int demo_value = 1;
volatile unsigned int demo_heartbeat;

__attribute__((noinline, noclone, used, visibility("default")))
void vscode_demo_breakpoint(void)
{
    __asm__ volatile("" : : "r"(&demo_value) : "memory");
}

static void set_screen_position(int x, int y)
{
    psvDebugScreenSetCoordsXY(&x, &y);
}

static void write_screen_line(int y, const char* format, ...)
{
    char line[112];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    line[sizeof(line) - 1] = '\0';

    set_screen_position(24, y);
    psvDebugScreenPrintf("%-105.105s", line);
}

static int start_network(char ip_address[16])
{
    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if(result < 0)
        return result;
    demo_net_module_loaded = 1;

    SceNetInitParam init = {
        .memory = demo_net_memory,
        .size = sizeof(demo_net_memory),
        .flags = 0,
    };
    result = sceNetInit(&init);
    if(result < 0)
        return result;
    demo_net_initialized = 1;

    result = sceNetCtlInit();
    if(result < 0)
        return result;
    demo_netctl_initialized = 1;

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

    strncpy(ip_address, info.ip_address, 15);
    ip_address[15] = '\0';
    return 0;
}

static void stop_network(void)
{
    if(demo_netctl_initialized)
    {
        sceNetCtlTerm();
        demo_netctl_initialized = 0;
    }
    if(demo_net_initialized)
    {
        sceNetTerm();
        demo_net_initialized = 0;
    }
    if(demo_net_module_loaded)
    {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        demo_net_module_loaded = 0;
    }
}

static const char* value_message(int value)
{
    if(value == 1)
        return "Waiting: set demo_value to 42 in VS Code";
    if(value == 42)
        return "Changed live from VS Code - success!";
    return "Custom debugger value received";
}

static void draw_live_values(const char* endpoint, int server_result,
                             int stdio_result)
{
    int value = demo_value;

    write_screen_line(32, "VitaDebugger - VS Code live-edit demo");
    write_screen_line(72, "GDB endpoint: %s", endpoint);
    write_screen_line(112, "Debugger service: %s",
                      server_result == 0 ? "running" : "not started");
    write_screen_line(144, "GDB console bridge: %s",
                      stdio_result == 0 ? "running" : "not started");
    write_screen_line(200, "demo_value = %d", value);
    write_screen_line(232, "%s", value_message(value));
    write_screen_line(288, "heartbeat = %u", demo_heartbeat);
    write_screen_line(352, "Press X to revisit vscode_demo_breakpoint.");
    write_screen_line(384, "Press Circle to shut down cleanly.");
}

int main(void)
{
    char ip_address[16] = {0};
    char endpoint[32] = "network unavailable";

    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    write_screen_line(32, "VitaDebugger - VS Code live-edit demo");
    write_screen_line(72, "Initializing Vita networking...");

    int network_result = start_network(ip_address);
    if(network_result < 0)
    {
        write_screen_line(112, "Network initialization failed: 0x%08X",
                          network_result);
        write_screen_line(144, "Close the app and verify the Vita Wi-Fi link.");
        sceKernelDelayThread(5000000);
        stop_network();
        psvDebugScreenFinish();
        return 1;
    }
    snprintf(endpoint, sizeof(endpoint), "%s:%d", ip_address, DEMO_GDB_PORT);

    const struct uvdb_config debugger_config = {
        .port = DEMO_GDB_PORT,
        .max_packet_buffer = 256 * 1024,
    };
    int configure_result = uvdb_configure(&debugger_config);
    int register_result = uvdb_register_thread("VS Code demo main");
    if(configure_result < 0 || register_result < 0)
    {
        write_screen_line(112,
                          "Debugger setup failed: configure=%d register=%d",
                          configure_result, register_result);
        sceKernelDelayThread(5000000);
        uvdb_shutdown();
        stop_network();
        psvDebugScreenFinish();
        return 1;
    }

    write_screen_line(112, "Waiting for the first GDB connection at %s...",
                      endpoint);

    /* The ASLR helper owns this first deterministic connection, takes one
     * verified module snapshot, and detaches. */
    uvdb_enter();

    /* Keep the application thread itself at the second stop so VS Code receives
     * a readable PC before it installs source breakpoints. After Continue, the
     * persistent service takes over this connection and future reconnects. */
    write_screen_line(112, "Verified symbols captured; waiting for VS Code...");
    uvdb_enter();

    int server_result = uvdb_start_server();
    int stdio_result = server_result == 0 ? uvdb_redirect_stdio() : -1;
    int last_reported_value = demo_value;
    printf("VitaDebugger demo ready; demo_value=%d\n", last_reported_value);

    SceCtrlData pad;
    unsigned int previous_buttons = 0;
    int running = 1;
    while(running)
    {
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        if((demo_value == 1 && (demo_heartbeat % 5u) == 0u) ||
           (pressed & SCE_CTRL_CROSS))
            vscode_demo_breakpoint();
        if(pressed & SCE_CTRL_CIRCLE)
            running = 0;

        if(last_reported_value != demo_value)
        {
            last_reported_value = demo_value;
            printf("demo_value changed through GDB: %d\n",
                   last_reported_value);
        }

        ++demo_heartbeat;
        __asm__ volatile("" ::: "memory");
        draw_live_values(endpoint, server_result, stdio_result);
        sceKernelDelayThread(100000);
    }

    uvdb_unregister_thread();
    uvdb_shutdown();
    stop_network();
    psvDebugScreenFinish();
    sceKernelExitProcess(0);
    return 0;
}
