#include "Gpu.h"
#include <dxgi1_5.h>
#include <cstdio>
#include <cstring>

namespace me {

void fatal(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"Météo & Eau : erreur", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = t;
    return p;
}

bool Gpu::init(HWND hwnd, UINT width, UINT height) {
    m_width = width;
    m_height = height;
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
        dbg->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    ME_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));

    // Adaptateur matériel avec le plus de VRAM.
    ComPtr<IDXGIAdapter1> best;
    SIZE_T bestMem = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
            d.DedicatedVideoMemory > bestMem) {
            bestMem = d.DedicatedVideoMemory;
            best = adapter;
        }
    }
    if (!best) fatal(L"Aucun GPU compatible DirectX 12 n'a été trouvé.");
    ME_CHECK(D3D12CreateDevice(best.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ME_CHECK(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)));

    // Tearing (vsync désactivable sur écrans VRR).
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(m_factory.As(&f5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            m_tearing = allow == TRUE;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    ME_CHECK(m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1));
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    ME_CHECK(sc1.As(&m_swapChain));

    for (UINT i = 0; i < kFramesInFlight; ++i)
        ME_CHECK(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc[i])));
    ME_CHECK(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0].Get(), nullptr, IID_PPV_ARGS(&m_cmd)));
    m_cmd->Close();
    ME_CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Tas de descripteurs.
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kPersistentDescriptors + kRingDescriptorsPerFrame * kFramesInFlight;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ME_CHECK(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)));
    hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 64;
    ME_CHECK(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 16;
    ME_CHECK(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap)));
    m_srvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_rtvCount = 2; // les deux premiers RTV sont réservés au swapchain

    // Signature racine unique : b0 (frame), b1 (passe), t0-t7, u0-u7, 4 samplers statiques.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kTableSize;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = kTableSize;
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; ++i) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    const D3D12_FILTER filters[4] = {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                     D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_POINT};
    const D3D12_TEXTURE_ADDRESS_MODE modes[4] = {D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP};
    for (UINT i = 0; i < 4; ++i) {
        samplers[i].Filter = filters[i];
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = modes[i];
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 4;
    rsd.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        fatal(L"Échec de sérialisation de la signature racine.");
    ME_CHECK(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));

    // Anneaux d'upload (constantes + données initiales).
    for (UINT i = 0; i < kFramesInFlight; ++i) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kUploadBytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto hp = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        ME_CHECK(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&m_upload[i])));
        D3D12_RANGE none{0, 0};
        m_upload[i]->Map(0, &none, reinterpret_cast<void**>(&m_uploadPtr[i]));
    }
    createBackBuffers();
    return true;
}

