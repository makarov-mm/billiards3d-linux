#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Short sounds made up at start: the cue striking a ball, balls clicking together, cushions, balls dropping into
// pockets or nets, and the interface. They are mixed on the audio device's thread, so that many sound at once and a
// break is heard ball by ball; without a device, the game is silent.
class SoundBank {
public:
    enum class Effect { Cue, Ball, Cushion, Pocket, Net, Rack, Click, Foul, Win, Count };

    SoundBank();
    ~SoundBank();
    SoundBank(const SoundBank&) = delete;
    SoundBank& operator=(const SoundBank&) = delete;

    // `volume` 0 to 1, `pan` from -1 (left) to 1 (right), `pitch` as a rate of playback.
    void Play(Effect effect, float volume = 1, float pan = 0, float pitch = 1);
    bool Enabled() const { return m_enabled; }
    void SetEnabled(bool enabled);

private:
    struct Voice {
        const std::vector<float>* samples = nullptr;
        double position = 0;
        double rate = 1;
        float left = 1, right = 1;
    };

    // On the audio thread, with the device locked: the voices added up into interleaved stereo.
    static void Mix(void* bank, std::uint8_t* stream, int bytes);

    std::array<std::vector<float>, static_cast<std::size_t>(Effect::Count)> m_samples;
    std::vector<Voice> m_voices; // shared with the audio thread, under the device's lock
    std::uint32_t m_device = 0;  // SDL_AudioDeviceID
    bool m_enabled = true;
};
