#pragma once

#include <stddef.h>
#include <stdint.h>

int miot_crypto_init(void);
int miot_crypto_make_public_key(uint8_t public_key[64]);
int miot_crypto_derive_session(const uint8_t peer_public_key[64],
                              const uint8_t ltmk[32], uint8_t session_key[64]);
int miot_crypto_make_login_proof(const uint8_t session_key[64],
                                 const uint8_t peer_public_key[64], uint8_t proof[8]);
int miot_crypto_encrypt(const uint8_t session_key[64], uint16_t counter,
                       uint16_t roll, const uint8_t *plain, size_t plain_len,
                       uint8_t *cipher_and_tag, size_t output_capacity,
                       size_t *output_len);
int miot_crypto_decrypt(const uint8_t session_key[64], uint16_t counter,
                       const uint8_t *cipher_and_tag, size_t input_len,
                       uint8_t *plain, size_t output_capacity, size_t *output_len);
