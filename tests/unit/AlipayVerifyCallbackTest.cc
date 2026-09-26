#include <drogon/drogon_test.h>
#include "channels/AlipayChannel.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Mint a throwaway RSA-2048 keypair, writing the private (PKCS#8) and public
// PEMs to temp files whose paths we hand back. The Alipay client loads keys
// from paths at construction, so this is the smallest fixture that lets us
// exercise the real sign/verify code path without any network or database.
namespace
{
struct KeyFiles
{
    std::string privatePath;
    std::string publicPath;
    EVP_PKEY *pkey = nullptr;
};

KeyFiles makeTempKeypair()
{
    KeyFiles kf;
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    if (pkey == nullptr)
    {
        return kf;
    }
    kf.pkey = pkey;

    auto dir = std::filesystem::temp_directory_path();
    // Unique per invocation: a fixed name lets a second PayBackendTests process
    // (a developer's run beside ctest) truncate the PEMs mid-test.
    static int invocation = 0;
    const std::string tag =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
      std::to_string(invocation++);
    kf.privatePath = (dir / ("alipay_test_" + tag + "_private.pem")).string();
    kf.publicPath = (dir / ("alipay_test_" + tag + "_public.pem")).string();

    {
        BIO *bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        BUF_MEM *mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        std::ofstream f(kf.privatePath, std::ios::binary);
        f.write(mem->data, static_cast<std::streamsize>(mem->length));
        BIO_free(bio);
    }
    {
        BIO *bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PUBKEY(bio, pkey);
        BUF_MEM *mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        std::ofstream f(kf.publicPath, std::ios::binary);
        f.write(mem->data, static_cast<std::streamsize>(mem->length));
        BIO_free(bio);
    }
    return kf;
}

std::string rsaSignBase64(EVP_PKEY *pkey, const std::string &data)
{
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestSignUpdate(mdctx, data.c_str(), data.length());
    size_t sigLen = 0;
    EVP_DigestSignFinal(mdctx, nullptr, &sigLen);
    std::vector<unsigned char> sig(sigLen);
    EVP_DigestSignFinal(mdctx, sig.data(), &sigLen);
    EVP_MD_CTX_free(mdctx);

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);
    BIO_write(b64, sig.data(), static_cast<int>(sigLen));
    BIO_flush(b64);
    BUF_MEM *bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string out(bptr->data, bptr->length);
    BIO_free_all(b64);
    return out;
}

// Build the Alipay string-to-sign as the open platform defines it: every
// parameter except sign and sign_type, sorted by key in ASCII order, joined as
// key=value with '&'. `dropEmptyValues` selects between the official rule
// (empty-valued parameters excluded) and the rule this verifier used to
// implement (they were included), so a test can pin which side is accepted.
std::string alipaySignString(const Json::Value &params, bool dropEmptyValues)
{
    std::vector<std::string> keys;
    for (const auto &key : params.getMemberNames())
    {
        if (key != "sign" && key != "sign_type")
        {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    std::string data;
    for (const auto &key : keys)
    {
        const std::string value = params[key].asString();
        if (dropEmptyValues && value.empty())
        {
            continue;
        }
        if (!data.empty())
        {
            data += "&";
        }
        data += key + "=" + value;
    }
    return data;
}
}  // namespace

DROGON_TEST(AlipayVerifyCallback_EmptyParamExcluded)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);

    Json::Value config;
    config["app_id"] = "2021000000000000";
    config["private_key_path"] = kf.privatePath;
    config["alipay_public_key_path"] = kf.publicPath;
    config["gateway_url"] = "https://openapi.alipaydev.com/gateway.do";

    AlipaySandboxClient client(config);

    // A realistic notification: some optional fields are present but EMPTY.
    // Alipay does not fold empty-valued parameters into its signature, so the
    // verifier must exclude them too. If it includes them, the recomputed
    // string differs and a genuine notification is rejected.
    Json::Value params;
    params["out_trade_no"] = "ORDER-123";
    params["trade_no"] = "20260919220010000005";
    params["trade_status"] = "TRADE_SUCCESS";
    params["total_amount"] = "88.88";
    params["app_id"] = "2021000000000000";
    params["sign_type"] = "RSA2";
    params["refund_amount"] = "";  // empty: must be skipped by the verifier
    params["gmt_refund"] = "";     // empty: must be skipped by the verifier

    const std::string signature =
      rsaSignBase64(kf.pkey, alipaySignString(params, /*dropEmptyValues=*/true));
    params["sign"] = signature;

    CHECK(client.verifyCallback(params, signature) == true);

    // Discriminating control: a signature over the pre-fix string-to-sign (which
    // folded the empty-valued parameters in) covers a different byte sequence, so
    // the verifier must reject it. The old implementation accepted exactly this
    // one, which is what makes the pair pin the rule rather than just "some
    // signature works".
    const std::string legacySignature =
      rsaSignBase64(kf.pkey, alipaySignString(params, /*dropEmptyValues=*/false));
    CHECK(client.verifyCallback(params, legacySignature) == false);

    // Sanity: a tampered signature must still be rejected.
    CHECK(client.verifyCallback(params, "AAAAAAAAAAAAAAAA") == false);

    EVP_PKEY_free(kf.pkey);
    std::error_code ec;
    std::filesystem::remove(kf.privatePath, ec);
    std::filesystem::remove(kf.publicPath, ec);
}
