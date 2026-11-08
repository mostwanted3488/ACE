#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <vector>

/*  ACE — saturation engine.
    Same maths as the browser preview:
      SOFT : tanh + a touch of even harmonics, darkening low-pass as the charge rises
      HARD : asymmetric hard clip with a soft cubic edge
      BIT  : tanh, then bit reduction (16 -> 10 bits) and sample-and-hold
    The three shapers always run and are cross-faded, so switching ammo never clicks.
    Drive: SOFT +9 dB, HARD +12 dB, BIT +15 dB at 100 %.                              */
namespace ace
{
constexpr int   numModes = 3;
constexpr float driveDb[numModes] = { 9.0f, 12.0f, 15.0f };
constexpr int   numDancers = 5;

struct Biquad
{
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    // RBJ band-pass, 0 dB peak gain
    void setBandPass (double sr, double freq, double q)
    {
        freq = std::clamp (freq, 20.0, sr * 0.45);
        const double w0 = 2.0 * 3.14159265358979323846 * freq / sr;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = (float) (alpha / a0);
        b1 = 0.0f;
        b2 = (float) (-alpha / a0);
        a1 = (float) (-2.0 * std::cos (w0) / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0.0f; }
};

struct ChannelState
{
    float lp = 0.0f;              // SOFT darkening filter
    float hold = 0.0f;            // BIT sample & hold
    int   holdCount = 0;
    float dcX1 = 0.0f, dcY1 = 0.0f;
    std::array<Biquad, numDancers> bands;

    void reset()
    {
        lp = hold = dcX1 = dcY1 = 0.0f;
        holdCount = 0;
        for (auto& b : bands) b.reset();
    }
};

class Engine
{
public:
    // ---- set from the message thread -------------------------------------------------
    std::atomic<float> targetCharge { 0.0f };   // 0..1
    std::atomic<int>   targetMode   { 1 };      // 0 SOFT, 1 HARD, 2 BIT
    std::atomic<float> targetMix    { 1.0f };   // 0..1
    std::atomic<bool>  safety       { false };
    std::array<std::atomic<float>, numDancers> bandFreq;
    std::array<std::atomic<int>,   numDancers> shootCount;

    Engine()
    {
        const float defaults[numDancers] = { 64.0f, 176.0f, 637.0f, 1532.0f, 4478.0f };
        for (int i = 0; i < numDancers; ++i)
        {
            bandFreq[(size_t) i].store (defaults[i]);
            shootCount[(size_t) i].store (0);
        }
    }

    void prepare (double newSampleRate, int numChannels)
    {
        sr = newSampleRate;
        chans.assign ((size_t) std::max (1, numChannels), ChannelState{});
        // ~7.5 ms parameter glide, ~1.5 ms for the stutter gate
        paramCoef   = 1.0f - std::exp (-1.0f / (0.0075f * (float) sr));
        stutterCoef = 1.0f - std::exp (-1.0f / (0.0015f * (float) sr));
        charge = targetCharge.load();
        mix    = safety.load() ? 0.0f : targetMix.load();
        modeW  = { 0.0f, 0.0f, 0.0f };
        modeW[(size_t) std::clamp (targetMode.load(), 0, 2)] = 1.0f;
        for (int i = 0; i < numDancers; ++i)
        {
            appliedFreq[(size_t) i] = -1.0f;
            seenShoot[(size_t) i]   = shootCount[(size_t) i].load();
            stutterPos[(size_t) i]  = -1;
            depth[(size_t) i]       = 0.0f;
        }
        updateBandFilters (true);
    }

    void reset()
    {
        for (auto& c : chans) c.reset();
    }

