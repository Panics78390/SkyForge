// Couche DirectX 12 minimale : device, swapchain, ressources, liaison automatique
// des descripteurs et des transitions. Tout le reste du programme passe par ici.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace me {
using Microsoft::WRL::ComPtr;

constexpr UINT kFramesInFlight = 2;
constexpr UINT kTableSize = 8;           // t0..t7 et u0..u7 par passe
constexpr UINT kPersistentDescriptors = 256; // police ImGui + vues de debug
constexpr UINT kRingDescriptorsPerFrame = 8192;

struct GpuResource {
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

enum TexFlags : uint32_t { TexNone = 0, TexUav = 1, TexRT = 2, TexDepth = 4 };

struct Texture : GpuResource {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;     // format de la ressource
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // format SRV/UAV
    UINT width = 0, height = 0, depth = 1;
    bool is3D = false, isDepth = false;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
};

struct Buffer : GpuResource {
    UINT stride = 0, count = 0;
};

// Vue passée à une passe : une texture ou un buffer structuré.
struct View {
    const Texture* tex = nullptr;
    const Buffer* buf = nullptr;
    View() = default;
    View(const Texture& t) : tex(&t) {}
    View(const Buffer& b) : buf(&b) {}
};

class Gpu {
public:
    bool init(HWND hwnd, UINT width, UINT height);
    void shutdown();
    void resize(UINT width, UINT height);
    void beginFrame();
    void endFrame(bool vsync);
    void waitIdle();

    ID3D12Device* device() const { return m_device.Get(); }
    ID3D12GraphicsCommandList* cmd() const { return m_cmd.Get(); }
    ID3D12RootSignature* rootSignature() const { return m_rootSig.Get(); }
    ID3D12DescriptorHeap* srvHeap() const { return m_srvHeap.Get(); }
    ID3D12CommandQueue* queue() const { return m_queue.Get(); }
    UINT width() const { return m_width; }
    UINT height() const { return m_height; }
    UINT frameIndex() const { return m_frameIndex; }
    uint64_t frameCount() const { return m_frameCount; }

    // Création de ressources.
    Texture createTexture2D(UINT w, UINT h, DXGI_FORMAT fmt, uint32_t flags, const wchar_t* name);
    Texture createTexture3D(UINT w, UINT h, UINT d, DXGI_FORMAT fmt, const wchar_t* name);
    Buffer createStructured(UINT stride, UINT count, bool uav, const wchar_t* name);
    void releaseViews(Texture& t); // rend les emplacements RTV/DSV (avant recréation)
    void uploadBuffer(Buffer& dst, const void* data, size_t bytes); // via upload ring de la frame

    // Transitions et barrières.
    void transition(GpuResource& r, D3D12_RESOURCE_STATES s);
    void uavBarrier(GpuResource& r);

    // Constantes : copie dans l'anneau d'upload de la frame, renvoie l'adresse GPU.
    D3D12_GPU_VIRTUAL_ADDRESS uploadConstants(const void* data, size_t bytes);
    void setFrameConstants(const void* data, size_t bytes) { m_frameCB = uploadConstants(data, bytes); }

    // Lance un compute shader : liaisons, transitions et barrière UAV automatiques.
    void dispatch(ID3D12PipelineState* pso, const void* passCB, size_t cbBytes,
                  std::initializer_list<View> srvs, std::initializer_list<View> uavs,
                  UINT gx, UINT gy, UINT gz);
    // Prépare un draw graphique (même signature racine).
    void bindGraphics(ID3D12PipelineState* pso, const void* passCB, size_t cbBytes,
                      std::initializer_list<View> srvs);

    // Cibles de rendu.
    Texture& backBuffer() { return m_backBuffers[m_backIndex]; }
    void setViewport(UINT w, UINT h);

    // Descripteurs persistants (ImGui, aperçus de textures dans le panneau).
    UINT allocPersistent();
    D3D12_CPU_DESCRIPTOR_HANDLE persistentCpu(UINT i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE persistentGpu(UINT i) const;
    void writeSrv(const View& v, D3D12_CPU_DESCRIPTOR_HANDLE dst);

    // Requêtes de timestamps (utilisées par GpuProfiler).
    UINT64 timestampFrequency() const;

private:
    void createBackBuffers();
    void writeUav(const View& v, D3D12_CPU_DESCRIPTOR_HANDLE dst);
    D3D12_GPU_DESCRIPTOR_HANDLE buildTable(std::initializer_list<View> views, bool uav);
    void transitionViews(std::initializer_list<View> views, D3D12_RESOURCE_STATES s);

    ComPtr<IDXGIFactory4> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12CommandAllocator> m_alloc[kFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> m_cmd;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValues[kFramesInFlight] = {};
    UINT64 m_nextFence = 1;

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap, m_rtvHeap, m_dsvHeap;
    UINT m_srvInc = 0, m_rtvInc = 0, m_dsvInc = 0;
    UINT m_rtvCount = 0, m_dsvCount = 0;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_freeRtv, m_freeDsv;
    UINT m_persistentCount = 0;
    UINT m_ringOffset = 0;

    ComPtr<ID3D12Resource> m_upload[kFramesInFlight];
    uint8_t* m_uploadPtr[kFramesInFlight] = {};
    size_t m_uploadOffset = 0;
    static constexpr size_t kUploadBytes = 32ull << 20;

    Texture m_backBuffers[2];
    UINT m_backIndex = 0;
    UINT m_frameIndex = 0;
    uint64_t m_frameCount = 0;
    UINT m_width = 0, m_height = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_frameCB = 0;
    bool m_tearing = false;
};

// Utilitaire : arrondi au multiple supérieur pour les dispatches.
inline UINT divUp(UINT a, UINT b) { return (a + b - 1) / b; }

void fatal(const std::wstring& msg);
#define ME_CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { \
    wchar_t b_[512]; swprintf_s(b_, L"%hs\nHRESULT 0x%08X\n%hs:%d", #x, (unsigned)hr_, __FILE__, __LINE__); \
    me::fatal(b_); } } while (0)

} // namespace me
