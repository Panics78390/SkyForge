// Compilation HLSL à l'exécution (D3DCompile, SM 5.1) et création des PSO.
// Les shaders vivent dans le dossier shaders/ à côté de l'exécutable : on peut
// les modifier puis cliquer « Recharger les shaders » sans relancer le programme.
#pragma once
#include "Gpu.h"
#include <string>
#include <utility>
#include <vector>

namespace me {

enum class DepthMode { None, WriteGreater, TestGreater };
enum class BlendMode { Opaque, Alpha, Additive };

struct GraphicsDesc {
    const char* file = nullptr;
    const char* vsEntry = "VSMain";
    const char* psEntry = "PSMain";
    DXGI_FORMAT rtFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // UNKNOWN = pas de cible couleur
    DepthMode depth = DepthMode::None;
    BlendMode blend = BlendMode::Opaque;
    bool cullBack = false;
};

using Defines = std::vector<std::pair<std::string, std::string>>;

class Pipelines {
public:
    void init(Gpu& gpu, std::wstring shaderDir) { m_gpu = &gpu; m_dir = std::move(shaderDir); }
    ComPtr<ID3D12PipelineState> compute(const char* file, const Defines& defines = {}, const char* entry = "main");
    ComPtr<ID3D12PipelineState> graphics(const GraphicsDesc& d, const Defines& defines = {});

    const std::string& errors() const { return m_errors; }
    void clearErrors() { m_errors.clear(); }

private:
    ComPtr<ID3DBlob> compile(const char* file, const char* entry, const char* target, const Defines& defines);
    Gpu* m_gpu = nullptr;
    std::wstring m_dir;
    std::string m_errors;
};

// Mesure GPU par passe (timestamps), lue avec 2 frames de retard.
class GpuProfiler {
public:
    static constexpr UINT kMaxPasses = 32;
    void init(Gpu& gpu);
    void beginFrame();                   // lit les résultats de la frame terminée
    void begin(const char* name);
    void end();
    void endFrame();                     // résout les requêtes
    struct Entry { const char* name; float ms; float avgMs; };
    const std::vector<Entry>& results() const { return m_results; }
    float totalMs() const;
private:
    Gpu* m_gpu = nullptr;
    ComPtr<ID3D12QueryHeap> m_heap;
    ComPtr<ID3D12Resource> m_readback;
    const char* m_names[kFramesInFlight][kMaxPasses] = {};
    UINT m_count[kFramesInFlight] = {};
    int m_open = -1;
    std::vector<Entry> m_results;
    double m_freq = 1.0;
};

class ScopedPass {
public:
    ScopedPass(GpuProfiler& p, const char* n) : m_p(p) { m_p.begin(n); }
    ~ScopedPass() { m_p.end(); }
private:
    GpuProfiler& m_p;
};

} // namespace me
