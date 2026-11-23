#pragma once

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace player {

// The esp_pthread fields the board sets for a thread it creates. `priority` and `name` are left to
// the creating thread unless set: esp_pthread copies the whole config, so a thread inherits the
// priority the caller runs at.
struct ThreadConfig {
  std::size_t stack = 0;
  int core = 0;
  std::uint32_t caps = 0;
  std::optional<int> priority;
  const char* name = nullptr;
};

// Creates a thread with `spawn` under `config`, then puts the creating thread's own config back:
// esp_pthread keeps the config per thread, and the threads created here later would inherit it.
// The stack's memory comes from `caps`: PSRAM when the task only reads RAM (ESP-IDF's rule for
// PSRAM stacks: no flash access, see api-guides/external-ram), internal RAM for the ones that
// cannot do without.
template <class Spawn>
auto spawn_with(const ThreadConfig& config, Spawn&& spawn) {
  esp_pthread_cfg_t previous{};
  if (esp_pthread_get_cfg(&previous) != ESP_OK) previous = esp_pthread_get_default_config();
  esp_pthread_cfg_t cfg = previous;
  cfg.stack_size = static_cast<std::uint32_t>(config.stack);
  cfg.pin_to_core = config.core;
  cfg.stack_alloc_caps = config.caps;
  if (config.priority) cfg.prio = *config.priority;
  if (config.name) cfg.thread_name = config.name;
  if (const auto err = esp_pthread_set_cfg(&cfg); err != ESP_OK)
    std::printf("thread: esp_pthread_set_cfg failed: %s\n", esp_err_to_name(err));
  auto created = spawn();
  esp_pthread_set_cfg(&previous);
  return created;
}

}  // namespace player
