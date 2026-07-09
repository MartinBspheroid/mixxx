#include <gtest/gtest.h>

#include "companion/httpconnection.h"

using mixxx::companion::HttpConnection;
using mixxx::companion::HttpRequest;

namespace {

TEST(CompanionHttpParserTest, ParsesSimpleGet) {
    HttpRequest request;
    ASSERT_TRUE(HttpConnection::parseRequest(
            "GET /v1/status HTTP/1.1\r\nHost: x\r\n\r\n", &request));
    EXPECT_EQ(request.method, QByteArray("GET"));
    EXPECT_EQ(request.path, QStringLiteral("/v1/status"));
    EXPECT_TRUE(request.body.isEmpty());
}

TEST(CompanionHttpParserTest, ParsesQueryAndPercentEncodedPath) {
    HttpRequest request;
    ASSERT_TRUE(HttpConnection::parseRequest(
            "GET /v1/library/search?q=bic%20ep&limit=5 HTTP/1.1\r\n\r\n",
            &request));
    EXPECT_EQ(request.path, QStringLiteral("/v1/library/search"));
    EXPECT_EQ(request.query.queryItemValue(QStringLiteral("q")),
            QStringLiteral("bic ep"));
    EXPECT_EQ(request.query.queryItemValue(QStringLiteral("limit")),
            QStringLiteral("5"));
}

TEST(CompanionHttpParserTest, HeadersAreCaseInsensitive) {
    HttpRequest request;
    ASSERT_TRUE(HttpConnection::parseRequest(
            "GET / HTTP/1.1\r\nAuThOrIzAtIoN: Bearer abc123\r\n\r\n", &request));
    EXPECT_EQ(request.header("authorization"), QByteArray("Bearer abc123"));
    EXPECT_EQ(request.bearerToken(), QByteArray("abc123"));
}

TEST(CompanionHttpParserTest, CredentialFallbackOrder) {
    // Authorization header wins; then ?code=; then ?token=.
    HttpRequest withCode;
    ASSERT_TRUE(HttpConnection::parseRequest(
            "GET /v1/status?code=123456 HTTP/1.1\r\n\r\n", &withCode));
    EXPECT_EQ(withCode.bearerToken(), QByteArray("123456"));

    HttpRequest withToken;
    ASSERT_TRUE(HttpConnection::parseRequest(
            "GET /v1/status?token=deadbeef HTTP/1.1\r\n\r\n", &withToken));
    EXPECT_EQ(withToken.bearerToken(), QByteArray("deadbeef"));
}

TEST(CompanionHttpParserTest, ParsesPostBody) {
    HttpRequest request;
    ASSERT_TRUE(HttpConnection::parseRequest(
            "POST /v1/decks/1/load HTTP/1.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 16\r\n\r\n"
            "{\"trackId\":1234}",
            &request));
    EXPECT_EQ(request.method, QByteArray("POST"));
    EXPECT_EQ(request.body, QByteArray("{\"trackId\":1234}"));
}

TEST(CompanionHttpParserTest, LowercaseMethodIsNormalized) {
    HttpRequest request;
    ASSERT_TRUE(HttpConnection::parseRequest(
            "post / HTTP/1.1\r\n\r\n", &request));
    EXPECT_EQ(request.method, QByteArray("POST"));
}

TEST(CompanionHttpParserTest, RejectsGarbage) {
    HttpRequest request;
    EXPECT_FALSE(HttpConnection::parseRequest("garbage", &request));
    EXPECT_FALSE(HttpConnection::parseRequest("", &request));
    EXPECT_FALSE(HttpConnection::parseRequest("GET\r\n\r\n", &request));
    EXPECT_FALSE(HttpConnection::parseRequest("GET /\r\n\r\n", &request));
}

TEST(CompanionHttpParserTest, MalformedHeaderLinesAreSkipped) {
    HttpRequest request;
    ASSERT_TRUE(HttpConnection::parseRequest(
            "GET / HTTP/1.1\r\nno-colon-line\r\nX-Ok: yes\r\n\r\n", &request));
    EXPECT_EQ(request.header("x-ok"), QByteArray("yes"));
}

} // namespace
