#include "speaker_output.hpp"

#include "core/ring_tap.hpp"
#include "thread_config.hpp"

#include <bsp/esp32_s31_korvo_1.h>
#include <esp_codec_dev.h>
#include <esp_err.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace player {
namespace {

constexpr std::size_t kFrames = 1024;  // one write: ~23 ms at 44.1 kHz
constexpr int kWriterStack = 8192;
constexpr int kMaxVolume = 100;

void log_failure(const char* call, esp_err_t err) {
  if (err != ESP_OK) std::printf("speaker: %s failed: %s\n", call, esp_err_to_name(err));
}

}  // namespace

// The writer owns the codec handle while it writes, the logic thread opens and closes it: the
// handle is an atomic pointer so both can look at it. Everything else here is the logic thread's.
struct SpeakerOutput::Impl {
  esp_codec_dev_handle_t device = nullptr;  // created once, opened and closed in place
  std::atomic<bool> opened{false};          // connected(): the player may start
  int rate = 0;                             // the open rate, 0 when closed
  std::atomic<int> volume{kMaxVolume};
  std::mutex codec_mutex;  // one write against a close-and-reopen, and against set_out_vol

  RingTap tap;  // what the writer pops from, and the handshake with the logic thread

  std::mutex mutex;  // guards the wait, not the ring
  std::condition_variable wake;
  bool quit = false;
  std::thread writer;

  // Internal RAM: the writer calls the codec driver, and the render thread needs the PSRAM for its
  // own contiguous block.
  void start_writer() {
    writer = spawn_with(
        ThreadConfig{kWriterStack, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, std::nullopt,
                     "spk writer"},
        [this] { return std::thread([this] { run(); }); });
  }

  void shutdown() {
    {
      const std::lock_guard lock(mutex);
      quit = true;
    }
    wake.notify_all();
    if (writer.joinable()) writer.join();
  }

  void attach(PcmRing& next) {
    {
      const std::lock_guard lock(mutex);
      tap.attach(next);
    }
    wake.notify_all();
  }

  // core/audio.hpp: the player clears the ring once this returns. The tap clears the pointer before
  // it looks at who is inside, which is what makes that safe; the mutex is not part of the
  // handshake, so it is not held while waiting.
  void detach() {
    tap.detach();
    wake.notify_all();
  }

  // Waits for a ring to play from. False: shutting down.
  bool wait_for_ring() {
    std::unique_lock lock(mutex);
    wake.wait(lock, [this] { return quit || tap.attached(); });
    return !quit;
  }

  void run() {
    std::vector<std::int16_t> buffer(kFrames * 2);
    const std::span<std::int16_t> samples{buffer};
    while (wait_for_ring()) {
      tap.pop_or_silence(samples);  // silence if the ring went away in between: one buffer's worth
      write(buffer);
    }
  }

  // Blocks on the I2S DMA until the codec takes the whole buffer: that is the pacing. The lock is
  // what keeps a close-and-reopen in start() from landing in the middle of the write.
  void write(std::vector<std::int16_t>& buffer) {
    const std::lock_guard lock(codec_mutex);
    if (!opened.load()) return;
    log_failure("esp_codec_dev_write",
                esp_codec_dev_write(device, buffer.data(),
                                    static_cast<int>(buffer.size() * sizeof(std::int16_t))));
  }

  // Opens the handle at `sample_rate`, closing it first when the rate changed. False: cannot play.
  bool open(int sample_rate) {
    if (opened.load() && rate == sample_rate) return true;
    const std::lock_guard lock(codec_mutex);
    if (opened.load()) {
      close_device();
      opened = false;
      rate = 0;
    }
    if (!device) return false;  // create() refused: no handle to open
    esp_codec_dev_sample_info_t info{};
    info.bits_per_sample = 16;
    info.channel = 2;
    info.channel_mask = 0;
    info.sample_rate = static_cast<std::uint32_t>(sample_rate);
    info.mclk_multiple = 0;
    if (const auto err = esp_codec_dev_open(device, &info); err != ESP_OK) {
      log_failure("esp_codec_dev_open", err);
      return false;
    }
    opened = true;
    rate = sample_rate;
    log_failure("esp_codec_dev_set_out_vol", esp_codec_dev_set_out_vol(device, volume.load()));
    return true;
  }

  void close_device() {
    if (device) log_failure("esp_codec_dev_close", esp_codec_dev_close(device));
  }

  // Once per instance, from the constructor: the player asks connected() before it asks start(),
  // so the codec exists and is open at 44.1 kHz before any track is chosen.
  bool create() {
    device = bsp_audio_codec_speaker_init();
    if (!device) {
      std::printf("speaker: bsp_audio_codec_speaker_init returned null\n");
      return false;
    }
    return open(player::kRate44k1);
  }
};

SpeakerOutput::SpeakerOutput() : impl_{std::make_unique<Impl>()} {
  impl_->create();
  impl_->start_writer();
}

SpeakerOutput::~SpeakerOutput() {
  impl_->shutdown();  // the writer is out of the ring and done before the codec closes
  const std::lock_guard lock(impl_->codec_mutex);
  impl_->close_device();
  impl_->opened = false;
  impl_->device = nullptr;
}

bool SpeakerOutput::accepts(PcmFormat format) const {
  return format.channels == 2 &&
         (format.sample_rate == player::kRate44k1 || format.sample_rate == player::kRate48k);
}

bool SpeakerOutput::start(PcmFormat format, PcmRing& ring) {
  if (!accepts(format)) return false;
  if (!impl_->open(format.sample_rate)) return false;  // reopens only when the rate changed
  impl_->attach(ring);
  return true;
}

// Never closes the codec: the next track plays through the same one, and a rate change reopens it.
void SpeakerOutput::stop() { impl_->detach(); }

bool SpeakerOutput::connected() const { return impl_->opened.load(); }

std::uint32_t SpeakerOutput::underruns() const { return impl_->tap.underruns(); }

void SpeakerOutput::set_volume(float volume) {
  impl_->volume = static_cast<int>(std::clamp(volume, 0.f, 1.f) * kMaxVolume);
  const std::lock_guard lock(impl_->codec_mutex);
  if (impl_->opened.load())
    log_failure("esp_codec_dev_set_out_vol",
                esp_codec_dev_set_out_vol(impl_->device, impl_->volume.load()));
}

}  // namespace player
