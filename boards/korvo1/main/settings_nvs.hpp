#pragma once

namespace player {

// What the board plays through, kept between boots (NVS namespace "player", key "output").
enum class OutputChoice { bluetooth, speaker };

// The stored choice, or bluetooth when nothing is stored or NVS fails (both are logged).
OutputChoice load_output_choice();
void save_output_choice(OutputChoice choice);

}  // namespace player
