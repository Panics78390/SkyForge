#include "Audio.h"
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace me {

static constexpr float kTwoPi = 6.28318530718f;

// ---------------------------------------------------------------------------
// Filtres biquad (formules RBJ « Audio EQ Cookbook »).
float AudioEngine::Biquad::process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

static void setBiquad(float b0, float b1, float b2, float a0, float a1, float a2, float* out) {
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0; out[3] = a1 / a0; out[4] = a2 / a0;
}

void AudioEngine::setLowpass(Biquad& f, float hz, float q) {
    const float w = kTwoPi * std::clamp(hz, 10.0f, 20000.0f) / kRate, c = std::cos(w), a = std::sin(w) / (2 * q);
    float k[5];
    setBiquad((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a, k);
    f.b0 = k[0]; f.b1 = k[1]; f.b2 = k[2]; f.a1 = k[3]; f.a2 = k[4];
}

void AudioEngine::setHighpass(Biquad& f, float hz, float q) {
    const float w = kTwoPi * std::clamp(hz, 10.0f, 20000.0f) / kRate, c = std::cos(w), a = std::sin(w) / (2 * q);
    float k[5];
    setBiquad((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + a, -2 * c, 1 - a, k);
    f.b0 = k[0]; f.b1 = k[1]; f.b2 = k[2]; f.a1 = k[3]; f.a2 = k[4];
}

void AudioEngine::setBandpass(Biquad& f, float hz, float q) {
    const float w = kTwoPi * std::clamp(hz, 10.0f, 20000.0f) / kRate, c = std::cos(w), a = std::sin(w) / (2 * q);
    float k[5];
    setBiquad(a, 0, -a, 1 + a, -2 * c, 1 - a, k);
    f.b0 = k[0]; f.b1 = k[1]; f.b2 = k[2]; f.a1 = k[3]; f.a2 = k[4];
}

float AudioEngine::white() {
    m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
    return float(m_rng) / 2147483648.0f - 1.0f;
}

// ---------------------------------------------------------------------------
// Lecture WAV (PCM 16 bits ou flottant 32 bits, mono/stéréo), rééchantillonné en stéréo 48 kHz.
std::vector<float> AudioEngine::loadWav(const std::wstring& path, int targetRate) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return {};
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) || std::memcmp(data.data() + 8, "WAVE", 4)) return {};
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const char* pcm = nullptr;
    uint32_t pcmBytes = 0;
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        uint32_t size;
        std::memcpy(&size, data.data() + pos + 4, 4);
        if (pos + 8 + size > data.size()) size = uint32_t(data.size() - pos - 8);
        if (!std::memcmp(data.data() + pos, "fmt ", 4) && size >= 16) {
            std::memcpy(&fmt, data.data() + pos + 8, 2);
            std::memcpy(&ch, data.data() + pos + 10, 2);
            std::memcpy(&rate, data.data() + pos + 12, 4);
            std::memcpy(&bits, data.data() + pos + 22, 2);
        } else if (!std::memcmp(data.data() + pos, "data", 4)) {
            pcm = data.data() + pos + 8;
            pcmBytes = size;
        }
        pos += 8 + size + (size & 1);
    }
    if (!pcm || ch < 1 || ch > 2 || rate == 0) return {};
    const bool isFloat = (fmt == 3 && bits == 32);
    const bool isPcm16 = (fmt == 1 && bits == 16);
    if (!isFloat && !isPcm16) return {};
    const uint32_t frames = pcmBytes / (ch * bits / 8);
    auto sample = [&](uint32_t frame, int c) -> float {
        const uint32_t idx = frame * ch + (ch == 2 ? c : 0);
        if (isFloat) { float v; std::memcpy(&v, pcm + idx * 4, 4); return v; }
        int16_t v; std::memcpy(&v, pcm + idx * 2, 2); return v / 32768.0f;
    };
    const double ratio = double(rate) / targetRate;
    const uint32_t outFrames = uint32_t(frames / ratio);
    std::vector<float> out(size_t(outFrames) * 2);
    for (uint32_t i = 0; i < outFrames; ++i) {
        const double src = i * ratio;
        const uint32_t i0 = std::min(uint32_t(src), frames - 1), i1 = std::min(i0 + 1, frames - 1);
        const float t = float(src - i0);
        for (int c = 0; c < 2; ++c) out[size_t(i) * 2 + c] = sample(i0, c) * (1 - t) + sample(i1, c) * t;
    }
    return out;
}

