/* NVIDIA RM on Nocturne: the crypto backend of libspdm, unavailable.
 *
 * RM's SPDM code (the DMTF libspdm in third_party/nvidia-open/src/libraries/libspdm) does not
 * bring its own cryptography: it calls the libspdm_* primitives below (SHA-2, HMAC, HKDF,
 * AES-GCM, ECDSA/RSA, X.509, base64, random), which NVIDIA's Linux module implements on top of
 * the Linux kernel crypto API. Nocturne has no such backend and none is wanted here: SPDM
 * sessions with the GPU are used only for confidential computing (Hopper and later) and for
 * attestation, and are never started on an Ada GPU such as the RTX 4070.
 *
 * Every function returns 0 (false / NULL / no work done). A context allocator ("_new") returning
 * NULL and a "verify" returning false make any attempt to start an SPDM session fail cleanly
 * instead of "succeeding" with no cryptography. The functions take whatever arguments the
 * library passes; the x86-64 calling convention lets a callee ignore them.
 */
#include <stdint.h>

#define SPDM_UNAVAILABLE(name) uintptr_t name(void) { return 0; }

SPDM_UNAVAILABLE(libspdm_aead_aes_gcm_decrypt)
SPDM_UNAVAILABLE(libspdm_aead_aes_gcm_decrypt_prealloc)
SPDM_UNAVAILABLE(libspdm_aead_aes_gcm_encrypt)
SPDM_UNAVAILABLE(libspdm_aead_aes_gcm_encrypt_prealloc)
SPDM_UNAVAILABLE(libspdm_aead_free)
SPDM_UNAVAILABLE(libspdm_aead_gcm_prealloc)
SPDM_UNAVAILABLE(libspdm_asn1_get_tag)
SPDM_UNAVAILABLE(libspdm_check_crypto_backend)
SPDM_UNAVAILABLE(libspdm_decode_base64)
SPDM_UNAVAILABLE(libspdm_ec_compute_key)
SPDM_UNAVAILABLE(libspdm_ec_free)
SPDM_UNAVAILABLE(libspdm_ec_generate_key)
SPDM_UNAVAILABLE(libspdm_ec_get_public_key_from_x509)
SPDM_UNAVAILABLE(libspdm_ec_new_by_nid)
SPDM_UNAVAILABLE(libspdm_ecdsa_sign)
SPDM_UNAVAILABLE(libspdm_ecdsa_verify)
SPDM_UNAVAILABLE(libspdm_encode_base64)
SPDM_UNAVAILABLE(libspdm_hkdf_sha256_expand)
SPDM_UNAVAILABLE(libspdm_hkdf_sha256_extract)
SPDM_UNAVAILABLE(libspdm_hkdf_sha384_expand)
SPDM_UNAVAILABLE(libspdm_hkdf_sha384_extract)
SPDM_UNAVAILABLE(libspdm_hmac_sha256_all)
SPDM_UNAVAILABLE(libspdm_hmac_sha256_duplicate)
SPDM_UNAVAILABLE(libspdm_hmac_sha256_final)
SPDM_UNAVAILABLE(libspdm_hmac_sha256_free)
SPDM_UNAVAILABLE(libspdm_hmac_sha256_new)
SPDM_UNAVAILABLE(libspdm_hmac_sha256_set_key)
SPDM_UNAVAILABLE(libspdm_hmac_sha256_update)
SPDM_UNAVAILABLE(libspdm_hmac_sha384_all)
SPDM_UNAVAILABLE(libspdm_hmac_sha384_duplicate)
SPDM_UNAVAILABLE(libspdm_hmac_sha384_final)
SPDM_UNAVAILABLE(libspdm_hmac_sha384_free)
SPDM_UNAVAILABLE(libspdm_hmac_sha384_new)
SPDM_UNAVAILABLE(libspdm_hmac_sha384_set_key)
SPDM_UNAVAILABLE(libspdm_hmac_sha384_update)
SPDM_UNAVAILABLE(libspdm_random_bytes)
SPDM_UNAVAILABLE(libspdm_rsa_free)
SPDM_UNAVAILABLE(libspdm_rsa_get_public_key_from_x509)
SPDM_UNAVAILABLE(libspdm_rsa_new)
SPDM_UNAVAILABLE(libspdm_rsa_pss_sign)
SPDM_UNAVAILABLE(libspdm_rsa_pss_verify)
SPDM_UNAVAILABLE(libspdm_rsa_set_key)
SPDM_UNAVAILABLE(libspdm_sha256_duplicate)
SPDM_UNAVAILABLE(libspdm_sha256_final)
SPDM_UNAVAILABLE(libspdm_sha256_free)
SPDM_UNAVAILABLE(libspdm_sha256_hash_all)
SPDM_UNAVAILABLE(libspdm_sha256_init)
SPDM_UNAVAILABLE(libspdm_sha256_new)
SPDM_UNAVAILABLE(libspdm_sha256_update)
SPDM_UNAVAILABLE(libspdm_sha384_duplicate)
SPDM_UNAVAILABLE(libspdm_sha384_final)
SPDM_UNAVAILABLE(libspdm_sha384_free)
SPDM_UNAVAILABLE(libspdm_sha384_hash_all)
SPDM_UNAVAILABLE(libspdm_sha384_init)
SPDM_UNAVAILABLE(libspdm_sha384_new)
SPDM_UNAVAILABLE(libspdm_sha384_update)
SPDM_UNAVAILABLE(libspdm_x509_compare_date_time)
SPDM_UNAVAILABLE(libspdm_x509_get_cert_from_cert_chain)
SPDM_UNAVAILABLE(libspdm_x509_get_extended_basic_constraints)
SPDM_UNAVAILABLE(libspdm_x509_get_extended_key_usage)
SPDM_UNAVAILABLE(libspdm_x509_get_extension_data)
SPDM_UNAVAILABLE(libspdm_x509_get_issuer_name)
SPDM_UNAVAILABLE(libspdm_x509_get_key_usage)
SPDM_UNAVAILABLE(libspdm_x509_get_serial_number)
SPDM_UNAVAILABLE(libspdm_x509_get_signature_algorithm)
SPDM_UNAVAILABLE(libspdm_x509_get_subject_name)
SPDM_UNAVAILABLE(libspdm_x509_get_validity)
SPDM_UNAVAILABLE(libspdm_x509_get_version)
SPDM_UNAVAILABLE(libspdm_x509_set_date_time)
SPDM_UNAVAILABLE(libspdm_x509_verify_cert)
SPDM_UNAVAILABLE(libspdm_x509_verify_cert_chain)
