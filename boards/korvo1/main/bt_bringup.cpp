// Bluetooth bring-up: the controller and Bluedroid, then the profiles the player uses, then the
// link task. Once per boot, from BtOutput::open().

#include "app_support.hpp"
#include "bt_link.hpp"
#include "settings_nvs.hpp"

#include <esp_bt.h>
#include <esp_bt_main.h>

namespace player::bt_link {
namespace {

// IDF's default-config macros are C initializers that leave fields out: C++ warns, -Werror stops.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
void start_bluedroid() {
  check("esp_bt_controller_mem_release", esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
  esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  check("esp_bt_controller_init", esp_bt_controller_init(&controller));
  check("esp_bt_controller_enable", esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
  esp_bluedroid_config_t bluedroid = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
  check("esp_bluedroid_init_with_cfg", esp_bluedroid_init_with_cfg(&bluedroid));
  check("esp_bluedroid_enable", esp_bluedroid_enable());
}
#pragma GCC diagnostic pop

void start_profiles() {
  check("esp_bt_gap_set_device_name", esp_bt_gap_set_device_name("PRISM"));
  check("esp_bt_gap_register_callback", esp_bt_gap_register_callback(gap_callback));
  check("esp_avrc_ct_init", esp_avrc_ct_init());
  check("esp_avrc_ct_register_callback", esp_avrc_ct_register_callback(avrc_ct_callback));
  check("esp_avrc_tg_init", esp_avrc_tg_init());
  check("esp_avrc_tg_register_callback", esp_avrc_tg_register_callback(avrc_tg_callback));
  check("esp_a2d_source_init", esp_a2d_source_init());
  check("esp_a2d_register_callback", esp_a2d_register_callback(a2d_callback));
  check("esp_a2d_source_register_data_callback",
        esp_a2d_source_register_data_callback(data_callback));
  // Connectable but not discoverable: a headset that knows us may come back by itself, while
  // nothing new can pair without the headset's pairing mode.
  check("esp_bt_gap_set_scan_mode",
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
}

}  // namespace

// The queue first: the callbacks may post as soon as they are registered.
void start_bluetooth() {
  events = xQueueCreate(kQueueDepth, sizeof(Event));
  player::init_nvs();
  start_bluedroid();
  korvo1::print_internal_heap("after bluedroid");
  start_profiles();
  start_link_task();  // the first attempt, now, and every retry from there
}

}  // namespace player::bt_link