// ---------------------------------------------------------------------------
bool AudioEngine::init(const std::wstring& soundDir) {
    for (int i = 0; i < 4; ++i) {
        std::wstring p = soundDir + L"/";
        for (const char* c = kSampleNames[i]; *c; ++c) p.push_back(wchar_t(*c));
        m_samples[i] = loadWav(p, kRate);
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&m_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { m_status = "XAudio2 indisponible : pas de son"; return false; }
    if (FAILED(m_xa->CreateMasteringVoice(&m_master))) { m_status = "Aucune sortie audio trouvée"; return false; }
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8;
    wf.nAvgBytesPerSec = kRate * 8;
    if (FAILED(m_xa->CreateSourceVoice(&m_voice, &wf))) { m_status = "Impossible de créer la voix audio"; return false; }
    for (auto& b : m_buffers) b.assign(kBlock * 2, 0.0f);
    m_voice->Start();
    m_running = true;
    m_thread = std::thread(&AudioEngine::threadMain, this);
    int loaded = 0;
    for (auto& s : m_samples) loaded += s.empty() ? 0 : 1;
    m_status = loaded ? "Son actif (" + std::to_string(loaded) + " enregistrement(s) WAV chargé(s))" : "Son actif (synthèse en temps réel)";
    return true;
}

void AudioEngine::shutdown() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_voice) { m_voice->Stop(); m_voice->DestroyVoice(); m_voice = nullptr; }
    if (m_master) { m_master->DestroyVoice(); m_master = nullptr; }
    if (m_xa) { m_xa->Release(); m_xa = nullptr; }
}

void AudioEngine::setInputs(const AudioInputs& in) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_shared = in;
    m_paramsShared = params;
}

void AudioEngine::triggerThunder(float delaySeconds, float closeness) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& t : m_thunder) {
        if (!t.active) { t = {delaySeconds, 0.0f, std::clamp(closeness, 0.0f, 1.0f), true}; return; }
    }
}

void AudioEngine::threadMain() {
    while (m_running) {
        XAUDIO2_VOICE_STATE st{};
        m_voice->GetState(&st);
        if (st.BuffersQueued < kBuffers - 1) {
            std::vector<float>& buf = m_buffers[m_nextBuffer];
            render(buf.data(), kBlock);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = UINT32(buf.size() * sizeof(float));
            xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
            m_voice->SubmitSourceBuffer(&xb);
            m_nextBuffer = (m_nextBuffer + 1) % kBuffers;
        } else {
            Sleep(2);
        }
    }
}

