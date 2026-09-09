#ifndef SMITHY_TESTING_TLS_TEST_IDENTITY_H_
#define SMITHY_TESTING_TLS_TEST_IDENTITY_H_

// The test TLS identities every suite shares (//runtime:test_tls_identity).
// Both are self-signed and valid to 2046, so either can be handed to a client
// as its own trust anchor (ca_pem) — which is what lets a test separate
// "chain does not validate" from "chain validates but the name is wrong".
//
// Regenerate here — and only here — with the following one-line commands
// (wrapped for readability; a trailing backslash in a // comment would trip
// -Wall's -Wcomment in every including TU).
//
// The matching identity, CN=localhost with SANs for localhost and 127.0.0.1 —
// the name every test server binds and every test client dials:
//
//   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1
//     -keyout key.pem -out cert.pem -days 7300 -nodes -subj "/CN=localhost"
//     -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
//
// The mismatched identity, whose SAN deliberately covers neither 127.0.0.1
// nor localhost. A server presenting it is reached at 127.0.0.1 exactly as
// usual and its chain validates against itself, so the only thing that can
// reject it is hostname verification — the isolation the mismatch test needs:
//
//   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1
//     -keyout otherkey.pem -out othercert.pem -days 7300 -nodes
//     -subj "/CN=other.example.com"
//     -addext "subjectAltName=DNS:other.example.com"

namespace opal::testing {

inline constexpr char kTestCertificatePem[] = R"pem(-----BEGIN CERTIFICATE-----
MIIBmTCCAT+gAwIBAgIUV9JEHAQKR6U3ipSZd7B2JYm3AhYwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDcwNzE3NTUzOFoXDTQ2MDcwMjE3
NTUzOFowFDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0D
AQcDQgAE9w/RcpMxfYw3dzUYhuTpvkuuABBXioP9Wtn/XjbPAIn+cQ0nRAd79Wck
YwILgRQZdnQnNG7fasqRueFE4yTYkKNvMG0wHQYDVR0OBBYEFDc4bE/TAzWlbN5k
ssc68nJgFclfMB8GA1UdIwQYMBaAFDc4bE/TAzWlbN5kssc68nJgFclfMA8GA1Ud
EwEB/wQFMAMBAf8wGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMAoGCCqGSM49
BAMCA0gAMEUCIDVtF5Rhglp49Ich8hPj3aJdmejLf3TueQj4L8bnWtrvAiEAlmDl
mR4BsuAO7ZrPNIi5mCZbUTWfZwBuUgO3m/cFxsw=
-----END CERTIFICATE-----
)pem";

inline constexpr char kTestPrivateKeyPem[] = R"pem(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgk9X4X8xaMTznQYjF
b4LQYbNRZPb87gFiSZ827xahR2mhRANCAAT3D9FykzF9jDd3NRiG5Om+S64AEFeK
g/1a2f9eNs8Aif5xDSdEB3v1ZyRjAguBFBl2dCc0bt9qypG54UTjJNiQ
-----END PRIVATE KEY-----
)pem";

// CN=other.example.com, SAN DNS:other.example.com only — deliberately not
// 127.0.0.1 or localhost. Serve this to make a client's hostname check the
// sole reason a handshake fails.
inline constexpr char kMismatchedNameCertificatePem[] = R"pem(-----BEGIN CERTIFICATE-----
MIIBqzCCAVGgAwIBAgIUbJl0fsIOAS8TBRGd9n2LNSDylAMwCgYIKoZIzj0EAwIw
HDEaMBgGA1UEAwwRb3RoZXIuZXhhbXBsZS5jb20wHhcNMjYwNzMwMTM0NDMxWhcN
NDYwNzI1MTM0NDMxWjAcMRowGAYDVQQDDBFvdGhlci5leGFtcGxlLmNvbTBZMBMG
ByqGSM49AgEGCCqGSM49AwEHA0IABNpQxe4KZK6motVEh9g2DYvACr0Y3Jj/MZsR
020fCrnH4WyGaxLWf9S2t9O7dLRsmyoRZMRHmTfVVSr5dUAIeUOjcTBvMB0GA1Ud
DgQWBBS0hMqCCZmOr0mQHEW8MkeZL+UQhTAfBgNVHSMEGDAWgBS0hMqCCZmOr0mQ
HEW8MkeZL+UQhTAPBgNVHRMBAf8EBTADAQH/MBwGA1UdEQQVMBOCEW90aGVyLmV4
YW1wbGUuY29tMAoGCCqGSM49BAMCA0gAMEUCIQDHcierMoxv9nrwEfpPcMiEf1PL
oY5kmqFk+EluBwkBMgIgT7TPpaRJTxw20MF7Z2hAABdpv3IN0O3vSR7XHIy6N54=
-----END CERTIFICATE-----
)pem";

inline constexpr char kMismatchedNamePrivateKeyPem[] = R"pem(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgK/4XpCCwx255Rot/
sjQDqORDjHUlEMiiDXgz4ZtY2GKhRANCAATaUMXuCmSupqLVRIfYNg2LwAq9GNyY
/zGbEdNtHwq5x+FshmsS1n/UtrfTu3S0bJsqEWTER5k31VUq+XVACHlD
-----END PRIVATE KEY-----
)pem";

}  // namespace opal::testing

#endif  // SMITHY_TESTING_TLS_TEST_IDENTITY_H_
