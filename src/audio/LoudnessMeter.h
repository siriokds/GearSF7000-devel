// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef GEARSF7000_LOUDNESS_METER_H
#define GEARSF7000_LOUDNESS_METER_H

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

// Perceived loudness per ITU-R BS.1770-4 / EBU R128 (Tech 3341, 3342).
//
// Peak and RMS answer "how close to clipping" and "how much energy", not
// "how loud does it sound". BS.1770 is what broadcast and streaming use for
// the latter: a K-weighting filter (high shelf around 1.7 kHz, high pass
// around 38 Hz, approximating the head's acoustic response) followed by a
// mean square over 400 ms blocks, and for the integrated figure a two-stage
// gate so pauses and quiet passages do not drag the result down. Two games
// with the same LUFS sound equally loud to a listener; two with the same
// peak can be 10 dB apart.
//
// Header-only so it needs no entry in any of the build lists. Fed one stereo
// frame at a time, normalized to full scale = 1.0. Not thread-safe: it lives
// on the emulation thread like Audio::PeakInfo, which the GUI reads the same way.
class LoudnessMeter
{
public:
    static constexpr double kSilence = -std::numeric_limits<double>::infinity();

    void Reset(double sampleRate)
    {
        m_sampleRate = sampleRate;
        DesignKWeighting(sampleRate);
        m_state = {};
        m_subBlockLength = static_cast<int>(sampleRate / 10.0 + 0.5);
        m_subBlockFill = 0;
        m_subBlockEnergy = 0.0;
        m_ring = {};
        m_ringCount = 0;
        m_ringPos = 0;
        m_momentaryBlocks.clear();
        m_shortTermBlocks.clear();
        m_momentary = kSilence;
        m_shortTerm = kSilence;
        m_momentaryMax = kSilence;
        m_shortTermMax = kSilence;
        m_integrated = kSilence;
        m_range = 0.0;
        m_peak = 0.0;
        m_frames = 0;
    }

    void Process(float left, float right)
    {
        // Sample peak of the unweighted input, so a loudness figure can be
        // read next to how close the same passage came to full scale.
        m_peak = std::max(m_peak, static_cast<double>(
            std::max(std::fabs(left), std::fabs(right))));
        const double l = Filter(0, left);
        const double r = Filter(1, right);
        // BS.1770 channel weights are 1.0 for left and right: the energies
        // add, so dual mono reads 3 dB above the same signal on one channel.
        m_subBlockEnergy += l * l + r * r;
        ++m_frames;
        if (++m_subBlockFill >= m_subBlockLength)
            CloseSubBlock();
    }

    double GetMomentary() const { return m_momentary; }
    double GetShortTerm() const { return m_shortTerm; }
    double GetMomentaryMax() const { return m_momentaryMax; }
    double GetShortTermMax() const { return m_shortTermMax; }
    double GetIntegrated() const { return m_integrated; }
    double GetRange() const { return m_range; }
    // dBFS, full scale = 1.0.
    double GetPeakDbfs() const
    {
        return m_peak > 0.0 ? 20.0 * std::log10(m_peak) : kSilence;
    }
    double GetSeconds() const
    {
        return m_sampleRate > 0.0 ? m_frames / m_sampleRate : 0.0;
    }

private:
    struct Biquad
    {
        double b0, b1, b2, a1, a2;
    };

    // Coefficients from the analog prototype rather than the 48 kHz table in
    // the standard, so the filter stays correct at any output rate. Same
    // derivation as libebur128; at 48 kHz it reproduces the published table.
    void DesignKWeighting(double rate)
    {
        const double pi = 3.14159265358979323846;
        double f0 = 1681.974450955533;
        const double g = 3.999843853973347;
        double q = 0.7071752369554196;
        double k = std::tan(pi * f0 / rate);
        const double vh = std::pow(10.0, g / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        double a0 = 1.0 + k / q + k * k;
        m_shelf = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0,
                    (vh - vb * k / q + k * k) / a0, 2.0 * (k * k - 1.0) / a0,
                    (1.0 - k / q + k * k) / a0 };

        f0 = 38.13547087602444;
        q = 0.5003270373238773;
        k = std::tan(pi * f0 / rate);
        a0 = 1.0 + k / q + k * k;
        m_highPass = { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0,
                       (1.0 - k / q + k * k) / a0 };
    }

    static double RunBiquad(const Biquad& f, double* z, double x)
    {
        // Transposed direct form II.
        const double y = f.b0 * x + z[0];
        z[0] = f.b1 * x - f.a1 * y + z[1];
        z[1] = f.b2 * x - f.a2 * y;
        return y;
    }

    double Filter(int channel, double x)
    {
        double* z = m_state[channel].data();
        return RunBiquad(m_highPass, z + 2, RunBiquad(m_shelf, z, x));
    }

