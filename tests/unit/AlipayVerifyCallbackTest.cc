#include <drogon/drogon_test.h>
#include "channels/AlipayChannel.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
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

    KeyFiles() = default;

    // RAII: free the key and remove the temp PEMs even when a REQUIRE aborts the
    // test early (Drogon's REQUIRE returns on failure), so no leak survives.
    ~KeyFiles()
    {
        if (pkey)
        {
            EVP_PKEY_free(pkey);
        }
        std::error_code ec;
        if (!privatePath.empty())
        {
            std::filesystem::remove(privatePath, ec);
        }
        if (!publicPath.empty())
        {
            std::filesystem::remove(publicPath, ec);
        }
    }

    KeyFiles(const KeyFiles &) = delete;
    KeyFiles &operator=(const KeyFiles &) = delete;

    KeyFiles(KeyFiles &&other) noexcept
        : privatePath(std::move(other.privatePath)),
          publicPath(std::move(other.publicPath)),
          pkey(other.pkey)
    {
        other.pkey = nullptr;
    }

    KeyFiles &operator=(KeyFiles &&other) noexcept
    {
        if (this != &other)
        {
            if (pkey)
            {
                EVP_PKEY_free(pkey);
            }
            privatePath = std::move(other.privatePath);
            publicPath = std::move(other.publicPath);
            pkey = other.pkey;
            other.pkey = nullptr;
        }
        return *this;
    }
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

