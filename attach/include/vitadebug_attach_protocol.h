#pragma once

/*
 * Shared constants for the read-only attach discovery wire protocol and its
 * bounded C broker core.
 *
 * Version 1 deliberately defines no process-stop, memory-write, module-load,
 * module-unload, or debugger-start operation.  A future mutating protocol must
 * use a new version and an authenticated authorization envelope.
 */

#define VD_ATTACH_WIRE_VERSION 1u
#define VD_ATTACH_MAX_FRAME_SIZE 4096u
#define VD_ATTACH_CURRENT_KERNEL_ABI 0x0001000Cu

#define VD_ATTACH_REQUEST_ID_HEX_LENGTH 32u
#define VD_ATTACH_NONCE_HEX_LENGTH 64u
#define VD_ATTACH_TITLE_ID_LENGTH 9u

#define VD_ATTACH_CAP_STATUS (1u << 0)
#define VD_ATTACH_CAP_EXACT_TITLE_DISCOVERY (1u << 1)
#define VD_ATTACH_CAP_IDENTITY_TICKET (1u << 2)
#define VD_ATTACH_CAP_TICKET_RELEASE (1u << 3)
#define VD_ATTACH_READ_ONLY_CAPABILITIES \
    (VD_ATTACH_CAP_STATUS | VD_ATTACH_CAP_EXACT_TITLE_DISCOVERY | \
     VD_ATTACH_CAP_IDENTITY_TICKET | VD_ATTACH_CAP_TICKET_RELEASE)

#define VD_ATTACH_MODE_OBSERVE "observe"
#define VD_ATTACH_CONTROL_POLICY_DISABLED "disabled"

#define VD_ATTACH_HELLO_HEADER "VITADEBUG-ATTACH-HELLO-1"
#define VD_ATTACH_HELLO_RESULT_HEADER "VITADEBUG-ATTACH-HELLO-RESULT-1"
#define VD_ATTACH_DISCOVER_HEADER "VITADEBUG-ATTACH-DISCOVER-1"
#define VD_ATTACH_DISCOVER_RESULT_HEADER "VITADEBUG-ATTACH-DISCOVER-RESULT-1"
#define VD_ATTACH_RELEASE_HEADER "VITADEBUG-ATTACH-RELEASE-1"
#define VD_ATTACH_RELEASE_RESULT_HEADER "VITADEBUG-ATTACH-RELEASE-RESULT-1"
