#pragma once
#include "core/audio.hpp"
#include "core/wheel.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace player {

// The headset as the player's output: A2DP source, 44.1 kHz stereo, with AVRCP for the headset's
// own buttons. One instance, brought up once at boot; until a headset connects it is simply not
// connected, so the player plays nothing.
// The methods run on the logic thread. The data callback runs on the Bluetooth task and touches
// nothing but the ring and two atomics (core/audio.hpp has the contract).
class BtOutput final : public PcmOutput {
public:
  // Powers Bluetooth up and connects to the bonded headset, or scans for CONFIG_PLAYER_BT_NAME.
  static BtOutput& open();

  bool accepts(PcmFormat format) const override;
  bool start(PcmFormat format, PcmRing& ring) override;
  void stop() override;
  bool connected() const override;
  std::uint32_t underruns() const override;
  void set_volume(float volume) override;

  // Play, pause, next and previous from the headset. Called on the Bluetooth task: post it to the
  // logic thread, never touch the UI from here.
  void on_remote(std::function<void(Button)> handler);

  // "Off", "Scanning…", "Connecting…", "Connected to <name>": the Settings screen's value.
  std::string state_text() const;

  // A number that changes when state_text() would say something else: compare it instead of
  // building the string (and taking a lock) every frame.
  std::uint32_t state_revision() const;
};

}  // namespace player
