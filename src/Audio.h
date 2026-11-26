// Ambiance sonore : mer, ressac synchronisé avec les vagues de bord, vent, pluie et tonnerre.
// Les sons sont synthétisés en temps réel (bruits filtrés modulés par la météo), sans fichier.
// Pour utiliser de vrais enregistrements, déposer des fichiers WAV dans le dossier « sons »
// à côté de l'exécutable : mer.wav, ressac.wav, vent.wav, pluie.wav (lus en boucle, toujours
// modulés par la météo). Sortie : XAudio2, 48 kHz stéréo, blocs de 10 ms sur un fil dédié.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace me {

struct AudioParams {
    float master = 0.8f;
    float sea = 1.0f, surf = 1.0f, wind = 1.0f, rain = 1.0f, thunder = 1.0f;
    bool mute = false;
};

// État du monde transmis au fil audio à chaque frame.
struct AudioInputs {
    float windSpeed = 6, gust = 0.3f, precip = 0, snowFactor = 0;
    float shoreAmp = 0.4f;            // amplitude des vagues de bord (m)
    float shoreOmega = 1.6f;          // pulsation des vagues de bord (rad/s)
    float shorePhaseOffset = 0;       // décalage de phase le long du rivage, au droit de la caméra
    float oceanTime = 0;              // temps de l'océan (même horloge que les shaders)
    float distToShore = 50;           // m
    float shorePan = 0;               // -1 gauche, +1 droite
    float camHeight = 2;              // au-dessus de la mer (m)
    float covered = 0;                // 1 = abrité de la pluie (sous l'arche)
    bool paused = false;
};

class AudioEngine {
public:
    bool init(const std::wstring& soundDir);
    void shutdown();
    void setInputs(const AudioInputs& in);
    void triggerThunder(float delaySeconds, float closeness); // closeness 0 (loin) .. 1 (tout près)
    AudioParams params;
    const std::string& status() const { return m_status; }
    bool hasSample(int i) const { return !m_samples[i].empty(); }
    static constexpr const char* kSampleNames[4] = {"mer.wav", "ressac.wav", "vent.wav", "pluie.wav"};

private:
    void threadMain();
    void render(float* out, int frames);
    static std::vector<float> loadWav(const std::wstring& path, int targetRate);

    IXAudio2* m_xa = nullptr;
    IXAudio2MasteringVoice* m_master = nullptr;
    IXAudio2SourceVoice* m_voice = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_mutex;
    AudioInputs m_in, m_shared;
    AudioParams m_paramsShared;
    std::string m_status = "Son non initialisé";

    static constexpr int kRate = 48000, kBlock = 480, kBuffers = 4;
    std::vector<float> m_buffers[kBuffers];
    int m_nextBuffer = 0;

    // État de synthèse (fil audio uniquement).
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; float process(float x); };
    void setLowpass(Biquad& f, float hz, float q);
    void setHighpass(Biquad& f, float hz, float q);
    void setBandpass(Biquad& f, float hz, float q);
    float white();
    uint32_t m_rng = 0x12345678u;
    float m_brown[2] = {}, m_pink[2][7] = {};
    Biquad m_seaLp[2], m_surfBp[2], m_hissHp[2], m_windBp[2], m_whistle[2], m_rainHp[2], m_rainLp[2], m_dropBp[2], m_thunderLp[2];
    float m_smooth[8] = {};
    float m_gustLfo = 0, m_windPhase = 0, m_seaLfo = 0;
    float m_surfTail = 0;
    float m_dropEnv[2] = {}, m_dropLevel[2] = {};
    struct Thunder { float delay; float t; float closeness; bool active; };
    Thunder m_thunder[4] = {};
    float m_thunderEnvNoise = 0;
    std::vector<float> m_samples[4];
    size_t m_samplePos[4] = {};
    float m_lastReceivedTime = -1.0f;
};

} // namespace me
