#include "common.h"
#include "crypto.h"

#include <monocypher.h>
#include <monocypher-ed25519.h>

#include <stdlib.h>
#include <string.h>

static const uint8_t signed_job_domain[] = "VITADEVDEPLOY-SIGNED-JOB-1";

int vdev_verify_job_signature(const uint8_t signature[64],
                              const uint8_t *request, size_t request_size,
                              const uint8_t *manifest, size_t manifest_size)
{
#ifdef VDD_PUBLIC_KEY_HEX
    static const char public_key_hex[] = VDD_PUBLIC_KEY_HEX;
    uint8_t public_key[32];
    uint8_t *message;
    size_t message_size;
    int result;

    if (sizeof(public_key_hex) != 65u ||
        !vdev_is_lower_hex(public_key_hex, 64u) ||
        vdev_hex_decode(public_key_hex, 64u, public_key, sizeof(public_key)) < 0) {
        return VDEV_ERR_TRUST_KEY;
    }
    if (request_size > SIZE_MAX - sizeof(signed_job_domain) ||
        manifest_size > SIZE_MAX - sizeof(signed_job_domain) - request_size) {
        crypto_wipe(public_key, sizeof(public_key));
        return VDEV_ERR_LIMIT;
    }
    message_size = sizeof(signed_job_domain) + request_size + manifest_size;
    message = (uint8_t *)malloc(message_size);
    if (message == NULL) {
        crypto_wipe(public_key, sizeof(public_key));
        return VDEV_ERR_OOM;
    }
    memcpy(message, signed_job_domain, sizeof(signed_job_domain));
    memcpy(message + sizeof(signed_job_domain), request, request_size);
    memcpy(message + sizeof(signed_job_domain) + request_size,
           manifest, manifest_size);
    result = crypto_ed25519_check(signature, public_key, message, message_size);
    crypto_wipe(public_key, sizeof(public_key));
    crypto_wipe(message, message_size);
    free(message);
    return result == 0 ? VDEV_OK : VDEV_ERR_SIGNATURE;
#else
    (void)signature;
    (void)request;
    (void)request_size;
    (void)manifest;
    (void)manifest_size;
    return VDEV_ERR_TRUST_KEY;
#endif
}
