#pragma once

namespace player {

// What the board plays through, kept between boots (NVS namespace "player", key "output").
enum class OutputChoice { bluetooth, speaker };

// Brings NVS up, erasing a partition written by another version. Idempotent, so the Bluetooth
// bring-up and the settings can both call it, and a boot that only uses the speaker still gets a
// usable NVS. False: NVS cannot be used (logged).
bool init_nvs();

// The stored choice, or bluetooth when nothing is stored or NVS fails (both are logged).
OutputChoice load_output_choice();
void save_output_choice(OutputChoice choice);

}  // namespace player