// Request signing (unlike notification verification) keeps sign_type and drops
// only sign. This mirrors Alipay's gateway-request rule so a test can recompute
// the exact string the client must have signed.
std::string requestSignString(const Json::Value &params)
{
    std::vector<std::string> keys;
    for (const auto &key : params.getMemberNames())
    {
        if (key != "sign")
        {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    std::string data;
    for (const auto &key : keys)
    {
        const std::string value = params[key].asString();
        if (value.empty())
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

bool rsaVerifyBase64(EVP_PKEY *pkey, const std::string &data, const std::string &sigB64)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO *mem = BIO_new_mem_buf(sigB64.c_str(), static_cast<int>(sigB64.length()));
    b64 = BIO_push(b64, mem);
    std::vector<unsigned char> sig;
    unsigned char buf[256];
    int n;
    while ((n = BIO_read(b64, buf, sizeof(buf))) > 0)
    {
        sig.insert(sig.end(), buf, buf + n);
    }
    BIO_free_all(b64);

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestVerifyUpdate(mdctx, data.c_str(), data.length());
    int ok = EVP_DigestVerifyFinal(mdctx, sig.data(), sig.size());
    EVP_MD_CTX_free(mdctx);
    return ok == 1;
}

std::string readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Days-from-civil (Howard Hinnant) so a "yyyy-MM-dd HH:mm:ss" string can be
// interpreted as UTC portably (no timegm/_mkgmtime split). Used to pin that the
// request timestamp is GMT+8, not the host's local wall clock.
long long civilToEpoch(int y, int mo, int d, int h, int mi, int s)
{
    y -= (mo <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + static_cast<long long>(doe) - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
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

    // Discriminating control: a signature over a string-to-sign that FOLDS the
    // empty-valued parameters in covers a different byte sequence, so the
    // verifier must reject it. Paired with the positive case above this pins the
    // empty-value exclusion rule: a verifier that included empty values would
    // accept this signature and reject the genuine one.
    const std::string legacySignature =
      rsaSignBase64(kf.pkey, alipaySignString(params, /*dropEmptyValues=*/false));
    CHECK(client.verifyCallback(params, legacySignature) == false);

    // Sanity: a tampered signature must still be rejected.
    CHECK(client.verifyCallback(params, "AAAAAAAAAAAAAAAA") == false);
}

// =============================================================================
// Gateway request signing: notify_url is a TOP-LEVEL signed public parameter
// (H2), sign_type participates, version/charset are present, the timestamp is
// GMT+8, and the obsolete nonce is gone.
// =============================================================================
DROGON_TEST(AlipayRequestSigning_NotifyUrlTopLevelAndSigned)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);

    Json::Value config;
    config["app_id"] = "2021000000000000";
    config["private_key_path"] = kf.privatePath;
    config["alipay_public_key_path"] = kf.publicPath;
    config["gateway_url"] = "https://openapi.alipaydev.com/gateway.do";
    config["notify_url"] = "https://merchant.example.com/api/pay/notify/alipay";

    AlipaySandboxClient client(config);

    Json::Value biz;
    biz["out_trade_no"] = "ORDER-QR-1";
    biz["total_amount"] = "88.88";
    biz["subject"] = "QR order";

    const Json::Value params = client.buildSignedRequestParams("alipay.trade.precreate", biz);

    // notify_url lives at the top level, never inside biz_content.
    REQUIRE(params.isMember("notify_url"));
    CHECK(params["notify_url"].asString() == config["notify_url"].asString());
    CHECK(params["biz_content"].asString().find("notify_url") == std::string::npos);

    // Mandatory public parameters present; obsolete nonce removed.
    CHECK(params["version"].asString() == "1.0");
    CHECK(params["charset"].asString() == "utf-8");
    CHECK(params["sign_type"].asString() == "RSA2");
    CHECK(params["method"].asString() == "alipay.trade.precreate");
    CHECK(!params.isMember("nonce"));

    // Timestamp is "yyyy-MM-dd HH:mm:ss" and reads as GMT+8 (within a few seconds
    // of now+8h), not the host's local zone.
    const std::string ts = params["timestamp"].asString();
    REQUIRE(ts.size() == 19);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    REQUIRE(std::sscanf(ts.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6);
    const long long produced = civilToEpoch(y, mo, d, h, mi, s);
    const long long expected = static_cast<long long>(std::time(nullptr)) + 8 * 3600;
    CHECK(std::llabs(produced - expected) <= 5);

    // The signature covers the full request string-to-sign (sign_type included,
    // only sign dropped, empty values excluded).
    REQUIRE(params.isMember("sign"));
    const std::string sign = params["sign"].asString();
    CHECK(rsaVerifyBase64(kf.pkey, requestSignString(params), sign) == true);

    // Discriminating controls: the signature must bind notify_url and sign_type.
    // Dropping either from the recomputed string changes the bytes, so the same
    // signature must NOT verify — proving they really participate.
    Json::Value noNotify = params;
    noNotify.removeMember("notify_url");
    CHECK(rsaVerifyBase64(kf.pkey, requestSignString(noNotify), sign) == false);

    Json::Value noSignType = params;
    noSignType.removeMember("sign_type");
    CHECK(rsaVerifyBase64(kf.pkey, requestSignString(noSignType), sign) == false);

    // Tampering with the amount invalidates the signature.
    Json::Value tampered = params;
    tampered["biz_content"] =
      std::string("{\"out_trade_no\":\"ORDER-QR-1\",\"total_amount\":\"0.01\"}");
    CHECK(rsaVerifyBase64(kf.pkey, requestSignString(tampered), sign) == false);
}

// =============================================================================
// Synchronous-response node name mapping (M3): '.' -> '_' + "_response".
// =============================================================================
DROGON_TEST(AlipayResponseKeyForMethod_Mapping)
{
    CHECK(
      AlipaySandboxClient::responseKeyForMethod("alipay.trade.create") ==
      "alipay_trade_create_response"
    );
    CHECK(
      AlipaySandboxClient::responseKeyForMethod("alipay.trade.precreate") ==
      "alipay_trade_precreate_response"
    );
    CHECK(
      AlipaySandboxClient::responseKeyForMethod("alipay.trade.query") ==
      "alipay_trade_query_response"
    );
    CHECK(
      AlipaySandboxClient::responseKeyForMethod("alipay.trade.fastpay.refund.query") ==
      "alipay_trade_fastpay_refund_query_response"
    );
}

// =============================================================================
// Synchronous-response signature verification (M1, fail-closed): the exact raw
// JSON node bytes are signed; a missing/invalid sign or node is rejected.
// =============================================================================
DROGON_TEST(AlipayVerifyResponseSignature_ValidTamperedMissing)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);
    const std::string pubPem = readFile(kf.publicPath);
    REQUIRE(!pubPem.empty());

    const std::string key = "alipay_trade_create_response";
    // Exact node bytes Alipay would sign (compact, includes a nested object to
    // exercise the brace-depth scanner).
    const std::string node =
      "{\"code\":\"10000\",\"msg\":\"Success\",\"out_trade_no\":\"ORDER-1\","
      "\"trade_no\":\"20260919220010000005\","
      "\"fund_bill_list\":[{\"amount\":\"88.88\",\"fund_channel\":\"ALIPAYACCOUNT\"}]}";
    const std::string sign = rsaSignBase64(kf.pkey, node);
    const std::string rawBody = "{\"" + key + "\":" + node + ",\"sign\":\"" + sign + "\"}";

    // Positive: the genuine signed response verifies.
    CHECK(AlipaySandboxClient::verifyResponseSignature(rawBody, key, pubPem) == true);

    // Negative: a single tampered digit in the node breaks the signature.
    std::string tampered = rawBody;
    const size_t amtPos = tampered.find("88.88");
    REQUIRE(amtPos != std::string::npos);
    tampered[amtPos] = '0';  // 88.88 -> 08.88
    CHECK(AlipaySandboxClient::verifyResponseSignature(tampered, key, pubPem) == false);

    // Negative: no sign member at all (fail-closed, not "nothing to check").
    const std::string noSign = "{\"" + key + "\":" + node + "}";
    CHECK(AlipaySandboxClient::verifyResponseSignature(noSign, key, pubPem) == false);

    // Negative: the requested node is absent (wrong key) -> rejected, no fallback.
    CHECK(
      AlipaySandboxClient::verifyResponseSignature(
        rawBody, "alipay_trade_query_response", pubPem
      ) == false
    );

    // Negative: empty public key -> rejected.
    CHECK(AlipaySandboxClient::verifyResponseSignature(rawBody, key, "") == false);

    // Negative: sign present but not a string -> fail-closed (false), not a throw.
    const std::string objSign = "{\"" + key + "\":" + node + ",\"sign\":{}}";
    CHECK(AlipaySandboxClient::verifyResponseSignature(objSign, key, pubPem) == false);

    // Discriminating control: Alipay signs the EXACT raw bytes, not a
    // re-serialization. A signature over a pretty-printed equivalent node must
    // NOT verify against the compact node embedded in the body.
    const std::string prettyNode =
      "{\n  \"code\" : \"10000\",\n  \"msg\" : \"Success\",\n  \"out_trade_no\" : \"ORDER-1\"\n}";
    const std::string prettySign = rsaSignBase64(kf.pkey, prettyNode);
    const std::string mismatchBody =
      "{\"" + key + "\":" + prettyNode + ",\"sign\":\"" + prettySign + "\"}";
    // prettyNode IS the embedded node here, so this verifies...
    CHECK(AlipaySandboxClient::verifyResponseSignature(mismatchBody, key, pubPem) == true);
    // ...but swapping in the compact node while keeping the pretty signature fails.
    const std::string swappedBody =
      "{\"" + key + "\":" + node + ",\"sign\":\"" + prettySign + "\"}";
    CHECK(AlipaySandboxClient::verifyResponseSignature(swappedBody, key, pubPem) == false);
}

// =============================================================================
// Duplicate-key splice (security): a genuine signed node followed by a second
// top-level member with the SAME key must be rejected. extractResponseNode takes
// the FIRST raw occurrence while jsoncpp (rejectDupKeys=false) delivers the LAST
// duplicate, so without a hard reject an attacker could verify a pending node
// yet deliver a TRADE_SUCCESS node and credit an unpaid order.
// =============================================================================
DROGON_TEST(AlipayVerifyResponseSignature_DuplicateKeyRejected)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);
    const std::string pubPem = readFile(kf.publicPath);
    REQUIRE(!pubPem.empty());

    const std::string key = "alipay_trade_query_response";
    const std::string genuine =
      "{\"code\":\"10000\",\"trade_status\":\"WAIT_BUYER_PAY\",\"total_amount\":\"88.88\"}";
    const std::string sign = rsaSignBase64(kf.pkey, genuine);
    const std::string attacker =
      "{\"code\":\"10000\",\"trade_status\":\"TRADE_SUCCESS\",\"total_amount\":\"88.88\"}";

    // Genuine-first, attacker-second, signature covers only the genuine node.
    const std::string dupBody = "{\"" + key + "\":" + genuine + ",\"" + key + "\":" + attacker +
                                ",\"sign\":\"" + sign + "\"}";
    CHECK(AlipaySandboxClient::verifyResponseSignature(dupBody, key, pubPem) == false);

    // Sanity: the same genuine node WITHOUT the duplicate still verifies, so the
    // rejection above is specifically the duplicate key, not a broken fixture.
    const std::string okBody = "{\"" + key + "\":" + genuine + ",\"sign\":\"" + sign + "\"}";
    CHECK(AlipaySandboxClient::verifyResponseSignature(okBody, key, pubPem) == true);
}

