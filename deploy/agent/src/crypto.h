#ifndef VITADEVDEPLOY_CRYPTO_H
#define VITADEVDEPLOY_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

int vdev_verify_job_signature(const uint8_t signature[64],
                              const uint8_t *request, size_t request_size,
                              const uint8_t *manifest, size_t manifest_size);

#endif
