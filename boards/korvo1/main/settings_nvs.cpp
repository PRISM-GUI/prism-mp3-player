#include "settings_nvs.hpp"

#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <cstdint>
#include <cstdio>

namespace player {
namespace {

constexpr const char* kNamespace = "player";
constexpr const char* kOutputKey = "output";
constexpr OutputChoice kFallback = OutputChoice::bluetooth;

void log_failure(const char* call, esp_err_t err) {
  if (err != ESP_OK) std::printf("settings: %s failed: %s\n", call, esp_err_to_name(err));
}

}  // namespace

bool init_nvs() {
  auto err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    log_failure("nvs_flash_erase", nvs_flash_erase());
    err = nvs_flash_init();
  }
  log_failure("nvs_flash_init", err);
  return err == ESP_OK;
}

OutputChoice load_output_choice() {
  if (!init_nvs()) return kFallback;
  nvs_handle_t handle{};
  if (const auto err = nvs_open(kNamespace, NVS_READONLY, &handle); err != ESP_OK) {
    log_failure("nvs_open", err);
    return kFallback;
  }
  std::uint8_t stored = 0;
  const auto err = nvs_get_u8(handle, kOutputKey, &stored);
  nvs_close(handle);
  if (err != ESP_OK) {
    log_failure("nvs_get_u8", err);  // ESP_ERR_NVS_NOT_FOUND on a board that never chose
    return kFallback;
  }
  return stored == static_cast<std::uint8_t>(OutputChoice::speaker) ? OutputChoice::speaker
                                                                    : kFallback;
}

void save_output_choice(OutputChoice choice) {
  if (!init_nvs()) return;
  nvs_handle_t handle{};
  if (const auto err = nvs_open(kNamespace, NVS_READWRITE, &handle); err != ESP_OK) {
    log_failure("nvs_open", err);
    return;
  }
  if (const auto err = nvs_set_u8(handle, kOutputKey, static_cast<std::uint8_t>(choice));
      err != ESP_OK) {
    log_failure("nvs_set_u8", err);
  } else if (const auto err = nvs_commit(handle); err != ESP_OK) {
    log_failure("nvs_commit", err);
  }
  nvs_close(handle);
}

}  // namespace player
