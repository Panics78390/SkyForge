#include "Pipelines.h"
#include <d3dcompiler.h>
#include <cstring>

namespace me {

ComPtr<ID3DBlob> Pipelines::compile(const char* file, const char* entry, const char* target, const Defines& defines) {
    std::wstring path = m_dir + L"/";
    for (const char* c = file; *c; ++c) path.push_back(wchar_t(*c));
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& d : defines) macros.push_back({d.first.c_str(), d.second.c_str()});
    macros.push_back({nullptr, nullptr});
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompileFromFile(path.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                    flags, 0, &code, &err);
    if (FAILED(hr)) {
        m_errors += std::string("[") + file + ":" + entry + "] ";
        if (err) m_errors.append(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        else m_errors += "fichier introuvable ou illisible\n";
        m_errors += "\n";
        return nullptr;
    }
    return code;
}

ComPtr<ID3D12PipelineState> Pipelines::compute(const char* file, const Defines& defines, const char* entry) {
    ComPtr<ID3DBlob> cs = compile(file, entry, "cs_5_1", defines);
    if (!cs) return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = m_gpu->rootSignature();
    d.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(m_gpu->device()->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)))) {
        m_errors += std::string("[") + file + "] création du PSO compute impossible\n";
        return nullptr;
    }
    return pso;
}

ComPtr<ID3D12PipelineState> Pipelines::graphics(const GraphicsDesc& g, const Defines& defines) {
    ComPtr<ID3DBlob> vs = compile(g.file, g.vsEntry, "vs_5_1", defines);
    ComPtr<ID3DBlob> ps = compile(g.file, g.psEntry, "ps_5_1", defines);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = m_gpu->rootSignature();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = g.cullBack ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    auto& rt = d.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (g.blend != BlendMode::Opaque) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = g.blend == BlendMode::Alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (g.depth != DepthMode::None) {
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // Z inversé
        d.DepthStencilState.DepthWriteMask =
            g.depth == DepthMode::WriteGreater ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    }
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (g.rtFormat != DXGI_FORMAT_UNKNOWN) {
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = g.rtFormat;
    }
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(m_gpu->device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
        m_errors += std::string("[") + g.file + "] création du PSO graphique impossible\n";
        return nullptr;
    }
    return pso;
}

// ----------------------------------------------------------------------------
void GpuProfiler::init(Gpu& gpu) {
    m_gpu = &gpu;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kMaxPasses * 2 * kFramesInFlight;
    ME_CHECK(gpu.device()->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_heap)));
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = sizeof(UINT64) * qd.Count;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ME_CHECK(gpu.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&m_readback)));
    m_freq = double(gpu.timestampFrequency());
}

void GpuProfiler::beginFrame() {
    // La fence de cette frame vient d'être attendue : ses timestamps sont disponibles.
    const UINT f = m_gpu->frameIndex();
    const UINT n = m_count[f];
    if (n == 0) m_results.clear();
    if (n > 0) {
        D3D12_RANGE r{sizeof(UINT64) * kMaxPasses * 2 * f, sizeof(UINT64) * kMaxPasses * 2 * (f + 1)};
        UINT64* data = nullptr;
        if (SUCCEEDED(m_readback->Map(0, &r, reinterpret_cast<void**>(&data)))) {
            const UINT64* q = data + kMaxPasses * 2 * f;
            // On ne garde que les passes exécutées dans cette frame (moyenne glissante conservée).
            std::vector<Entry> next;
            for (UINT i = 0; i < n; ++i) {
                const float ms = float(double(q[2 * i + 1] - q[2 * i]) * 1000.0 / m_freq);
                float avg = ms;
                for (const auto& x : m_results)
                    if (std::strcmp(x.name, m_names[f][i]) == 0) avg = x.avgMs * 0.95f + ms * 0.05f;
                next.push_back({m_names[f][i], ms, avg});
            }
            m_results.swap(next);
            D3D12_RANGE none{0, 0};
            m_readback->Unmap(0, &none);
        }
    }
    m_count[f] = 0;
}

void GpuProfiler::begin(const char* name) {
    const UINT f = m_gpu->frameIndex();
    if (m_count[f] >= kMaxPasses) return;
    m_open = int(m_count[f]);
    m_names[f][m_open] = name;
    m_gpu->cmd()->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (kMaxPasses * f + m_open) * 2);
}

void GpuProfiler::end() {
    if (m_open < 0) return;
    const UINT f = m_gpu->frameIndex();
    m_gpu->cmd()->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (kMaxPasses * f + m_open) * 2 + 1);
    m_count[f] = UINT(m_open) + 1;
    m_open = -1;
}

void GpuProfiler::endFrame() {
    const UINT f = m_gpu->frameIndex();
    if (m_count[f] == 0) return;
    m_gpu->cmd()->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, kMaxPasses * 2 * f, m_count[f] * 2,
                                   m_readback.Get(), sizeof(UINT64) * kMaxPasses * 2 * f);
}

float GpuProfiler::totalMs() const {
    float t = 0;
    for (const auto& e : m_results) t += e.avgMs;
    return t;
}

} // namespace me
