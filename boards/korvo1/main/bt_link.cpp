#include "bt_link.hpp"

#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <cstdio>

namespace player::bt_link {

bool bonded_address(esp_bd_addr_t out) {
  if (esp_bt_gap_get_bond_device_num() <= 0) return false;
  esp_bd_addr_t bonded[kBondedList]{};
  int listed = kBondedList;  // in: how many fit, out: how many came back
  if (esp_bt_gap_get_bond_device_list(&listed, bonded) != ESP_OK || listed <= 0) return false;
  std::memcpy(out, bonded[0], ESP_BD_ADDR_LEN);
  return true;
}

void remember_name(const char* name) {
  if (name[0] == '\0') return;
  const std::lock_guard lock(shared.text_mutex);
  shared.name = name;
  shared.revision.fetch_add(1);
}

TickType_t ticks_until(std::optional<std::int64_t> deadline) {
  if (!deadline) return portMAX_DELAY;
  const auto left_us = *deadline - now_us();
  if (left_us <= 0) return 0;
  return pdMS_TO_TICKS(left_us / 1000) + 1;
}

void run_link_task(void*) {
  LinkManager link;
  link.begin();
  for (;;) {
    Event e;
    if (xQueueReceive(events, &e, ticks_until(link.next_deadline())) == pdTRUE) link.handle(e);
    link.fire_due(now_us());
  }
}

// PSRAM for the stack, as the logic thread's: the task calls the Bluetooth API, which only posts
// to the Bluetooth task and reads RAM, never flash (ESP-IDF's rule for PSRAM stacks: no code that
// disables the cache, see api-guides/external-ram).
void start_link_task() {
  TaskHandle_t task = nullptr;
  if (xTaskCreatePinnedToCoreWithCaps(run_link_task, "bt link", kLinkTaskStack, nullptr,
                                      kLinkTaskPriority, &task, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    std::printf("bt: cannot start the link task\n");
}

}  // namespace player::bt_link
