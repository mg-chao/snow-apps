//! Windows CNG primitives: SHA-256 hashing and RSA-3072/PSS verification.
//! Keeping crypto in the operating system avoids embedding any cryptographic
//! code in the helper, exactly like the previous Qt implementation.

use crate::errors::{Error, Result};
use std::fmt::Write as _;
use std::io::Read;
use std::path::Path;
use windows::Win32::Security::Cryptography::{
    BCRYPT_HASH_HANDLE, BCRYPT_OPEN_ALGORITHM_PROVIDER_FLAGS, BCRYPT_PAD_PSS,
    BCRYPT_PSS_PADDING_INFO, BCRYPT_RSA_ALGORITHM, BCRYPT_RSAKEY_BLOB, BCRYPT_RSAPUBLIC_BLOB,
    BCRYPT_SHA256_ALGORITHM, BCryptCloseAlgorithmProvider, BCryptCreateHash, BCryptDestroyHash,
    BCryptDestroyKey, BCryptFinishHash, BCryptHashData, BCryptImportKeyPair,
    BCryptOpenAlgorithmProvider, BCryptVerifySignature,
};

const NO_PROVIDER_FLAGS: BCRYPT_OPEN_ALGORITHM_PROVIDER_FLAGS =
    BCRYPT_OPEN_ALGORITHM_PROVIDER_FLAGS(0);

pub fn hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        let _ = write!(text, "{byte:02x}");
    }
    text
}

/// Streaming CNG SHA-256.
pub struct Sha256 {
    handle: BCRYPT_HASH_HANDLE,
}

impl Drop for Sha256 {
    fn drop(&mut self) {
        unsafe {
            let _ = BCryptDestroyHash(self.handle);
        }
    }
}

impl Sha256 {
    pub fn new() -> Result<Self> {
        unsafe {
            let mut algorithm = Default::default();
            if !BCryptOpenAlgorithmProvider(
                &mut algorithm,
                BCRYPT_SHA256_ALGORITHM,
                None,
                NO_PROVIDER_FLAGS,
            )
            .is_ok()
            {
                return Err(Error::fixed("Could not hash update payload"));
            }
            let mut handle = Default::default();
            let created = BCryptCreateHash(algorithm, &mut handle, None, None, 0);
            let _ = BCryptCloseAlgorithmProvider(algorithm, 0);
            if !created.is_ok() {
                return Err(Error::fixed("Could not hash update payload"));
            }
            Ok(Sha256 { handle })
        }
    }

    pub fn update(&mut self, data: &[u8]) -> Result<()> {
        if unsafe { BCryptHashData(self.handle, data, 0) }.is_ok() {
            Ok(())
        } else {
            Err(Error::fixed("Could not hash update payload"))
        }
    }

    pub fn finish(&mut self) -> [u8; 32] {
        let mut digest = [0u8; 32];
        unsafe {
            let _ = BCryptFinishHash(self.handle, &mut digest, 0);
        }
        digest
    }
}

/// SHA-256 of a byte slice.
pub fn sha256(data: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new().expect("CNG SHA-256 provider");
    hasher.update(data).expect("CNG SHA-256 update");
    hasher.finish()
}

/// SHA-256 of a file, hex encoded.
pub fn sha256_file(path: &Path) -> Result<String> {
    let mut file =
        std::fs::File::open(path).map_err(|_| Error::fixed("Could not read update payload"))?;
    let mut hasher = Sha256::new()?;
    let mut buffer = [0u8; 65536];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|_| Error::fixed("Could not hash update payload"))?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count])?;
    }
    Ok(hex(&hasher.finish()))
}