// =============================================================================
// Nested-decoy splice (security): rejectDupKeys only rejects duplicates at the
// SAME object level. A genuine node nested under a sibling key plus a forged
// TOP-LEVEL node of the same name parses cleanly (different nesting levels).
// extractResponseNode must anchor the key search to the root object: matching
// the first raw occurrence anywhere would verify the genuine nested node while
// jsoncpp delivers the forged top-level one — the same verified!=delivered
// bypass as the duplicate-key case, one level deeper.
// =============================================================================
DROGON_TEST(AlipayVerifyResponseSignature_NestedDecoyRejected)
{
    KeyFiles kf = makeTempKeypair();
    REQUIRE(kf.pkey);
    const std::string pubPem = readFile(kf.publicPath);
    REQUIRE(!pubPem.empty());

    const std::string key = "alipay_trade_query_response";
    const std::string genuine =
      "{\"code\":\"10000\",\"trade_status\":\"WAIT_BUYER_PAY\",\"total_amount\":\"88.88\"}";
    const std::string sign = rsaSignBase64(kf.pkey, genuine);
    const std::string attacker =
      "{\"code\":\"10000\",\"trade_status\":\"TRADE_SUCCESS\",\"total_amount\":\"88.88\"}";

    // Genuine node nested under "decoy" (the first raw occurrence of the key),
    // forged node at the root (what jsoncpp delivers). The signature covers only
    // the genuine node, so a root-anchored extractor must reject this.
    const std::string nestedBody = "{\"decoy\":{\"" + key + "\":" + genuine + "},\"" + key +
                                   "\":" + attacker + ",\"sign\":\"" + sign + "\"}";
    CHECK(AlipaySandboxClient::verifyResponseSignature(nestedBody, key, pubPem) == false);

    // Positive control: the genuine node at the ROOT still verifies, so the
    // rejection above is the nesting anchor, not a broken fixture.
    const std::string okBody = "{\"" + key + "\":" + genuine + ",\"sign\":\"" + sign + "\"}";
    CHECK(AlipaySandboxClient::verifyResponseSignature(okBody, key, pubPem) == true);

    // A legitimate node whose own value contains a nested object repeating the
    // key name must still verify: the anchor restricts the KEY search to root
    // depth, not the interior of the node being extracted.
    const std::string innerDup =
      "{\"code\":\"10000\",\"trade_status\":\"TRADE_SUCCESS\",\"detail\":{\"" + key +
      "\":" + genuine + "}}";
    const std::string innerSign = rsaSignBase64(kf.pkey, innerDup);
    const std::string innerBody =
      "{\"" + key + "\":" + innerDup + ",\"sign\":\"" + innerSign + "\"}";
    CHECK(AlipaySandboxClient::verifyResponseSignature(innerBody, key, pubPem) == true);
}