    // in-place processing; channels beyond the prepared count are left untouched
    void process (float* const* data, int numChannels, int numSamples) noexcept
    {
        if (chans.empty()) return;
        numChannels = std::min (numChannels, (int) chans.size());
        updateBandFilters (false);
        pollShots();

        const float tCharge = targetCharge.load();
        const float tMix    = safety.load() ? 0.0f : targetMix.load();
        const int   tMode   = std::clamp (targetMode.load(), 0, 2);

        const int stepOn  = (int) (0.040 * sr);   // gate closed 40 ms
        const int stepLen = (int) (0.070 * sr);   // every 70 ms
        const int stutterTotal = stepLen * 6;

        for (int i = 0; i < numSamples; ++i)
        {
            charge += (tCharge - charge) * paramCoef;
            mix    += (tMix - mix) * paramCoef;
            for (int m = 0; m < numModes; ++m)
                modeW[(size_t) m] += ((m == tMode ? 1.0f : 0.0f) - modeW[(size_t) m]) * paramCoef;

            // dancer stutter gates
            for (int d = 0; d < numDancers; ++d)
            {
                auto& pos = stutterPos[(size_t) d];
                float target = 0.0f;
                if (pos >= 0)
                {
                    target = (pos % stepLen) < stepOn ? 1.0f : 0.0f;
                    if (++pos >= stutterTotal) pos = -1;
                }
                depth[(size_t) d] += (target - depth[(size_t) d]) * stutterCoef;
            }

            const float k = charge;
            const float gains[numModes] = { std::pow (10.0f, driveDb[0] * k / 20.0f),
                                            std::pow (10.0f, driveDb[1] * k / 20.0f),
                                            std::pow (10.0f, driveDb[2] * k / 20.0f) };
            const float softA   = std::exp (-2.0f * 3.14159265f * (18000.0f - k * 5500.0f) / (float) sr);
            const float bits    = 16.0f - k * 6.0f;
            const float q       = std::pow (2.0f, bits - 1.0f);
            const int   holdLen = 1 + (int) std::floor (k * k * 6.0f);
            const float makeup  = 1.0f / (1.0f + k * 0.45f);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& c = chans[(size_t) ch];
                float x = data[ch][i];

                // shot dancers: cut their frequency band (x - bandpass = notch)
                for (int d = 0; d < numDancers; ++d)
                {
                    const float bp = c.bands[(size_t) d].process (x);
                    x -= bp * depth[(size_t) d];
                }

                float wet = 0.0f;

                if (modeW[0] > 1.0e-4f)
                {
                    const float u = x * gains[0];
                    float y = std::tanh (u + 0.12f * k * u * u);
                    c.lp = y + (c.lp - y) * softA;
                    wet += c.lp * modeW[0];
                }
                else
                {
                    c.lp = 0.0f;
                }

                if (modeW[1] > 1.0e-4f)
                {
                    const float u  = x * gains[1] * 1.15f;
                    const float cl = std::clamp (u, -0.82f, 0.97f);
                    wet += (cl - cl * cl * cl * 0.08f) * modeW[1];
                }

                if (modeW[2] > 1.0e-4f)
                {
                    const float u = std::tanh (x * gains[2] * 0.7f);
                    if (c.holdCount <= 0)
                    {
                        c.hold = std::round (u * q) / q;
                        c.holdCount = holdLen;
                    }
                    --c.holdCount;
                    wet += c.hold * modeW[2];
                }

                // DC blocker (the asymmetric clip and the even harmonics add offset)
                const float dc = wet - c.dcX1 + 0.995f * c.dcY1;
                c.dcX1 = wet;
                c.dcY1 = dc;
                wet = dc * makeup;

                const float out = x * (1.0f - mix) + wet * mix;
                data[ch][i] = std::clamp (out, -1.0f, 1.0f);
            }
        }
    }

private:
    void updateBandFilters (bool force)
    {
        for (int d = 0; d < numDancers; ++d)
        {
            const float f = bandFreq[(size_t) d].load();
            if (force || f != appliedFreq[(size_t) d])
            {
                appliedFreq[(size_t) d] = f;
                for (auto& c : chans)
                    c.bands[(size_t) d].setBandPass (sr, f, 1.3);
            }
        }
    }

    void pollShots()
    {
        for (int d = 0; d < numDancers; ++d)
        {
            const int n = shootCount[(size_t) d].load();
            if (n != seenShoot[(size_t) d])
            {
                seenShoot[(size_t) d] = n;
                stutterPos[(size_t) d] = 0;
            }
        }
    }

    double sr = 44100.0;
    std::vector<ChannelState> chans;
    float paramCoef = 0.003f, stutterCoef = 0.01f;
    float charge = 0.0f, mix = 1.0f;
    std::array<float, numModes> modeW { 0.0f, 1.0f, 0.0f };
    std::array<float, numDancers> appliedFreq {};
    std::array<int,   numDancers> seenShoot {};
    std::array<int,   numDancers> stutterPos {};
    std::array<float, numDancers> depth {};
};
} // namespace ace