void Gpu::createBackBuffers() {
    for (UINT i = 0; i < 2; ++i) {
        Texture& t = m_backBuffers[i];
        t = Texture{};
        ME_CHECK(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&t.res)));
        t.state = D3D12_RESOURCE_STATE_PRESENT;
        t.format = t.viewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.width = m_width;
        t.height = m_height;
        t.rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        t.rtv.ptr += SIZE_T(i) * m_rtvInc;
        m_device->CreateRenderTargetView(t.res.Get(), nullptr, t.rtv);
    }
    m_backIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void Gpu::resize(UINT width, UINT height) {
    if (width == 0 || height == 0 || (width == m_width && height == m_height)) return;
    waitIdle();
    for (auto& b : m_backBuffers) b.res.Reset();
    m_width = width;
    m_height = height;
    ME_CHECK(m_swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                        m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    createBackBuffers();
}

void Gpu::waitIdle() {
    const UINT64 v = m_nextFence++;
    m_queue->Signal(m_fence.Get(), v);
    if (m_fence->GetCompletedValue() < v) {
        m_fence->SetEventOnCompletion(v, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Gpu::shutdown() {
    if (m_queue) waitIdle();
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
}

void Gpu::beginFrame() {
    m_frameIndex = UINT(m_frameCount % kFramesInFlight);
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_alloc[m_frameIndex]->Reset();
    m_cmd->Reset(m_alloc[m_frameIndex].Get(), nullptr);
    m_uploadOffset = 0;
    m_ringOffset = 0;
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    m_cmd->SetDescriptorHeaps(1, heaps);
    m_cmd->SetComputeRootSignature(m_rootSig.Get());
    m_cmd->SetGraphicsRootSignature(m_rootSig.Get());
    m_backIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void Gpu::endFrame(bool vsync) {
    transition(backBuffer(), D3D12_RESOURCE_STATE_PRESENT);
    m_cmd->Close();
    ID3D12CommandList* lists[] = {m_cmd.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    const UINT flags = (!vsync && m_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    m_swapChain->Present(vsync ? 1 : 0, flags);
    m_fenceValues[m_frameIndex] = m_nextFence++;
    m_queue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]);
    ++m_frameCount;
}

Texture Gpu::createTexture2D(UINT w, UINT h, DXGI_FORMAT fmt, uint32_t flags, const wchar_t* name) {
    Texture t;
    t.width = w;
    t.height = h;
    t.isDepth = (flags & TexDepth) != 0;
    t.format = t.isDepth ? DXGI_FORMAT_R32_TYPELESS : fmt;
    t.viewFormat = t.isDepth ? DXGI_FORMAT_R32_FLOAT : fmt;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = t.format;
    d.SampleDesc.Count = 1;
    if (flags & TexUav) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (flags & TexRT) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (flags & TexDepth) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{};
    const D3D12_CLEAR_VALUE* pcv = nullptr;
    if (flags & TexDepth) {
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 0.0f; // Z inversé : 0 = loin
        pcv = &cv;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else if (flags & TexRT) {
        cv.Format = fmt;
        pcv = &cv;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ME_CHECK(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t.state, pcv, IID_PPV_ARGS(&t.res)));
    t.res->SetName(name);
    if (flags & TexRT) {
        if (!m_freeRtv.empty()) { t.rtv = m_freeRtv.back(); m_freeRtv.pop_back(); }
        else {
            if (m_rtvCount >= 64) fatal(L"Plus d'emplacements RTV.");
            t.rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            t.rtv.ptr += SIZE_T(m_rtvCount++) * m_rtvInc;
        }
        m_device->CreateRenderTargetView(t.res.Get(), nullptr, t.rtv);
    }
    if (flags & TexDepth) {
        if (!m_freeDsv.empty()) { t.dsv = m_freeDsv.back(); m_freeDsv.pop_back(); }
        else {
            if (m_dsvCount >= 16) fatal(L"Plus d'emplacements DSV.");
            t.dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
            t.dsv.ptr += SIZE_T(m_dsvCount++) * m_dsvInc;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(t.res.Get(), &dd, t.dsv);
    }
    return t;
}

Texture Gpu::createTexture3D(UINT w, UINT h, UINT dpt, DXGI_FORMAT fmt, const wchar_t* name) {
    Texture t;
    t.width = w;
    t.height = h;
    t.depth = dpt;
    t.is3D = true;
    t.format = t.viewFormat = fmt;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = UINT16(dpt);
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ME_CHECK(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t.state, nullptr, IID_PPV_ARGS(&t.res)));
    t.res->SetName(name);
    return t;
}

Buffer Gpu::createStructured(UINT stride, UINT count, bool uav, const wchar_t* name) {
    Buffer b;
    b.stride = stride;
    b.count = count;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = UINT64(stride) * count;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ME_CHECK(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, b.state, nullptr, IID_PPV_ARGS(&b.res)));
    b.res->SetName(name);
    return b;
}

void Gpu::releaseViews(Texture& t) {
    if (t.rtv.ptr) m_freeRtv.push_back(t.rtv);
    if (t.dsv.ptr) m_freeDsv.push_back(t.dsv);
    t.rtv = {};
    t.dsv = {};
}

void Gpu::uploadBuffer(Buffer& dst, const void* data, size_t bytes) {
    m_uploadOffset = (m_uploadOffset + 255) & ~size_t(255);
    if (m_uploadOffset + bytes > kUploadBytes) fatal(L"Anneau d'upload saturé.");
    std::memcpy(m_uploadPtr[m_frameIndex] + m_uploadOffset, data, bytes);
    transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    m_cmd->CopyBufferRegion(dst.res.Get(), 0, m_upload[m_frameIndex].Get(), m_uploadOffset, bytes);
    m_uploadOffset += bytes;
}

void Gpu::transition(GpuResource& r, D3D12_RESOURCE_STATES s) {
    if (r.state == s) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r.res.Get();
    b.Transition.StateBefore = r.state;
    b.Transition.StateAfter = s;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);
    r.state = s;
}

void Gpu::uavBarrier(GpuResource& r) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r.res.Get();
    m_cmd->ResourceBarrier(1, &b);
}

D3D12_GPU_VIRTUAL_ADDRESS Gpu::uploadConstants(const void* data, size_t bytes) {
    m_uploadOffset = (m_uploadOffset + 255) & ~size_t(255);
    if (m_uploadOffset + bytes > kUploadBytes) fatal(L"Anneau d'upload saturé.");
    std::memcpy(m_uploadPtr[m_frameIndex] + m_uploadOffset, data, bytes);
    const D3D12_GPU_VIRTUAL_ADDRESS va = m_upload[m_frameIndex]->GetGPUVirtualAddress() + m_uploadOffset;
    m_uploadOffset += bytes;
    return va;
}

void Gpu::writeSrv(const View& v, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (v.tex) {
        d.Format = v.tex->viewFormat;
        if (v.tex->is3D) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            d.Texture3D.MipLevels = 1;
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipLevels = 1;
        }
        m_device->CreateShaderResourceView(v.tex->res.Get(), &d, dst);
    } else if (v.buf) {
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Buffer.NumElements = v.buf->count;
        d.Buffer.StructureByteStride = v.buf->stride;
        m_device->CreateShaderResourceView(v.buf->res.Get(), &d, dst);
    } else {
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(nullptr, &d, dst);
    }
}

void Gpu::writeUav(const View& v, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    if (v.tex) {
        d.Format = v.tex->viewFormat;
        if (v.tex->is3D) {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            d.Texture3D.WSize = v.tex->depth;
        } else {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        m_device->CreateUnorderedAccessView(v.tex->res.Get(), nullptr, &d, dst);
    } else if (v.buf) {
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = v.buf->count;
        d.Buffer.StructureByteStride = v.buf->stride;
        m_device->CreateUnorderedAccessView(v.buf->res.Get(), nullptr, &d, dst);
    } else {
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(nullptr, nullptr, &d, dst);
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE Gpu::buildTable(std::initializer_list<View> views, bool uav) {
    if (m_ringOffset + kTableSize > kRingDescriptorsPerFrame) fatal(L"Anneau de descripteurs saturé.");
    const UINT base = kPersistentDescriptors + m_frameIndex * kRingDescriptorsPerFrame + m_ringOffset;
    m_ringOffset += kTableSize;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(base) * m_srvInc;
    gpu.ptr += UINT64(base) * m_srvInc;
    auto it = views.begin();
    for (UINT i = 0; i < kTableSize; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += SIZE_T(i) * m_srvInc;
        const View v = (it != views.end()) ? *it++ : View{};
        if (uav) writeUav(v, h);
        else writeSrv(v, h);
    }
    return gpu;
}

void Gpu::transitionViews(std::initializer_list<View> views, D3D12_RESOURCE_STATES s) {
    for (const View& v : views) {
        GpuResource* r = v.tex ? static_cast<GpuResource*>(const_cast<Texture*>(v.tex))
                               : static_cast<GpuResource*>(const_cast<Buffer*>(v.buf));
        if (!r) continue;
        D3D12_RESOURCE_STATES target = s;
        if (v.tex && v.tex->isDepth && s != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            target |= D3D12_RESOURCE_STATE_DEPTH_READ; // lisible + test de profondeur en lecture seule
        transition(*r, target);
    }
}

static const D3D12_RESOURCE_STATES kSrvState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void Gpu::dispatch(ID3D12PipelineState* pso, const void* passCB, size_t cbBytes,
                   std::initializer_list<View> srvs, std::initializer_list<View> uavs,
                   UINT gx, UINT gy, UINT gz) {
    if (!pso) return;
    transitionViews(srvs, kSrvState);
    transitionViews(uavs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cmd->SetComputeRootSignature(m_rootSig.Get());
    m_cmd->SetPipelineState(pso);
    m_cmd->SetComputeRootConstantBufferView(0, m_frameCB);
    m_cmd->SetComputeRootConstantBufferView(1, passCB ? uploadConstants(passCB, cbBytes) : m_frameCB);
    m_cmd->SetComputeRootDescriptorTable(2, buildTable(srvs, false));
    m_cmd->SetComputeRootDescriptorTable(3, buildTable(uavs, true));
    m_cmd->Dispatch(gx, gy, gz);
    for (const View& v : uavs) {
        GpuResource* r = v.tex ? static_cast<GpuResource*>(const_cast<Texture*>(v.tex))
                               : static_cast<GpuResource*>(const_cast<Buffer*>(v.buf));
        if (r) uavBarrier(*r);
    }
}

void Gpu::bindGraphics(ID3D12PipelineState* pso, const void* passCB, size_t cbBytes,
                       std::initializer_list<View> srvs) {
    transitionViews(srvs, kSrvState);
    m_cmd->SetGraphicsRootSignature(m_rootSig.Get());
    m_cmd->SetPipelineState(pso);
    m_cmd->SetGraphicsRootConstantBufferView(0, m_frameCB);
    m_cmd->SetGraphicsRootConstantBufferView(1, passCB ? uploadConstants(passCB, cbBytes) : m_frameCB);
    m_cmd->SetGraphicsRootDescriptorTable(2, buildTable(srvs, false));
    m_cmd->SetGraphicsRootDescriptorTable(3, buildTable({}, true));
}

void Gpu::setViewport(UINT w, UINT h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    m_cmd->RSSetViewports(1, &vp);
    m_cmd->RSSetScissorRects(1, &sc);
}

UINT Gpu::allocPersistent() {
    if (m_persistentCount >= kPersistentDescriptors) fatal(L"Plus de descripteurs persistants.");
    return m_persistentCount++;
}

D3D12_CPU_DESCRIPTOR_HANDLE Gpu::persistentCpu(UINT i) const {
    auto h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * m_srvInc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Gpu::persistentGpu(UINT i) const {
    auto h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(i) * m_srvInc;
    return h;
}

UINT64 Gpu::timestampFrequency() const {
    UINT64 f = 1;
    m_queue->GetTimestampFrequency(&f);
    return f;
}

} // namespace me
