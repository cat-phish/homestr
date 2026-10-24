#include "homestr/models/smart_speaker.hpp"

#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace homestr {
namespace {

std::unique_ptr<SmartSpeaker> makeSpeaker(DeviceStatus status = DeviceStatus::Online) {
  auto speaker = std::make_unique<SmartSpeaker>("speaker-1", "Kitchen Echo", "192.168.1.30");
  speaker->setStatus(status);
  return speaker;
}

std::unique_ptr<SmartSpeaker> makeConnectedSpeaker() {
  auto speaker = makeSpeaker();
  speaker->connect();
  return speaker;
}

TEST(PlaybackStateTest, ToStringUsesTheDatabaseEnumSpelling) {
  EXPECT_EQ(toString(PlaybackState::Playing), "PLAYING");
  EXPECT_EQ(toString(PlaybackState::Paused), "PAUSED");
  EXPECT_EQ(toString(PlaybackState::Stopped), "STOPPED");
}

TEST(SmartSpeakerTest, DefaultsToDisconnectedStoppedAtModerateVolume) {
  const SmartSpeaker speaker{"speaker-1", "Kitchen Echo", "192.168.1.30"};

  EXPECT_EQ(speaker.type(), "speaker");
  EXPECT_FALSE(speaker.connected());
  EXPECT_EQ(speaker.volume(), 30);
  EXPECT_EQ(speaker.playbackState(), PlaybackState::Stopped);
  EXPECT_TRUE(speaker.currentTrack().empty());
}

TEST(SmartSpeakerTest, ConnectRequiresAnOnlineDevice) {
  const auto offline = makeSpeaker(DeviceStatus::Offline);
  const auto errored = makeSpeaker(DeviceStatus::Error);

  EXPECT_FALSE(offline->connect());
  EXPECT_FALSE(offline->connected());
  EXPECT_FALSE(errored->connect());
  EXPECT_FALSE(errored->connected());
}

TEST(SmartSpeakerTest, ConnectSucceedsWhileOnline) {
  const auto speaker = makeSpeaker();

  EXPECT_TRUE(speaker->connect());
  EXPECT_TRUE(speaker->connected());
}

TEST(SmartSpeakerTest, TransportCommandsRequireAConnection) {
  const auto speaker = makeSpeaker();

  EXPECT_FALSE(speaker->play("song"));
  EXPECT_FALSE(speaker->pause());
  EXPECT_FALSE(speaker->setVolume(50));
  EXPECT_EQ(speaker->playbackState(), PlaybackState::Stopped);
  EXPECT_EQ(speaker->volume(), 30);
  EXPECT_TRUE(speaker->currentTrack().empty());
}

TEST(SmartSpeakerTest, TransportCommandsFailWhenTheDeviceGoesOffline) {
  const auto speaker = makeConnectedSpeaker();
  speaker->setStatus(DeviceStatus::Offline);

  EXPECT_FALSE(speaker->play("song"));
  EXPECT_FALSE(speaker->pause());
  EXPECT_FALSE(speaker->setVolume(50));
}

TEST(SmartSpeakerTest, PlayWithTrackStartsPlaybackAndRemembersTheTrack) {
  const auto speaker = makeConnectedSpeaker();

  EXPECT_TRUE(speaker->play("Blue in Green"));

  EXPECT_EQ(speaker->playbackState(), PlaybackState::Playing);
  EXPECT_EQ(speaker->currentTrack(), "Blue in Green");
}

TEST(SmartSpeakerTest, PlayWithoutTrackResumesTheCurrentOne) {
  const auto speaker = makeConnectedSpeaker();
  ASSERT_TRUE(speaker->play("Blue in Green"));
  ASSERT_TRUE(speaker->pause());

  EXPECT_TRUE(speaker->play());

  EXPECT_EQ(speaker->playbackState(), PlaybackState::Playing);
  EXPECT_EQ(speaker->currentTrack(), "Blue in Green");
}

TEST(SmartSpeakerTest, PlayWithNewTrackReplacesTheCurrentOne) {
  const auto speaker = makeConnectedSpeaker();
  ASSERT_TRUE(speaker->play("Blue in Green"));

  ASSERT_TRUE(speaker->play("So What"));

  EXPECT_EQ(speaker->currentTrack(), "So What");
}

TEST(SmartSpeakerTest, PauseMovesPlayingToPaused) {
  const auto speaker = makeConnectedSpeaker();
  ASSERT_TRUE(speaker->play("Blue in Green"));

  EXPECT_TRUE(speaker->pause());

  EXPECT_EQ(speaker->playbackState(), PlaybackState::Paused);
}

TEST(SmartSpeakerTest, PausingAStoppedSpeakerIsASuccessfulNoOp) {
  const auto speaker = makeConnectedSpeaker();

  EXPECT_TRUE(speaker->pause());

  EXPECT_EQ(speaker->playbackState(), PlaybackState::Stopped);
}

TEST(SmartSpeakerTest, SetVolumeStoresValueInRange) {
  const auto speaker = makeConnectedSpeaker();

  EXPECT_TRUE(speaker->setVolume(65));
  EXPECT_EQ(speaker->volume(), 65);
}

TEST(SmartSpeakerTest, SetVolumeAcceptsBothBoundaries) {
  const auto speaker = makeConnectedSpeaker();

  EXPECT_TRUE(speaker->setVolume(0));
  EXPECT_EQ(speaker->volume(), 0);
  EXPECT_TRUE(speaker->setVolume(100));
  EXPECT_EQ(speaker->volume(), 100);
}

TEST(SmartSpeakerTest, SetVolumeClampsOutOfRangeValues) {
  const auto speaker = makeConnectedSpeaker();

  EXPECT_TRUE(speaker->setVolume(400));
  EXPECT_EQ(speaker->volume(), 100);
  EXPECT_TRUE(speaker->setVolume(-1));
  EXPECT_EQ(speaker->volume(), 0);
}

TEST(SmartSpeakerTest, DisconnectDropsTheConnectionAndStopsPlayback) {
  const auto speaker = makeConnectedSpeaker();
  ASSERT_TRUE(speaker->play("Blue in Green"));

  speaker->disconnect();

  EXPECT_FALSE(speaker->connected());
  EXPECT_EQ(speaker->playbackState(), PlaybackState::Stopped);
  EXPECT_FALSE(speaker->play());
}

TEST(SmartSpeakerTest, TurnOnDoesNotConnectTheSpeaker) {
  const auto speaker = makeSpeaker();

  EXPECT_TRUE(speaker->turnOn());

  EXPECT_TRUE(speaker->isOn());
  EXPECT_FALSE(speaker->connected());
}

TEST(SmartSpeakerTest, TurnOffDropsTheConnectionAndStopsPlayback) {
  const auto speaker = makeConnectedSpeaker();
  ASSERT_TRUE(speaker->turnOn());
  ASSERT_TRUE(speaker->play("Blue in Green"));

  EXPECT_TRUE(speaker->turnOff());

  EXPECT_FALSE(speaker->isOn());
  EXPECT_FALSE(speaker->connected());
  EXPECT_EQ(speaker->playbackState(), PlaybackState::Stopped);
}

TEST(SmartSpeakerTest, FailedTurnOffLeavesTheStreamAlone) {
  const auto speaker = makeConnectedSpeaker();
  ASSERT_TRUE(speaker->turnOn());
  ASSERT_TRUE(speaker->play("Blue in Green"));
  speaker->setStatus(DeviceStatus::Offline);

  EXPECT_FALSE(speaker->turnOff());

  EXPECT_TRUE(speaker->isOn());
  EXPECT_TRUE(speaker->connected());
  EXPECT_EQ(speaker->playbackState(), PlaybackState::Playing);
}

TEST(SmartSpeakerTest, ToJsonExtendsTheBasePayload) {
  const auto speaker = makeConnectedSpeaker();
  ASSERT_TRUE(speaker->turnOn());
  ASSERT_TRUE(speaker->setVolume(45));
  ASSERT_TRUE(speaker->play("Blue in Green"));

  const nlohmann::json payload = speaker->toJson();

  EXPECT_EQ(payload.at("id"), "speaker-1");
  EXPECT_EQ(payload.at("name"), "Kitchen Echo");
  EXPECT_EQ(payload.at("type"), "speaker");
  EXPECT_EQ(payload.at("status"), "ONLINE");
  EXPECT_EQ(payload.at("powerOn"), true);
  EXPECT_EQ(payload.at("connected"), true);
  EXPECT_EQ(payload.at("volume"), 45);
  EXPECT_EQ(payload.at("playbackState"), "PLAYING");
  EXPECT_EQ(payload.at("currentTrack"), "Blue in Green");
}

}  // namespace
}  // namespace homestr
