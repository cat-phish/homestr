#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "homestr/models/smart_device.hpp"

namespace homestr {

enum class PlaybackState { Playing, Paused, Stopped };

std::string_view toString(PlaybackState state);

// `SmartSpeaker extends SmartDevice`: adds playback control and volume.
class SmartSpeaker : public SmartDevice {
 public:
  SmartSpeaker(std::string id, std::string name, std::string ipAddress);

  std::string_view type() const noexcept override { return "speaker"; }

  bool connected() const noexcept { return connected_; }

  int volume() const noexcept { return volume_; }

  PlaybackState playbackState() const noexcept { return playbackState_; }

  const std::string& currentTrack() const noexcept { return currentTrack_; }

  // A speaker must be connected before it accepts transport commands.
  bool connect();
  void disconnect();

  bool play(std::string_view track = {});
  bool pause();
  bool setVolume(int level);

  // Turning the speaker off also drops the stream, so playback stops.
  bool turnOff() override;

  nlohmann::json toJson() const override;

 private:
  bool connected_ = false;
  int volume_ = 30;
  PlaybackState playbackState_ = PlaybackState::Stopped;
  std::string currentTrack_;
};

}  // namespace homestr
