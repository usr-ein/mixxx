#include <gtest/gtest.h>

#include <QByteArray>

#include "network/prolink/rpc/rpcmessage.h"

namespace {

// XdrWriter/XdrReader are in mixxx::prolink; the RPC types are one level deeper.
using namespace mixxx::prolink;
using namespace mixxx::prolink::rpc;

QByteArray hex(const char* text) {
    return QByteArray::fromHex(QByteArray(text));
}

/// A real 76-byte portmap GETPORT call, captured from a CDJ-2000NXS on firmware
/// 1.44 asking our own server which port its mountd was on
/// (`prolinks-compat/captures/S24b-e9-control/run.pcap`).
///
/// Pinned as literal bytes because building a call correctly is the difference
/// between a working mount and an unanswered request, and because a golden
/// vector from the hardware we must interoperate with is worth more than one we
/// invented ourselves.
const char* kRealGetPortCall =
        "00000001"  // xid
        "00000000"  // msg_type = CALL
        "00000002"  // rpcvers = 2
        "000186a0"  // program = 100000, portmap
        "00000002"  // version = 2
        "00000003"  // procedure = 3, GETPORT
        "00000001"  // credential flavor = AUTH_UNIX
        "00000014"  //   body length = 20
        "967b8703"  //   stamp
        "00000000"  //   machine_name = "" (length only)
        "00000000"  //   uid = 0
        "00000000"  //   gid = 0
        "00000000"  //   supplementary gid count = 0
        "00000000"  // verifier flavor = AUTH_NULL
        "00000000"  //   body length = 0
        "000186a5"  // arg: program = 100005, mountd
        "00000001"  // arg: version = 1
        "00000011"  // arg: protocol = 17, UDP
        "00000000"; // arg: port = 0 (means "tell me")

TEST(ProLinkRpcTest, BuildsACallByteIdenticalToARealCdjs) {
    AuthUnix credentials;
    credentials.stamp = 0x967b8703;
    // machineName stays empty, uid and gid stay 0 -- what a player sends.

    XdrWriter args;
    args.u32(kMountProgram);
    args.u32(kMountVersion);
    args.u32(kIpProtoUdp);
    args.u32(0);

    const QByteArray call = buildCall(1,
            kPortmapProgram,
            kPortmapVersion,
            static_cast<quint32>(PortmapProc::GetPort),
            credentials,
            args.data());

    EXPECT_EQ(hex(kRealGetPortCall), call);
    EXPECT_EQ(76, call.size());
}

TEST(ProLinkRpcTest, StampsDifferPerCall) {
    // Documentation described the AUTH_UNIX stamp as a fixed magic constant.
    // 7917 distinct stamps across ~8500 real calls in our capture corpus say
    // otherwise (C8), so we generate a nonce per call. Two draws being equal is
    // possible but vanishingly unlikely; three in a row would mean the
    // generator is not generating.
    const quint32 a = randomStamp();
    const quint32 b = randomStamp();
    const quint32 c = randomStamp();
    EXPECT_FALSE(a == b && b == c);
}

TEST(ProLinkRpcTest, ParsesAnAcceptedReply) {
    XdrWriter reply;
    reply.u32(42);                                            // xid
    reply.u32(static_cast<quint32>(MsgType::Reply));
    reply.u32(static_cast<quint32>(ReplyStat::Accepted));
    reply.u32(static_cast<quint32>(AuthFlavor::Null));        // verifier
    reply.opaqueVar(QByteArray());
    reply.u32(static_cast<quint32>(AcceptStat::Success));
    reply.u32(48276);                                         // the mountd port

    Reply parsed;
    ASSERT_TRUE(parseReply(reply.data(), &parsed));
    EXPECT_EQ(42u, parsed.xid);
    EXPECT_TRUE(parsed.isOk());
    EXPECT_TRUE(parsed.errorString().isEmpty());

    // The results body is handed on untouched for the per-program decoder.
    XdrReader results(parsed.results);
    EXPECT_EQ(48276u, results.u32());
    EXPECT_TRUE(results.isValid());
}

TEST(ProLinkRpcTest, ReportsProcUnavailWithoutTreatingItAsMalformed) {
    // A player does not implement every NFSv2 procedure -- libcdj hit exactly
    // this on READDIR. It is a valid answer, so parsing must succeed while
    // isOk() is false; conflating the two would turn "this device cannot do
    // that" into "this device is broken".
    XdrWriter reply;
    reply.u32(7);
    reply.u32(static_cast<quint32>(MsgType::Reply));
    reply.u32(static_cast<quint32>(ReplyStat::Accepted));
    reply.u32(static_cast<quint32>(AuthFlavor::Null));
    reply.opaqueVar(QByteArray());
    reply.u32(static_cast<quint32>(AcceptStat::ProcUnavail));

    Reply parsed;
    ASSERT_TRUE(parseReply(reply.data(), &parsed));
    EXPECT_FALSE(parsed.isOk());
    EXPECT_EQ(QStringLiteral("PROC_UNAVAIL"), parsed.errorString());
}

TEST(ProLinkRpcTest, ProgMismatchCarriesTheSupportedRange) {
    // Distinguishes "wrong version" from "not there at all" -- one is our bug,
    // the other is a device that cannot do what we asked.
    XdrWriter reply;
    reply.u32(9);
    reply.u32(static_cast<quint32>(MsgType::Reply));
    reply.u32(static_cast<quint32>(ReplyStat::Accepted));
    reply.u32(static_cast<quint32>(AuthFlavor::Null));
    reply.opaqueVar(QByteArray());
    reply.u32(static_cast<quint32>(AcceptStat::ProgMismatch));
    reply.u32(2);
    reply.u32(3);

    Reply parsed;
    ASSERT_TRUE(parseReply(reply.data(), &parsed));
    EXPECT_FALSE(parsed.isOk());
    EXPECT_EQ(2u, parsed.lowVersion);
    EXPECT_EQ(3u, parsed.highVersion);
    EXPECT_TRUE(parsed.errorString().contains(QStringLiteral("v2..v3")));
}

TEST(ProLinkRpcTest, ParsesADeniedReply) {
    XdrWriter reply;
    reply.u32(11);
    reply.u32(static_cast<quint32>(MsgType::Reply));
    reply.u32(static_cast<quint32>(ReplyStat::Denied));
    reply.u32(static_cast<quint32>(RejectStat::AuthError));

    Reply parsed;
    ASSERT_TRUE(parseReply(reply.data(), &parsed));
    EXPECT_FALSE(parsed.accepted);
    EXPECT_EQ(QStringLiteral("AUTH_ERROR"), parsed.errorString());
}

TEST(ProLinkRpcTest, RejectsThingsThatAreNotReplies) {
    Reply parsed;
    // Our own call, echoed back at us: well-formed RPC, but msg_type is CALL.
    EXPECT_FALSE(parseReply(hex(kRealGetPortCall), &parsed));
    // Truncated mid-header.
    EXPECT_FALSE(parseReply(hex("0000002a000000"), &parsed));
    // Empty.
    EXPECT_FALSE(parseReply(QByteArray(), &parsed));
    // A DJ-Link keep-alive that arrived on the wrong socket.
    EXPECT_FALSE(parseReply(QByteArrayLiteral("Qspt1WmJOL\x06\x00"), &parsed));
}

} // namespace
