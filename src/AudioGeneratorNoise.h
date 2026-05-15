// AudioGeneratorNoise.h
// A minimal AudioGenerator subclass that produces white noise (static).
// Plug it into AudioOutputI2S just like AudioGeneratorMP3.
#pragma once

#include <AudioGenerator.h>

class AudioGeneratorNoise : public AudioGenerator {
public:
  AudioGeneratorNoise(float amplitude = 0.12f)
      : amplitude(amplitude) {}

  bool begin(AudioFileSource * /*src*/, AudioOutput *out) override {
    this->output = out;
    // 11025 Hz mono is plenty for noise and saves CPU/bandwidth
    output->SetRate(11025);
    output->SetBitsPerSample(16);
    output->SetChannels(2);
    if (!output->begin()) return false;
    running = true;
    return true;
  }

  bool loop() override {
    if (!running) return false;

    // Fill a small burst of samples each call so DMA buffers stay fed
    for (int i = 0; i < 64; i++) {
      int16_t s = (int16_t)((esp_random() & 0xFFFF) - 32768);
      // Scale amplitude: amplitude 1.0 == full scale, 0.12 == quiet static
      s = (int16_t)(s * amplitude);
      int16_t buf[2] = {s, s};
      if (!output->ConsumeSample(buf)) break; // DMA full, stop early
    }
    return running;
  }

  bool stop() override {
    running = false;
    output->stop();
    return true;
  }

  bool isRunning() override { return running; }

private:
  AudioOutput *output = nullptr;
  float amplitude;
};
