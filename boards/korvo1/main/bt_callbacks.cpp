// The Bluetooth stack's callbacks: GAP for discovery and pairing, A2DP for the link and the media
// stream, AVRCP for the headset's buttons. Each one reads its parameters and posts an event; the
// link task decides. They run on the Bluetooth task, so nothing here blocks.

#include "bt_shared.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace player::bt_link {
namespace {

std::string_view eir_name(std::uint8_t* eir) {
  std::uint8_t length = 0;
  auto* name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
  if (!name) name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
  return name ? std::string_view{reinterpret_cast<const char*>(name), length} : std::string_view{};
}

std::string_view property_name(const esp_bt_gap_dev_prop_t& prop) {
  if (prop.type == ESP_BT_GAP_DEV_PROP_EIR) return eir_name(static_cast<std::uint8_t*>(prop.val));
  if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME)
    return {static_cast<const char*>(prop.val), static_cast<std::size_t>(prop.len)};
  return {};
}

bool is_target(const esp_bt_gap_cb_param_t::disc_res_param& found) {
  return std::any_of(found.prop, found.prop + found.num_prop, [](const auto& prop) {
    return property_name(prop).find(kTargetName) != std::string_view::npos;
  });
}

Event found_event(const esp_bt_gap_cb_param_t::disc_res_param& found) {
  auto out = event(Event::Kind::found);
  std::memcpy(out.address, found.bda, ESP_BD_ADDR_LEN);
  for (int i = 0; i < found.num_prop; ++i) {
    const auto name = property_name(found.prop[i]);
    if (name.empty()) continue;
    name.copy(out.name, std::min(name.size(), kNameBytes - 1));
    break;
  }
  return out;
}

void post_connection(const esp_a2d_cb_param_t::a2d_conn_stat_param& connection) {
  if (connection.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    auto e = event(Event::Kind::connected);
    e.handle = connection.conn_hdl;
    post(e);
  } else if (connection.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    post(event(Event::Kind::disconnected));
  }
}

void post_audio_state(esp_a2d_audio_state_t state) {
  post(event(state == ESP_A2D_AUDIO_STATE_STARTED ? Event::Kind::audio_started
                                                  : Event::Kind::audio_suspended));
}

void post_ack(const esp_a2d_cb_param_t::media_ctrl_stat_param& ack) {
  auto e = event(Event::Kind::ack);
  e.command = ack.cmd;
  e.status = ack.status;
  post(e);
}

void on_passthrough(std::uint8_t key_code, std::uint8_t key_state) {
  if (key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) return;
  Button button = Button::PlayPause;
  switch (key_code) {
    case ESP_AVRC_PT_CMD_PLAY:
    case ESP_AVRC_PT_CMD_PAUSE:
    case ESP_AVRC_PT_CMD_STOP: break;
    case ESP_AVRC_PT_CMD_FORWARD: button = Button::Next; break;
    case ESP_AVRC_PT_CMD_BACKWARD: button = Button::Previous; break;
    default: return;
  }
  std::function<void(Button)> handler;
  {
    const std::lock_guard lock(shared.text_mutex);
    handler = shared.remote;
  }
  if (handler) handler(button);
}

}  // namespace

void gap_callback(esp_bt_gap_cb_event_t event_id, esp_bt_gap_cb_param_t* param) {
  switch (event_id) {
    case ESP_BT_GAP_DISC_RES_EVT:
      if (is_target(param->disc_res)) post(found_event(param->disc_res));
      break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED)
        post(event(Event::Kind::discovery_stopped));
      break;
    case ESP_BT_GAP_CFM_REQ_EVT: esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true); break;
    default: break;
  }
}

void a2d_callback(esp_a2d_cb_event_t event_id, esp_a2d_cb_param_t* param) {
  switch (event_id) {
    case ESP_A2D_CONNECTION_STATE_EVT: post_connection(param->conn_stat); break;
    case ESP_A2D_AUDIO_STATE_EVT: post_audio_state(param->audio_stat.state); break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: post_ack(param->media_ctrl_stat); break;
    default: break;
  }
}

void avrc_tg_callback(esp_avrc_tg_cb_event_t event_id, esp_avrc_tg_cb_param_t* param) {
  if (event_id == ESP_AVRC_TG_PASSTHROUGH_CMD_EVT)
    on_passthrough(param->psth_cmd.key_code, param->psth_cmd.key_state);
}

// We send nothing over AVRCP yet (the volume scales the samples), but the profile must be up for
// the headset to send its own buttons.
void avrc_ct_callback(esp_avrc_ct_cb_event_t, esp_avrc_ct_cb_param_t*) {}

}  // namespace player::bt_link
