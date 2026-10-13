#include "homestr/models/smart_speaker.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace homestr {

std::string_view toString(PlaybackState state) {
  switch (state) {
    case PlaybackState::Playing:
      return "PLAYING";
    case PlaybackState::Paused:
      return "PAUSED";
    case PlaybackState::Stopped:
      return "STOPPED";
  }
  return "STOPPED";
}

SmartSpeaker::SmartSpeaker(std::string id, std::string name, std::string ipAddress)
    : SmartDevice(std::move(id), std::move(name), std::move(ipAddress)) {}

bool SmartSpeaker::connect() {
  if (!reachable()) {
    return false;
  }
  connected_ = true;
  return true;
}

void SmartSpeaker::disconnect() {
  connected_ = false;
  playbackState_ = PlaybackState::Stopped;
}

bool SmartSpeaker::play(std::string_view track) {
  if (!reachable() || !connected_) {
    return false;
  }
  if (!track.empty()) {
    currentTrack_.assign(track);
  }
  playbackState_ = PlaybackState::Playing;
  return true;
}

bool SmartSpeaker::pause() {
  if (!reachable() || !connected_) {
    return false;
  }
  // Pausing a stopped speaker is a no-op rather than an error.
  if (playbackState_ == PlaybackState::Playing) {
    playbackState_ = PlaybackState::Paused;
  }
  return true;
}

bool SmartSpeaker::setVolume(int level) {
  if (!reachable() || !connected_) {
    return false;
  }
  volume_ = std::clamp(level, 0, 100);
  return true;
}

bool SmartSpeaker::turnOff() {
  if (!SmartDevice::turnOff()) {
    return false;
  }
  disconnect();
  return true;
}

nlohmann::json SmartSpeaker::toJson() const {
  nlohmann::json payload = SmartDevice::toJson();
  payload["connected"] = connected_;
  payload["volume"] = volume_;
  payload["playbackState"] = toString(playbackState_);
  payload["currentTrack"] = currentTrack_;
  return payload;
}

}  // namespace homestr