fn rsa_key_blob(exponent: &[u8], modulus: &[u8]) -> Vec<u8> {
    let header = BCRYPT_RSAKEY_BLOB {
        Magic: windows::Win32::Security::Cryptography::BCRYPT_RSAPUBLIC_MAGIC,
        BitLength: 3072,
        cbPublicExp: exponent.len() as u32,
        cbModulus: modulus.len() as u32,
        cbPrime1: 0,
        cbPrime2: 0,
    };
    let mut blob = Vec::with_capacity(
        std::mem::size_of::<BCRYPT_RSAKEY_BLOB>() + exponent.len() + modulus.len(),
    );
    for field in [
        header.Magic.0,
        header.BitLength,
        header.cbPublicExp,
        header.cbModulus,
        header.cbPrime1,
        header.cbPrime2,
    ] {
        blob.extend_from_slice(&field.to_le_bytes());
    }
    blob.extend_from_slice(exponent);
    blob.extend_from_slice(modulus);
    blob
}

fn pss_padding() -> BCRYPT_PSS_PADDING_INFO {
    BCRYPT_PSS_PADDING_INFO {
        pszAlgId: BCRYPT_SHA256_ALLOCATION.as_pcwstr(),
        cbSalt: 32,
    }
}

// `pszAlgId` needs a wide algorithm name; keep it as a byte-string constant so
// no runtime conversion is required.
struct WideAlgorithmName(&'static [u16]);

impl WideAlgorithmName {
    const fn as_pcwstr(&self) -> windows::core::PCWSTR {
        windows::core::PCWSTR(self.0.as_ptr())
    }
}

const BCRYPT_SHA256_ALLOCATION: WideAlgorithmName =
    WideAlgorithmName(&[0x53, 0x48, 0x41, 0x32, 0x35, 0x36, 0]); // "SHA256"

/// Verifies an RSA-3072/PSS/SHA-256 signature (32-byte salt) over `payload`
/// with the given big-endian modulus and exponent.
pub fn verify_rsa_pss_sha256(
    payload: &[u8],
    signature: &[u8],
    modulus: &[u8],
    exponent: &[u8],
) -> Result<bool> {
    crate::errors::require(
        modulus.len() == 384 && !exponent.is_empty() && exponent.len() <= 8,
        "Invalid release public key",
    )?;
    let blob = rsa_key_blob(exponent, modulus);
    let digest = sha256(payload);
    unsafe {
        let mut algorithm = Default::default();
        if !BCryptOpenAlgorithmProvider(
            &mut algorithm,
            BCRYPT_RSA_ALGORITHM,
            None,
            NO_PROVIDER_FLAGS,
        )
        .is_ok()
        {
            return Ok(false);
        }
        let mut key = Default::default();
        let imported =
            BCryptImportKeyPair(algorithm, None, BCRYPT_RSAPUBLIC_BLOB, &mut key, &blob, 0);
        if !imported.is_ok() {
            let _ = BCryptCloseAlgorithmProvider(algorithm, 0);
            return Ok(false);
        }
        let padding = pss_padding();
        let verified = BCryptVerifySignature(
            key,
            Some(&padding as *const BCRYPT_PSS_PADDING_INFO as *const _),
            &digest,
            signature,
            BCRYPT_PAD_PSS,
        );
        let _ = BCryptDestroyKey(key);
        let _ = BCryptCloseAlgorithmProvider(algorithm, 0);
        Ok(verified.is_ok())
    }
}

#[cfg(any(test, feature = "signing"))]
pub mod signer {
    //! In-process RSA-3072 test signer used by fixtures and cross-validation
    //! tests. Never compiled into shipped binaries.
    use super::*;
    use base64::Engine;
    use windows::Win32::Security::Cryptography::{
        BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, BCryptExportKey, BCryptFinalizeKeyPair,
        BCryptGenerateKeyPair, BCryptSignHash,
    };

    pub struct TestSigner {
        algorithm: BCRYPT_ALG_HANDLE,
        key: BCRYPT_KEY_HANDLE,
        pub public_keys_json: String,
    }

    impl Default for TestSigner {
        fn default() -> Self {
            Self::new()
        }
    }

    impl TestSigner {
        pub fn new() -> Self {
            unsafe {
                let mut algorithm = Default::default();
                let mut key = Default::default();
                assert!(
                    BCryptOpenAlgorithmProvider(
                        &mut algorithm,
                        BCRYPT_RSA_ALGORITHM,
                        None,
                        NO_PROVIDER_FLAGS
                    )
                    .is_ok()
                        && BCryptGenerateKeyPair(algorithm, &mut key, 3072, 0).is_ok()
                        && BCryptFinalizeKeyPair(key, 0).is_ok(),
                    "generate isolated test signing key"
                );
                let mut length = 0u32;
                assert!(
                    BCryptExportKey(key, None, BCRYPT_RSAPUBLIC_BLOB, None, &mut length, 0).is_ok(),
                    "measure public key"
                );
                let mut bytes = vec![0u8; length as usize];
                assert!(
                    BCryptExportKey(
                        key,
                        None,
                        BCRYPT_RSAPUBLIC_BLOB,
                        Some(&mut bytes),
                        &mut length,
                        0
                    )
                    .is_ok(),
                    "export public key"
                );
                let header_size = std::mem::size_of::<BCRYPT_RSAKEY_BLOB>();
                let cb_exponent = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
                let cb_modulus = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
                let exponent = &bytes[header_size..header_size + cb_exponent];
                let modulus =
                    &bytes[header_size + cb_exponent..header_size + cb_exponent + cb_modulus];
                let public_keys_json = serde_json::json!({
                    "keys": [{
                        "id": "test",
                        "modulus": base64_encode(modulus),
                        "exponent": base64_encode(exponent),
                    }]
                })
                .to_string();
                TestSigner {
                    algorithm,
                    key,
                    public_keys_json,
                }
            }
        }

        pub fn sign(&self, payload: &[u8]) -> Vec<u8> {
            unsafe {
                let digest = sha256(payload);
                let padding = pss_padding();
                let mut signature = vec![0u8; 384];
                let mut length = 0u32;
                assert!(
                    BCryptSignHash(
                        self.key,
                        Some(&padding as *const BCRYPT_PSS_PADDING_INFO as *const _),
                        &digest,
                        Some(&mut signature),
                        &mut length,
                        BCRYPT_PAD_PSS,
                    )
                    .is_ok(),
                    "sign fixture release"
                );
                signature
            }
        }

        /// Builds the signed envelope JSON for a payload value.
        pub fn envelope(&self, payload: &serde_json::Value) -> Vec<u8> {
            let payload_bytes = serde_json::to_vec(payload).expect("serialize payload");
            let signature = self.sign(&payload_bytes);
            serde_json::to_vec(&serde_json::json!({
                "schema": 1,
                "keyId": "test",
                "payload": base64_encode(&payload_bytes),
                "signature": base64_encode(&signature),
            }))
            .expect("serialize envelope")
        }
    }

    impl Drop for TestSigner {
        fn drop(&mut self) {
            unsafe {
                let _ = BCryptDestroyKey(self.key);
                let _ = BCryptCloseAlgorithmProvider(self.algorithm, 0);
            }
        }
    }

    pub fn base64_encode(data: &[u8]) -> String {
        base64::engine::general_purpose::STANDARD.encode(data)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_matches_known_vectors() {
        assert_eq!(
            hex(&sha256(b"abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(
            hex(&sha256(b"")),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
    }

    #[test]
    fn signer_and_verifier_round_trip() {
        let signer = signer::TestSigner::new();
        let payload = b"signed payload";
        let signature = signer.sign(payload);
        let keys: serde_json::Value = serde_json::from_str(&signer.public_keys_json).unwrap();
        let key = &keys["keys"][0];
        let modulus = crate::contract::decode_base64(key["modulus"].as_str().unwrap()).unwrap();
        let exponent = crate::contract::decode_base64(key["exponent"].as_str().unwrap()).unwrap();
        assert!(verify_rsa_pss_sha256(payload, &signature, &modulus, &exponent).unwrap());
        assert!(!verify_rsa_pss_sha256(b"tampered", &signature, &modulus, &exponent).unwrap());
    }
}
