//
//  TDF SDK
//
//  Created by Sujan Reddy on 2020/04/20.
//  Copyright 2020 Virtru Corporation
//

#define BOOST_TEST_MODULE test_ec_key_pair

#include "ec_key_pair.h"
#include "crypto_utils.h"
#include "bytes.h"
#include "gcm_encryption.h"
#include "gcm_decryption.h"
#include "nanotdf/ecc_mode.h"

#include <iostream>

#include <boost/test/included/unit_test.hpp>

using namespace virtru;
using namespace virtru::crypto;

void setLogging()
{
    Logger::getInstance().enableConsoleLogging();
    Logger::getInstance().setLogLevel(virtru::LogLevel::Trace);
}
#define TESTLOOPS 50

void testNanoTDFKeyMangement(const std::string& curveName, unsigned compressedPubKeySize) {
    constexpr auto KIvSize = 3;
    constexpr auto KAuthTagSize = 8;

    const std::string kPlainText = "Virtru!!";

    // -----------------------------------------------------------------
    // (SDK-Side) Generate an EC key pair
    // -----------------------------------------------------------------
    auto sdkECKeyPair = ECKeyPair::Generate(curveName);
    auto sdkPrivateKeyForEncrypt = sdkECKeyPair->PrivateKeyInPEMFormat();
    auto sdkPublicKeyForEncrypt = sdkECKeyPair->PublicKeyInPEMFormat();

    std::cout << "SDK private key for encrypt: " << sdkPrivateKeyForEncrypt <<'\n';
    std::cout << "SDK public key for encrypt: " << sdkPublicKeyForEncrypt <<'\n';

    // -----------------------------------------------------------------
    // (KAS-Side) Generate an EC key pair
    // -----------------------------------------------------------------
    auto kasECKeyPair = ECKeyPair::Generate(curveName);
    auto kasPrivateKey = kasECKeyPair->PrivateKeyInPEMFormat();
    auto kasPublicKey = kasECKeyPair->PublicKeyInPEMFormat();

    std::cout << "kasPrivateKey: " << kasPrivateKey <<'\n';
    std::cout << "kasPublicKey: " << kasPublicKey <<'\n';

    // -----------------------------------------------------------------
    // (SDK-Side) Generate the shared secret using sdk private key, and kas public-key
    //
    // NOTE: The kas public-key will be in PEM format and we get this from
    // entity object.
    //  -----------------------------------------------------------------
    std::vector<gsl::byte> wrappedKeyOnEncrypt;
    wrappedKeyOnEncrypt = ECKeyPair::ComputeECDHKey(kasPublicKey, sdkPrivateKeyForEncrypt);
    auto base64WrappedKeyOnEncrypt = base64Encode(toBytes(wrappedKeyOnEncrypt));
    std::cout << "Base64 wrapped key on Encrypt: " << base64WrappedKeyOnEncrypt << std::endl;

    // -----------------------------------------------------------------
    // (SDK-Side) Encrypt the plain data with wrapped key(TDF blob)
    // -----------------------------------------------------------------
    auto TDFBlobSize = kPlainText.size() + KIvSize + KAuthTagSize;
    std::vector<gsl::byte> TDFBlob(TDFBlobSize);
    {
        auto encryptedData = toWriteableBytes(TDFBlob);

        ByteArray<KAuthTagSize> tag;
        ByteArray<KIvSize> iv = symmetricKey<KIvSize>();
        const auto bufferSpan = encryptedData;

        auto encryptedDataSize = 0;
        const auto final = finalizeSize(encryptedData, encryptedDataSize);

        // Adjust the span to add the IV vector at the start of the buffer
        auto encryptBufferSpan = bufferSpan.subspan(KIvSize);

        auto encoder = GCMEncryption::create(toBytes(wrappedKeyOnEncrypt), iv);
        encoder->encrypt(toBytes(kPlainText), encryptBufferSpan);

        auto authTag = WriteableBytes{tag};
        encoder->finish(authTag);

        // Copy IV at start
        std::copy(iv.begin(), iv.end(), encryptedData.begin());

        // Copy tag at end
        std::copy(tag.begin(), tag.end(), encryptedData.begin() + KIvSize + kPlainText.size());

        // Final size.
        encryptedDataSize = TDFBlobSize;

        auto base64TDFBlob = base64Encode(encryptedData);
        std::cout << "Base64 of TDFBlob of data: " << base64TDFBlob << std::endl;
    }

    // -----------------------------------------------------------------
    // (SDK-Side) Store SDK public key as (33 Bytes, compressed ) in nanoTDF for 256-bit curve
    // -----------------------------------------------------------------
    std::vector<gsl::byte> compressedPubKey = ECKeyPair::CompressedECPublicKey(sdkPublicKeyForEncrypt);
    std::cout << "Compressed public key size: " << compressedPubKey.size() << std::endl;
    BOOST_TEST(compressedPubKey.size() == compressedPubKeySize,  "Checking the compressed public key size");

    auto pemPub = ECKeyPair::GetPEMPublicKeyFromECPoint(toBytes(compressedPubKey), curveName);
    BOOST_TEST(pemPub == sdkPublicKeyForEncrypt,  "Checking the sdk public is same after compression");

    ///******************************** ENCRYPTION COMPLETED ON SDK *******************************///

    // SDK side key-pair for rewrap
    sdkECKeyPair = ECKeyPair::Generate(curveName);
    auto sdkPrivateKeyForDecrypt = sdkECKeyPair->PrivateKeyInPEMFormat();
    auto sdkPublicKeyForDecrypt = sdkECKeyPair->PublicKeyInPEMFormat();

    // -----------------------------------------------------------------
    // (KAS-Side) Rewrap
    //
    // SDK ---> 'sdkPublicKeyForDecrypt' and 'compressed 33 bytes key from TDF' ---> KAS
    //
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // (KAS-Side) KAS computing the symmetric key.
    // -----------------------------------------------------------------
    std::vector<gsl::byte> symmetricKeyOnKas;
    symmetricKeyOnKas = ECKeyPair::ComputeECDHKey(sdkPublicKeyForEncrypt, kasPrivateKey);
    auto base64SymmetricKeyOnKas = base64Encode(toBytes(symmetricKeyOnKas));
    std::cout << "Base64 symmetric key on KAS: " << base64SymmetricKeyOnKas << std::endl;
    BOOST_TEST(base64SymmetricKeyOnKas == base64WrappedKeyOnEncrypt,  "symmetric key is same on SDK and KAS");

    kasECKeyPair = ECKeyPair::Generate(curveName);
    auto kasRewrapEphemeralPrivateKey = kasECKeyPair->PrivateKeyInPEMFormat();
    auto kasRewrapEphemeralPublicKey = kasECKeyPair->PublicKeyInPEMFormat();

    // Generate a session key on KAS
    auto sessionKeyOnKAS = ECKeyPair::ComputeECDHKey(sdkPublicKeyForDecrypt, kasRewrapEphemeralPrivateKey);
    auto base64SessionKeyOnKAS = base64Encode(toBytes(sessionKeyOnKAS));
    std::cout << "Base64 session key on KAS: " << base64SessionKeyOnKAS << std::endl;

    // Encrypt symmetricKeyOnKas with sessionKey on Kas -> payload KEK
    auto payloadKeKBufferSize = symmetricKeyOnKas.size() + KIvSize + KAuthTagSize;
    std::vector<gsl::byte> payloadKeK(payloadKeKBufferSize);
    {
        auto encryptedData = toWriteableBytes(payloadKeK);

        ByteArray<KAuthTagSize> tagForKek;
        ByteArray<KIvSize> iv = symmetricKey<KIvSize>();
        const auto bufferSpan = encryptedData;

        auto encryptedDataSize = 0;
        const auto final = finalizeSize(encryptedData, encryptedDataSize);

        // Adjust the span to add the IV vector at the start of the buffer
        auto encryptBufferSpan = bufferSpan.subspan(KIvSize);

        auto encoder = GCMEncryption::create(toBytes(sessionKeyOnKAS), iv);
        encoder->encrypt(toBytes(symmetricKeyOnKas), encryptBufferSpan);
        auto authTag = WriteableBytes{tagForKek};
        encoder->finish(authTag);

        // Copy IV at start
        std::copy(iv.begin(), iv.end(), encryptedData.begin());

        // Copy tag at end
        std::copy(tagForKek.begin(), tagForKek.end(), encryptedData.begin() + KIvSize + symmetricKeyOnKas.size());

        // Final size.
        encryptedDataSize = payloadKeKBufferSize;

        auto base64PayLoadKek = base64Encode(encryptedData);
        std::cout << "Base64 of payloadKEK: " << base64PayLoadKek << std::endl;
    }

    // -----------------------------------------------------------------
    // (SDK-Side) Rewrap
    //
    // KAS ---> 'payload KEK' and 'RewrapEphemeralPublic' ---> SDK
    //
    // -----------------------------------------------------------------

    // Generate a session key on SDK.
    auto sessionKeyOnSDK = ECKeyPair::ComputeECDHKey(kasRewrapEphemeralPublicKey, sdkPrivateKeyForDecrypt);
    auto base64SessionKeyOnSDK = base64Encode(toBytes(sessionKeyOnSDK));
    std::cout << "Base64 session key on SDK: " << base64SessionKeyOnSDK << std::endl;
    BOOST_TEST(base64SessionKeyOnKAS == base64SessionKeyOnSDK,  "session key should be same on SDK and KAS");

    // ON SDK - Decrypt 'payload KEK' with sessionKey to get a symmetricKey to decrypt.
    std::vector<gsl::byte> wrappedKeyOnRewrap;
    wrappedKeyOnRewrap.resize(payloadKeK.size() - KIvSize - KAuthTagSize);
    {
        // payloadKeK is data
        auto data = toBytes(payloadKeK);

        // Copy the auth tag from the data buffer.
        ByteArray<KAuthTagSize> tag;
        std::copy_n(data.last(KAuthTagSize).data(), KAuthTagSize, begin(tag));

        // Update the input buffer size after the auth tag is copied.
        auto inputSpan = data.first(data.size() - KAuthTagSize);
        auto decoder = GCMDecryption::create(toBytes(sessionKeyOnSDK), inputSpan.first(KIvSize));

        // Update the input buffer size after the IV is copied.
        inputSpan = inputSpan.subspan(KIvSize);

        // decrypt
        auto decryptedData = toWriteableBytes(wrappedKeyOnRewrap);
        decoder->decrypt(inputSpan, decryptedData);

        auto authTag = WriteableBytes{tag};
        decoder->finish(authTag);
    }

    auto base64wrappedKeyOnRewrap = base64Encode(toBytes(wrappedKeyOnRewrap));
    std::cout << "Base64 symmetric key on KAS: " << base64wrappedKeyOnRewrap << std::endl;
    BOOST_TEST(base64WrappedKeyOnEncrypt == base64wrappedKeyOnRewrap,  "symmetric key is same on encrypt and decrypt");

    // -----------------------------------------------------------------
    // (SDK-Side) TDFBlob decrypt to get a plain text.
    // -----------------------------------------------------------------
    std::vector<gsl::byte> TDFBlobDecrypt;
    TDFBlobDecrypt.resize(kPlainText.size());
    {
        // TDFBlob is data to be decrypt
        auto data = toBytes(TDFBlob);

        // Copy the auth tag from the data buffer.
        ByteArray<KAuthTagSize> tag;
        std::copy_n(data.last(KAuthTagSize).data(), KAuthTagSize, begin(tag));

        // Update the input buffer size after the auth tag is copied.
        auto inputSpan = data.first(data.size() - KAuthTagSize);
        auto decoder = GCMDecryption::create(toBytes(wrappedKeyOnRewrap), inputSpan.first(KIvSize));

        // Update the input buffer size after the IV is copied.
        inputSpan = inputSpan.subspan(KIvSize);

        // decrypt
        auto decryptedData = toWriteableBytes(TDFBlobDecrypt);
        decoder->decrypt(inputSpan, decryptedData);

        auto authTag = WriteableBytes{tag};
        decoder->finish(authTag);
    }
    std::string decryptedTDF(reinterpret_cast<const char *>(&TDFBlobDecrypt[0]), TDFBlobDecrypt.size());
    BOOST_TEST(kPlainText == decryptedTDF,  "TDF data decrypted successfully.");

}

