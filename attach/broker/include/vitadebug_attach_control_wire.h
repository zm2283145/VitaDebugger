#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_attach_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Canonical signing inputs for the future authenticated control protocol.
 * Domains are exact ASCII bytes without a terminating NUL. Integer fields are
 * unsigned, fixed-width, and encoded most-significant byte first. Signatures
 * are deliberately excluded from both encodings.
 */
#define VD_ATTACH_CONTROL_PEER_AUTH_DOMAIN \
    "VITADEBUG-ATTACH/PEER-AUTH/v1"
#define VD_ATTACH_CONTROL_OPERATION_AUTH_DOMAIN \
    "VITADEBUG-ATTACH/OPERATION-AUTH/v1"

#define VD_ATTACH_CONTROL_PEER_AUTH_DOMAIN_BYTES 29u
#define VD_ATTACH_CONTROL_OPERATION_AUTH_DOMAIN_BYTES 34u
#define VD_ATTACH_CONTROL_PEER_SIGNED_BYTES 153u
#define VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES 243u

int vd_attach_control_encode_peer_transcript(
    const VdAttachControlPeerTranscript *transcript,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size);

int vd_attach_control_encode_operation_authorization(
    const VdAttachControlAuthorization *authorization,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size);

#ifdef __cplusplus
}
#endif