    static double ToLufs(double meanSquare)
    {
        return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10(meanSquare) : kSilence;
    }

    // 100 ms sub-blocks give the 75% overlap of the 400 ms momentary blocks
    // BS.1770 gating specifies, and the 10 Hz update rate Tech 3342 asks of
    // the short-term series used for loudness range.
    void CloseSubBlock()
    {
        m_ring[m_ringPos] = m_subBlockEnergy / m_subBlockFill;
        m_ringPos = (m_ringPos + 1) % kRingSize;
        m_ringCount = std::min(m_ringCount + 1, kRingSize);
        m_subBlockEnergy = 0.0;
        m_subBlockFill = 0;

        if (m_ringCount >= 4)
        {
            const double energy = Average(4);
            m_momentary = ToLufs(energy);
            m_momentaryMax = std::max(m_momentaryMax, m_momentary);
            m_momentaryBlocks.push_back(energy);
            UpdateIntegrated();
        }
        if (m_ringCount >= kRingSize)
        {
            m_shortTerm = ToLufs(Average(kRingSize));
            m_shortTermMax = std::max(m_shortTermMax, m_shortTerm);
            m_shortTermBlocks.push_back(m_shortTerm);
            // Percentiles need a partial sort; once a second is plenty for
            // a figure that describes minutes of material.
            if (m_shortTermBlocks.size() % 10 == 1)
                UpdateRange();
        }
    }

    double Average(int blocks) const
    {
        double sum = 0.0;
        for (int i = 1; i <= blocks; ++i)
            sum += m_ring[(m_ringPos - i + kRingSize) % kRingSize];
        return sum / blocks;
    }

    // Absolute gate at -70 LUFS drops silence; the relative gate 10 LU below
    // the loudness of what survived drops quiet passages, so a title screen
    // pause does not make a game measure quieter than it sounds.
    void UpdateIntegrated()
    {
        const double absoluteGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
        double sum = 0.0;
        size_t count = 0;
        for (double e : m_momentaryBlocks)
            if (e > absoluteGate) { sum += e; ++count; }
        if (count == 0)
        {
            m_integrated = kSilence;
            return;
        }
        const double relativeGate = sum / count * std::pow(10.0, -10.0 / 10.0);
        sum = 0.0;
        count = 0;
        for (double e : m_momentaryBlocks)
            if (e > absoluteGate && e > relativeGate) { sum += e; ++count; }
        m_integrated = count ? ToLufs(sum / count) : kSilence;
    }

    // Loudness range (Tech 3342): spread between the 10th and 95th
    // percentile of gated short-term loudness, relative gate at -20 LU.
    void UpdateRange()
    {
        std::vector<double> gated;
        double sum = 0.0;
        for (double lufs : m_shortTermBlocks)
            if (lufs > -70.0)
            {
                gated.push_back(lufs);
                sum += std::pow(10.0, (lufs + 0.691) / 10.0);
            }
        if (gated.empty())
        {
            m_range = 0.0;
            return;
        }
        const double relativeGate = ToLufs(sum / gated.size()) - 20.0;
        gated.erase(std::remove_if(gated.begin(), gated.end(),
            [relativeGate](double lufs) { return lufs <= relativeGate; }),
            gated.end());
        if (gated.size() < 2)
        {
            m_range = 0.0;
            return;
        }
        const size_t last = gated.size() - 1;
        const size_t lo = static_cast<size_t>(std::lround(0.10 * last));
        const size_t hi = static_cast<size_t>(std::lround(0.95 * last));
        std::nth_element(gated.begin(), gated.begin() + lo, gated.end());
        const double low = gated[lo];
        std::nth_element(gated.begin(), gated.begin() + hi, gated.end());
        m_range = gated[hi] - low;
    }

    static constexpr int kRingSize = 30;

    double m_sampleRate = 0.0;
    Biquad m_shelf{};
    Biquad m_highPass{};
    std::array<std::array<double, 4>, 2> m_state{};
    int m_subBlockLength = 1;
    int m_subBlockFill = 0;
    double m_subBlockEnergy = 0.0;
    std::array<double, kRingSize> m_ring{};
    int m_ringCount = 0;
    int m_ringPos = 0;
    // One entry per 100 ms: about 36k doubles for an hour of play.
    std::vector<double> m_momentaryBlocks;
    std::vector<double> m_shortTermBlocks;
    double m_momentary = kSilence;
    double m_shortTerm = kSilence;
    double m_momentaryMax = kSilence;
    double m_shortTermMax = kSilence;
    double m_integrated = kSilence;
    double m_range = 0.0;
    double m_peak = 0.0;
    unsigned long long m_frames = 0;
};

#endif
