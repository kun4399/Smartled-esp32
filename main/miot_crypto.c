#include "miot_crypto.h"

#include <stdbool.h>
#include <string.h>

#include "psa/crypto.h"

static psa_key_id_t ephemeral_key;
static bool initialized;

static void make_nonce(uint8_t nonce[12], const uint8_t iv[4],
                       uint16_t counter, uint16_t roll)
{
    memset(nonce, 0, 12);
    memcpy(nonce, iv, 4);
    nonce[8] = (uint8_t)counter;
    nonce[9] = (uint8_t)(counter >> 8);
    nonce[10] = (uint8_t)roll;
    nonce[11] = (uint8_t)(roll >> 8);
}

int miot_crypto_init(void)
{
    psa_status_t status = psa_crypto_init();
    initialized = status == PSA_SUCCESS;
    return status;
}

int miot_crypto_make_public_key(uint8_t public_key[64])
{
    if (!initialized) return PSA_ERROR_BAD_STATE;
    if (ephemeral_key != 0) {
        psa_destroy_key(ephemeral_key);
        ephemeral_key = 0;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    psa_status_t status = psa_generate_key(&attr, &ephemeral_key);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) return status;
    uint8_t uncompressed[65];
    size_t output_len = 0;
    status = psa_export_public_key(ephemeral_key, uncompressed, sizeof(uncompressed), &output_len);
    if (status != PSA_SUCCESS || output_len != sizeof(uncompressed) || uncompressed[0] != 0x04) {
        psa_destroy_key(ephemeral_key);
        ephemeral_key = 0;
        return status == PSA_SUCCESS ? PSA_ERROR_DATA_INVALID : status;
    }
    memcpy(public_key, uncompressed + 1, 64);
    memset(uncompressed, 0, sizeof(uncompressed));
    return PSA_SUCCESS;
}

int miot_crypto_derive_session(const uint8_t peer_public_key[64],
                              const uint8_t ltmk[32], uint8_t session_key[64])
{
    if (ephemeral_key == 0) return PSA_ERROR_BAD_STATE;
    uint8_t encoded_peer[65];
    uint8_t shared[32];
    uint8_t ikm[64];
    encoded_peer[0] = 0x04;
    memcpy(encoded_peer + 1, peer_public_key, 64);
    size_t shared_len = 0;
    psa_status_t status = psa_raw_key_agreement(PSA_ALG_ECDH, ephemeral_key,
                                                encoded_peer, sizeof(encoded_peer),
                                                shared, sizeof(shared), &shared_len);
    psa_destroy_key(ephemeral_key);
    ephemeral_key = 0;
    memset(encoded_peer, 0, sizeof(encoded_peer));
    if (status != PSA_SUCCESS || shared_len != sizeof(shared)) {
        memset(shared, 0, sizeof(shared));
        return status == PSA_SUCCESS ? PSA_ERROR_DATA_INVALID : status;
    }
    memcpy(ikm, shared, sizeof(shared));
    memcpy(ikm + sizeof(shared), ltmk, 32);
    memset(shared, 0, sizeof(shared));

    static const uint8_t salt[] = "miot-mesh-login-salt";
    static const uint8_t info[] = "miot-mesh-login-info";
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    psa_key_id_t input_key = 0;
    status = psa_import_key(&attr, ikm, sizeof(ikm), &input_key);
    psa_reset_key_attributes(&attr);
    memset(ikm, 0, sizeof(ikm));
    if (status != PSA_SUCCESS) return status;

    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    status = psa_key_derivation_setup(&operation, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_bytes(&operation,
        PSA_KEY_DERIVATION_INPUT_SALT, salt, sizeof(salt) - 1);
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_bytes(&operation,
        PSA_KEY_DERIVATION_INPUT_INFO, info, sizeof(info) - 1);
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_key(&operation,
        PSA_KEY_DERIVATION_INPUT_SECRET, input_key);
    if (status == PSA_SUCCESS) status = psa_key_derivation_output_bytes(&operation,
        session_key, 64);
    psa_key_derivation_abort(&operation);
    psa_destroy_key(input_key);
    return status;
}

static uint32_t crc32_le(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

static int ccm_crypt(bool encrypt, const uint8_t *key, const uint8_t nonce[12],
                     const uint8_t *input, size_t input_len, uint8_t *output,
                     size_t output_capacity, size_t *output_len)
{
    if (output_len == NULL || (encrypt && output_capacity < input_len + 4) ||
        (!encrypt && (input_len < 4 || output_capacity < input_len - 4))) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4));
    psa_key_id_t key_id = 0;
    psa_status_t status = psa_import_key(&attr, key, 16, &key_id);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) return status;
    if (encrypt) {
        status = psa_aead_encrypt(key_id, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4),
                                  nonce, 12, NULL, 0, input, input_len,
                                  output, output_capacity, output_len);
    } else {
        status = psa_aead_decrypt(key_id, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4),
                                  nonce, 12, NULL, 0, input, input_len,
                                  output, output_capacity, output_len);
    }
    psa_destroy_key(key_id);
    return status;
}

int miot_crypto_make_login_proof(const uint8_t session_key[64],
                                 const uint8_t peer_public_key[64], uint8_t proof[8])
{
    uint8_t nonce[12];
    uint8_t crc[4];
    for (int i = 0; i < 12; ++i) nonce[i] = (uint8_t)(0x10 + i);
    uint32_t value = crc32_le(peer_public_key, 64);
    crc[0] = (uint8_t)value;
    crc[1] = (uint8_t)(value >> 8);
    crc[2] = (uint8_t)(value >> 16);
    crc[3] = (uint8_t)(value >> 24);
    size_t output_len = 0;
    return ccm_crypt(true, session_key + 16, nonce, crc, sizeof(crc),
                     proof, 8, &output_len) == PSA_SUCCESS && output_len == 8 ?
           PSA_SUCCESS : PSA_ERROR_GENERIC_ERROR;
}

int miot_crypto_encrypt(const uint8_t session_key[64], uint16_t counter,
                       uint16_t roll, const uint8_t *plain, size_t plain_len,
                       uint8_t *cipher_and_tag, size_t output_capacity,
                       size_t *output_len)
{
    uint8_t nonce[12];
    make_nonce(nonce, session_key + 36, counter, roll);
    return ccm_crypt(true, session_key + 16, nonce, plain, plain_len,
                     cipher_and_tag, output_capacity, output_len);
}

int miot_crypto_decrypt(const uint8_t session_key[64], uint16_t counter,
                       const uint8_t *cipher_and_tag, size_t input_len,
                       uint8_t *plain, size_t output_capacity, size_t *output_len)
{
    uint8_t nonce[12];
    make_nonce(nonce, session_key + 32, counter, 0);
    return ccm_crypt(false, session_key, nonce, cipher_and_tag, input_len,
                     plain, output_capacity, output_len);
}