BOOST_AUTO_TEST_SUITE(test_ec_key_pair_suite)

    const auto sdkPrivateKey =
            "-----BEGIN PRIVATE KEY-----\n"
            "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgSEsk7e/AN08XnkZ4\n"
            "ghU+j4Rkfe/saxBdTQvZQiGlRgWhRANCAAR4tEHPS3LLUAXxea7LjxBBDTQCtS3Y\n"
            "TrBTl8I8fjBuXPKdwI2gqGiCpg/nJLaGSPxzXcUxAwmjermxtVCvovDV\n"
            "-----END PRIVATE KEY-----\n"s;

    const auto sdkPublicKey =
            "-----BEGIN PUBLIC KEY-----\n"
            "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEeLRBz0tyy1AF8Xmuy48QQQ00ArUt\n"
            "2E6wU5fCPH4wblzyncCNoKhogqYP5yS2hkj8c13FMQMJo3q5sbVQr6Lw1Q==\n"
            "-----END PUBLIC KEY-----\n"s;

    const auto kasPrivateKey =
            "-----BEGIN PRIVATE KEY-----\n"
            "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgKu8dztYhzVN3R4e2\n"
            "dcxCFiPs6bdcNPj9at0wOIgo4FahRANCAASYZ8aBk+NME4kr96NovHyuT4hShSU1\n"
            "+ZzIQExlTq3O12QStm+rXoobTRQWnS4+8Goc+RFkt/TI6/oSoTmS+O18\n"
            "-----END PRIVATE KEY-----\n"s;

    const auto kasPublicKey =
            "-----BEGIN PUBLIC KEY-----\n"
            "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEmGfGgZPjTBOJK/ejaLx8rk+IUoUl\n"
            "NfmcyEBMZU6tztdkErZvq16KG00UFp0uPvBqHPkRZLf0yOv6EqE5kvjtfA==\n"
            "-----END PUBLIC KEY-----\n"s;

    BOOST_AUTO_TEST_CASE(ec_key_pair_curve_secp521r1)
    {
        for (int i=0; i<TESTLOOPS; i++) {

            setLogging();

            const std::string curveName = "secp521r1";
            auto eckeyPair = ECKeyPair::Generate(curveName);
            auto privateKey = eckeyPair->PrivateKeyInPEMFormat();
            auto publicKey = eckeyPair->PublicKeyInPEMFormat();

            std::cout << "Private key: " << privateKey << '\n';
            std::cout << "Public key: " << publicKey << '\n';

            unsigned int secp521KeySize = 521;
            BOOST_TEST(eckeyPair->KeySize() == secp521KeySize, "Checking EC key length - key size 521 bits");
            BOOST_TEST(eckeyPair->CurveName() == curveName, "Checking the curve name - secp521r1");
        }
    }

    BOOST_AUTO_TEST_CASE(ec_key_pair_test_nano_tdf_crypto)
    {
        using namespace virtru::nanotdf;

        for (int i=0; i<TESTLOOPS; i++) {
            setLogging();

            testNanoTDFKeyMangement(ECCMode::GetEllipticCurveName(EllipticCurve::SECP256R1),
                                    ECCMode::GetECCompressedPubKeySize(EllipticCurve::SECP256R1));

            testNanoTDFKeyMangement(ECCMode::GetEllipticCurveName(EllipticCurve::SECP384R1),
                                    ECCMode::GetECCompressedPubKeySize(EllipticCurve::SECP384R1));

            testNanoTDFKeyMangement(ECCMode::GetEllipticCurveName(EllipticCurve::SECP521R1),
                                    ECCMode::GetECCompressedPubKeySize(EllipticCurve::SECP521R1));
        }
    }

    BOOST_AUTO_TEST_CASE(ec_key_pair_test_crypto_methods)
    {
        using namespace virtru::nanotdf;
        const std::string kPlainText = "Virtru!!";

        std::vector<gsl::byte> wrappedKeyOnEncrypt;
        wrappedKeyOnEncrypt = ECKeyPair::ComputeECDHKey(kasPublicKey, sdkPrivateKey);
        auto base64WrappedKeyOnEncrypt = base64Encode(toBytes(wrappedKeyOnEncrypt));
        std::cout << "Base64 symmetric key on Encrypt: " << base64WrappedKeyOnEncrypt << std::endl;

        std::string symmetricKey("wvL7rvXdLd54f6+rwmDhGL4D0TMngEaLKNuMYk9Lx6w=");
        BOOST_TEST(base64WrappedKeyOnEncrypt == symmetricKey);

        std::vector<gsl::byte> compressedPubKey = ECKeyPair::CompressedECPublicKey(sdkPublicKey);
        std::cout << "Compressed public key size: " << compressedPubKey.size() << std::endl;
        BOOST_TEST(compressedPubKey.size() == 33,  "Checking the compressed public key size");

        auto base64compressedPubKey = base64Encode(toBytes(compressedPubKey));
        std::string compressSDKPubKey("A3i0Qc9LcstQBfF5rsuPEEENNAK1LdhOsFOXwjx+MG5c");
        BOOST_TEST(compressSDKPubKey == base64compressedPubKey, "Compressed pubkey test passed");

        auto curveName = ECCMode::GetEllipticCurveName(EllipticCurve::SECP256R1);
        auto pemPub = ECKeyPair::GetPEMPublicKeyFromECPoint(toBytes(compressedPubKey), curveName);
        std::cout << "Decompressed public key: " << pemPub << std::endl;
        std::cout << "Before Decompressed public key: " << sdkPublicKey << std::endl;
        BOOST_TEST(pemPub == sdkPublicKey);
    }

    BOOST_AUTO_TEST_CASE(test_ECDSA_signature_and_verify_with_only_private_key)
    {
        using namespace virtru::nanotdf;

        const std::string kPlainText = "Virtru!!";
        auto digest = calculateSHA256(toBytes(kPlainText));

        // Add EllipticCurve::SECP521R1 once th ECDSA r and s parameter are of same length
        EllipticCurve supportedCurve[] = {EllipticCurve::SECP256R1,EllipticCurve::SECP384R1};

        for (const auto& curveType: supportedCurve) {

            auto curveName = ECCMode::GetEllipticCurveName(curveType);

            auto signerECKeyPair = ECKeyPair::Generate(curveName);
            auto signerPrivateKey = signerECKeyPair->PrivateKeyInPEMFormat();

            // Generate public key from private key
            auto publicKey = ECKeyPair::GetPEMPublicKeyFromPrivateKey(signerPrivateKey, curveName);

            // Calculate signature with signer private key
            auto signature =  ECKeyPair::ComputeECDSASig(toBytes(digest), signerPrivateKey);
            bool result = ECKeyPair::VerifyECDSASignature(toBytes(digest), toBytes(signature), publicKey);
            BOOST_TEST(result);
        }
    }

    BOOST_AUTO_TEST_CASE(test_ECDSA_signature_and_verify)
    {
        const auto expectedPubKey =
                "-----BEGIN PUBLIC KEY-----\n"
                "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEeLRBz0tyy1AF8Xmuy48QQQ00ArUt\n"
                "2E6wU5fCPH4wblzyncCNoKhogqYP5yS2hkj8c13FMQMJo3q5sbVQr6Lw1Q==\n"
                "-----END PUBLIC KEY-----\n"s;

        using namespace virtru::nanotdf;

        const std::string kPlainText = "Virtru!!";
        auto digest = calculateSHA256(toBytes(kPlainText));

        // Generate public key from private key
        auto publicKey = ECKeyPair::GetPEMPublicKeyFromPrivateKey(sdkPrivateKey,
                                                                  ECCMode::GetEllipticCurveName(EllipticCurve::SECP256R1));
        BOOST_TEST(expectedPubKey == publicKey,  "Public key from private key test passed");

        // Calculate signature with signer private key
        auto signature =  ECKeyPair::ComputeECDSASig(toBytes(digest), sdkPrivateKey);
        auto base64signature = base64Encode(toBytes(signature));
        std::cout << "signature:" << base64signature << std::endl;

        bool result = ECKeyPair::VerifyECDSASignature(toBytes(digest), toBytes(signature), publicKey);
        std::cout << "The result:" << result << std::endl;
        BOOST_TEST(result);
    }

BOOST_AUTO_TEST_SUITE_END()