// ---------------------------------------------------------------------------
// Synthèse d'un bloc.
void AudioEngine::render(float* out, int frames) {
    AudioParams P;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Le temps de l'océan avance à l'échantillon près entre deux frames et se recale
        // sur celui du rendu à chaque nouvelle frame.
        const float t = m_in.oceanTime;
        const bool newFrame = m_shared.oceanTime != m_lastReceivedTime;
        m_in = m_shared;
        if (newFrame) m_lastReceivedTime = m_shared.oceanTime;
        else m_in.oceanTime = t;
        P = m_paramsShared;
    }
    const AudioInputs& I = m_in;
    const float dt = 1.0f / kRate;
    if (P.mute) { std::fill(out, out + frames * 2, 0.0f); return; }

    // Niveaux cibles de chaque couche (lissés par échantillon plus bas).
    const float altitude = 1.0f / (1.0f + std::max(I.camHeight, 0.0f) / 120.0f);
    const float shoreAtt = 1.0f / (1.0f + std::pow(std::max(I.distToShore, 0.0f) / 30.0f, 1.5f));
    const float snowMuffle = 1.0f - 0.3f * I.snowFactor * I.precip;
    const float rainAmount = I.precip * (1.0f - I.snowFactor);
    const float windNorm = std::clamp(I.windSpeed / 20.0f, 0.0f, 1.6f);
    const float targets[8] = {
        P.sea * (0.20f + 0.02f * I.windSpeed) * altitude * (0.35f + 0.65f / (1.0f + I.distToShore / 300.0f)) * snowMuffle,
        P.surf * std::clamp(I.shoreAmp / 0.5f, 0.0f, 2.5f) * shoreAtt * altitude * snowMuffle,
        P.wind * std::pow(windNorm, 1.3f) * (1.0f + std::min(I.camHeight / 300.0f, 1.0f)),
        P.rain * rainAmount * (I.covered > 0.5f ? 0.55f : 1.0f),
        P.thunder,
        0, 0, 0};

    // Mise à jour des filtres (une fois par bloc).
    m_gustLfo += (white() * 0.5f - m_gustLfo * 0.02f) * 0.05f;
    const float gust = std::clamp(1.0f + I.gust * m_gustLfo * 1.5f, 0.3f, 2.0f);
    for (int c = 0; c < 2; ++c) {
        setLowpass(m_seaLp[c], 380.0f + 10.0f * I.windSpeed, 0.6f);
        setHighpass(m_hissHp[c], 2800.0f, 0.7f);
        setBandpass(m_windBp[c], (240.0f + 32.0f * I.windSpeed) * gust * (c ? 1.07f : 0.94f), 0.8f);
        setBandpass(m_whistle[c], (620.0f + 45.0f * I.windSpeed) * gust * (c ? 1.02f : 0.98f), 9.0f);
        setHighpass(m_rainHp[c], 1300.0f, 0.7f);
        setLowpass(m_rainLp[c], I.covered > 0.5f ? 3500.0f : 9000.0f, 0.7f);
        setBandpass(m_dropBp[c], 2300.0f + 900.0f * c, 1.8f);
        setLowpass(m_thunderLp[c], 140.0f, 0.8f);
    }

    const float omega = I.shoreOmega;
    const float tailDecay = std::exp(-dt / 1.8f);
    const float dropDecay = std::exp(-dt / 0.004f);
    const float dropRate = 450.0f * rainAmount;
    const float panL = std::cos((I.shorePan * 0.7f + 1.0f) * 0.785398f), panR = std::sin((I.shorePan * 0.7f + 1.0f) * 0.785398f);

    for (int n = 0; n < frames; ++n) {
        for (int k = 0; k < 5; ++k) m_smooth[k] += (targets[k] - m_smooth[k]) * 0.0005f;
        const float time = I.oceanTime;
        if (!I.paused) m_in.oceanTime += dt;

        // Ressac : enveloppe synchronisée avec l'arrivée des crêtes sur le rivage.
        const float phase = I.shorePhaseOffset - omega * time;
        const float crash = std::pow(std::clamp(0.5f + 0.5f * std::cos(phase), 0.0f, 1.0f), 4.0f);
        m_surfTail = std::max(crash, m_surfTail * tailDecay);
        setBandpass(m_surfBp[0], 500.0f + 900.0f * crash, 0.9f); // (coût négligeable : 2 filtres)
        m_surfBp[1] = m_surfBp[0];

        float L = 0, R = 0;
        for (int c = 0; c < 2; ++c) {
            const float w = white();
            // Bruit brun (intégration fuyante) et rose (filtre de Paul Kellet).
            m_brown[c] = (m_brown[c] + 0.02f * w) * 0.998f;
            float* pk = m_pink[c];
            pk[0] = 0.99886f * pk[0] + w * 0.0555179f; pk[1] = 0.99332f * pk[1] + w * 0.0750759f;
            pk[2] = 0.96900f * pk[2] + w * 0.1538520f; pk[3] = 0.86650f * pk[3] + w * 0.3104856f;
            pk[4] = 0.55000f * pk[4] + w * 0.5329522f; pk[5] = -0.7616f * pk[5] - w * 0.0168980f;
            const float pink = (pk[0] + pk[1] + pk[2] + pk[3] + pk[4] + pk[5] + pk[6] + w * 0.5362f) * 0.11f;
            pk[6] = w * 0.115926f;

            auto sampleAt = [&](int i) -> float {
                const auto& s = m_samples[i];
                if (s.empty()) return 0.0f;
                const float v = s[m_samplePos[i] * 2 + c];
                return v;
            };

            // Mer au large : grondement grave et large.
            float sea = m_samples[0].empty() ? m_seaLp[c].process(m_brown[c] * 3.0f) * 2.2f : sampleAt(0);
            sea *= m_smooth[0];

            // Ressac : fracas (rose filtré) + chuintement de l'écume qui se retire (aigus).
            float surf;
            if (m_samples[1].empty()) {
                const float body = m_surfBp[c].process(pink) * crash * 2.2f;
                const float fizz = m_hissHp[c].process(w) * m_surfTail * 0.22f;
                const float thump = m_brown[c] * crash * 1.2f;
                surf = body + fizz + thump;
            } else {
                surf = sampleAt(1);
            }
            surf *= m_smooth[1] * (c == 0 ? panL : panR) * 1.41f;

            // Vent : bande de bruit mobile + sifflement aux fortes vitesses.
            float wind = m_samples[2].empty()
                ? m_windBp[c].process(w) * 0.9f + m_whistle[c].process(w) * 0.25f * std::max(windNorm - 0.5f, 0.0f)
                : sampleAt(2);
            wind *= m_smooth[2] * gust;

            // Pluie : souffle continu + impacts de gouttes aléatoires.
            float rain;
            if (m_samples[3].empty()) {
                const float hiss = m_rainLp[c].process(m_rainHp[c].process(w)) * 0.35f;
                if (float(m_rng & 0xFFFFF) / 1048576.0f < dropRate * dt) {
                    const float a = std::abs(white());
                    m_dropLevel[c] = a * a;
                    m_dropEnv[c] = 1.0f;
                }
                m_dropEnv[c] *= dropDecay;
                const float drop = m_dropBp[c].process(w * m_dropEnv[c] * m_dropLevel[c]) * 2.5f;
                rain = hiss + drop;
            } else {
                rain = sampleAt(3);
            }
            rain *= m_smooth[3];

            // Tonnerre : claquement (si proche) puis roulement grave.
            float thunder = 0;
            for (auto& t : m_thunder) {
                if (!t.active || t.delay > 0) continue;
                const float crack = t.t < 0.25f ? std::exp(-t.t * 14.0f) * t.closeness * w * 0.8f : 0.0f;
                const float env = (1.0f - std::exp(-t.t * 6.0f)) * std::exp(-t.t / (2.5f + 2.5f * (1.0f - t.closeness)));
                thunder += crack + m_brown[c] * 6.0f * env * (0.5f + 0.8f * t.closeness);
            }
            thunder = m_thunderLp[c].process(thunder) * m_smooth[4];

            const float mix = (sea + surf + wind + rain + thunder) * P.master;
            const float y = mix / (1.0f + std::abs(mix)); // écrêtage doux
            (c == 0 ? L : R) = y;
        }
        for (int i = 0; i < 4; ++i)
            if (!m_samples[i].empty()) m_samplePos[i] = (m_samplePos[i] + 1) % (m_samples[i].size() / 2);
        for (auto& t : m_thunder) {
            if (!t.active) continue;
            if (t.delay > 0) t.delay -= dt;
            else if ((t.t += dt) > 12.0f) t.active = false;
        }
        out[n * 2] = L;
        out[n * 2 + 1] = R;
    }
}

} // namespace me
