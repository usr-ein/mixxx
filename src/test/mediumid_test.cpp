#include "library/deck/mediumid.h"

#include <gtest/gtest.h>

namespace {

using mixxx::deck::MediumId;

// The keys land in SQL and in the track cache's file names, so their spelling
// is part of what a medium is: pinned here as well as read back.

TEST(MediumIdTest, LocalKeyNamesTheMountAndTheVolume) {
    const MediumId id = MediumId::local(
            QStringLiteral("/media/usb0"), QStringLiteral("1234-ABCD"));
    EXPECT_EQ(QStringLiteral("usb:/media/usb0#uuid:1234-ABCD"), id.key());
    EXPECT_TRUE(id.isLocal());
    EXPECT_EQ(QStringLiteral("/media/usb0"), id.mountPoint());
    EXPECT_EQ(QStringLiteral("1234-ABCD"), id.volumeId());
    EXPECT_TRUE(id.deviceKey().isEmpty());
    EXPECT_EQ(0, id.slot());
}

TEST(MediumIdTest, LocalKeyWithoutAVolumeIsTheMountAlone) {
    const MediumId id = MediumId::local(QStringLiteral("/media/usb1"));
    EXPECT_EQ(QStringLiteral("usb:/media/usb1"), id.key());
    EXPECT_EQ(QStringLiteral("/media/usb1"), id.mountPoint());
    EXPECT_TRUE(id.volumeId().isEmpty());
}

TEST(MediumIdTest, RemoteKeyGivesBackItsPlayerAndSlot) {
    const MediumId id = MediumId::proLink(QStringLiteral("c8d3ffa1b2c3"), 3);
    EXPECT_EQ(QStringLiteral("prolink:c8d3ffa1b2c3|3"), id.key());
    EXPECT_FALSE(id.isLocal());
    EXPECT_EQ(QStringLiteral("c8d3ffa1b2c3"), id.deviceKey());
    EXPECT_EQ(3, id.slot());
    EXPECT_TRUE(id.mountPoint().isEmpty());
    EXPECT_TRUE(id.volumeId().isEmpty());
}

TEST(MediumIdTest, KeysSurviveTheDatabase) {
    const MediumId local = MediumId::local(
            QStringLiteral("/media/usb0"), QStringLiteral("1234-ABCD"));
    const MediumId remote = MediumId::proLink(QStringLiteral("c8d3ffa1b2c3"), 2);
    EXPECT_EQ(local, MediumId::fromKey(local.key()));
    EXPECT_TRUE(MediumId::fromKey(local.key()).isLocal());
    EXPECT_EQ(remote, MediumId::fromKey(remote.key()));
    EXPECT_EQ(QStringLiteral("c8d3ffa1b2c3"), MediumId::fromKey(remote.key()).deviceKey());
    EXPECT_EQ(2, MediumId::fromKey(remote.key()).slot());
}

} // namespace
