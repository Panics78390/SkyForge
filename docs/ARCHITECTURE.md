# Module « Météo & Eau » : architecture et plan de fichiers

> Étape 0 sur 5. Hypothèses de travail (à confirmer) : **DirectX 12 + HLSL Shader Model 6.6**, **Windows 10/11 PC**, **DirectStorage 1.2**, GPU cible de référence classe RTX 3060 / RX 6600 en 1440p.
> Repère : **Y vers le haut, unités en mètres, main gauche** (convention D3D). Tout est isolé derrière une petite couche RHI pour qu'un portage Vulkan ne touche que `Backend/`.

---

## 1. Principes directeurs

1. **Un seul point d'entrée** : `WeatherSystem` possède les sous-systèmes, l'état météo global et le budget. Le moteur ne voit que lui.
2. **Données d'abord** : chaque sous-système expose un `struct …GpuConstants` (aligné 16 octets, partagé C++/HLSL via un header commun `.hlsli`) et des ressources GPU nommées. Aucune logique de rendu dans les classes « état ».
3. **Pilotage par un état météo unique** (`WeatherState`) interpolé dans le temps : couverture nuageuse, vent, précipitations, humidité, température, soleil. Tout le reste en dérive. Un changement de météo = une interpolation, jamais un saut.
4. **GPU-driven** : la simulation reste sur GPU. Le CPU ne relit que la flottabilité (readback asynchrone, latence 2 frames) et les timestamps de profilage.
5. **Async compute** : l'eau (FFT + grille) et le vent tournent sur la file compute en parallèle du depth prepass et des ombres. Les nuages tournent en graphics/compute après la depth (ils en ont besoin pour l'occlusion).
6. **Budget explicite** : chaque passe a un budget en µs par niveau de qualité. Le profileur compare le mesuré au budget et peut dégrader automatiquement (option).

---

## 2. Schéma d'architecture

```
                         ┌──────────────────────────────────────────┐
                         │               Moteur hôte                │
                         │  (scène, caméra, frame graph, physique)  │
                         └───────────────┬──────────────────────────┘
                                         │ IWeatherHost (callbacks : RHI, caméra,
                                         │ depth/GBuffer, physique, streaming)
                         ┌───────────────▼──────────────────────────┐
                         │              WeatherSystem               │
                         │  WeatherState (interpolé)  QualityPreset │
                         │  GpuProfiler   SimulationLOD   Streamer  │
                         └──┬──────────┬───────────┬──────────┬─────┘
                            │          │           │          │
          ┌─────────────────▼┐  ┌──────▼──────┐ ┌──▼────────┐ ┌▼──────────────────┐
          │      WATER       │  │   CLOUDS    │ │   WIND    │ │  RAIN & WETNESS   │
          │ OceanFFT (×3     │  │ NoiseBaker  │ │ WindField │ │ RainParticles     │
          │   cascades)      │  │ (3D tex,    │ │ (tex3D    │ │ RainOcclusion     │
          │ ShallowGrid      │  │  démarrage) │ │  RG16F/   │ │ (depth vue du ciel)│
          │   (eulérienne)   │  │ WeatherMap  │ │  RGBA16F) │ │ WetnessMap        │
          │ HullInteraction  │  │ CloudMarch  │ │           │ │ (humidité/flaques │
          │ Buoyancy(readbk) │  │ Temporal    │ │           │ │  + séchage)       │
          │ WaterSurface     │  │ Reprojection│ │           │ │ WetPBR (include)  │
          │ (rendu, Gerstner │  │ Composite   │ │           │ │                   │
          │  au loin)        │  │             │ │           │ │                   │
          └───────┬──────────┘  └─────┬───────┘ └────┬──────┘ └────────┬──────────┘
                  │                   │              │                 │
                  └───────────┬───────┴──────┬───────┴─────────┬───────┘
                              │              │                 │
                     ┌────────▼───────┐ ┌────▼───────┐  ┌──────▼─────────┐
                     │ Ressources     │ │ Shaders    │  │ Consommateurs  │
                     │ partagées      │ │ communs    │  │ externes       │
                     │ (WindTex,      │ │ Common.hlsli│ │ végétation,    │
                     │  WetnessTex,   │ │ Noise.hlsli │ │ tissu, VFX,    │
                     │  OceanDisp…)   │ │ Wet.hlsli   │ │ matériaux PBR  │
                     └────────────────┘ └────────────┘  └────────────────┘
```

### Dépendances de données entre sous-systèmes

```
WeatherState ──► Wind ──► (Clouds : advection de la carte météo)
     │             ├────► Rain (orientation, dérive)
     │             ├────► Ocean (vitesse/direction du vent → spectre JONSWAP)
     │             └────► Wetness (séchage)
     ├──► Clouds ──► (ombre des nuages → Wetness séchage solaire, éclairage)
     ├──► Rain ──► Wetness (accumulation) ; Rain ──► ShallowGrid (impacts, option)
     └──► Ocean ──► ShallowGrid (conditions aux bords) ──► HullInteraction ──► Buoyancy (CPU)
```

Règle : un sous-système lit les sorties d'un autre **de la frame courante si elles sont produites avant lui dans l'ordre ci-dessous, sinon de la frame précédente** (double buffer). Ça évite toute dépendance circulaire.

---

## 3. Ordre des passes dans une frame

```
CPU  : WeatherSystem::Update(dt)        interpolation état, LOD, requêtes streaming,
                                         lecture flottabilité de la frame N-2
──────────────────────────────────────────────────────────────────────────────────
ASYNC COMPUTE (chevauche Depth prepass + Shadow maps) :
  C1  Wind.Update                 tex3D 64×32×64, bruit + rafales + obstacles
  C2  Ocean.Spectrum              h̃(k,t) pour 3 cascades
  C3  Ocean.IFFT (lignes, colonnes) → déplacement XYZ + pentes + Jacobien (écume)
  C4  Ocean.Mips/Foam
  C5  ShallowGrid.InjectHulls     déplacement/vitesse des coques
  C6  ShallowGrid.Advect          semi-lagrangien (MacCormack en ultra)
  C7  ShallowGrid.Divergence → Pressure (Jacobi ×N ou multigrille) → Project
  C8  ShallowGrid.Boundaries      couplage avec l'océan FFT au bord
  C9  Buoyancy.Reduce             forces/couples par coque → buffer readback
──────────────────────────────────────────────────────────────────────────────────
GRAPHICS (après Depth prepass) :
  G1  Rain.OcclusionDepth         depth vue du ciel (orthographique, orientée vent) [1 frame sur 4]
  G2  Rain.Simulate (compute)     émission/mise à jour particules, test d'occlusion
  G3  Wetness.Update (compute)    accumulation/séchage, flaques (heightmap)
  G4  GBuffer                     matériaux PBR incluent Wet.hlsli (aucun coût passe dédiée)
  G5  Water.Surface               rendu océan + grille + Gerstner lointain
  G6  Clouds.March  (¼ ou ½ rés.) ray marching, 1 pixel sur 16 mis à jour par frame (option)
  G7  Clouds.Reproject            reprojection temporelle + rejet par voisinage
  G8  Lighting / SSR              les flaques réutilisent le SSR et les sondes du moteur
  G9  Clouds.Composite            avant transparents, avec atmosphère/brouillard
  G10 Rain.Draw                   quads étirés, alpha, après transparents
──────────────────────────────────────────────────────────────────────────────────
Fin de frame : copie readback flottabilité, résolution timestamps
```

Synchronisation : une fence async → graphics avant G5 (surface d'eau) et avant G2 (la pluie lit le vent). Le vent est lu par la végétation dans le GBuffer : il est donc prêt bien avant.

---

## 4. Budget GPU (cible 4,0 ms, préréglage Ultra, 1440p, GPU de référence)

| Passe | Bas | Moyen | Ultra | Remarques |
|---|---:|---:|---:|---|
| Vent (C1) | 0,02 | 0,03 | 0,05 | 32×16×32 → 64×32×64 |
| Océan FFT (C2–C4) | 0,15 | 0,30 | 0,45 | 2×128² → 3×256² cascades |
| Grille eulérienne (C5–C8) | 0,10 | 0,25 | 0,40 | 128² / 20 Jacobi → 256² / 40 Jacobi |
| Flottabilité (C9) | 0,02 | 0,03 | 0,05 | |
| Surface d'eau (G5) | 0,35 | 0,50 | 0,65 | tessellation/maillage projeté, écume, réfraction |
| Nuages march + reproj (G6–G7) | 0,55 | 0,90 | 1,25 | ¼ rés. + checkerboard 4×4 → ½ rés. |
| Composite nuages (G9) | 0,05 | 0,08 | 0,10 | upsample bilatéral |
| Pluie sim + draw (G1–G2, G10) | 0,10 | 0,20 | 0,35 | 16k → 128k particules |
| Humidité/flaques (G3) | 0,03 | 0,05 | 0,08 | coût PBR mouillé dilué dans le GBuffer |
| **Total** | **1,37** | **2,34** | **3,38** | |
| Marge | 2,6 | 1,7 | **0,62** | absorbe les pics (orages, plusieurs coques) |

Les passes async compute (≈1 ms en Ultra) se recouvrent en grande partie avec le travail graphique : le coût réel sur la frame est généralement de 60 à 70 % de la somme.

---

## 5. Plan de fichiers

```
meteo-eau/
├── 00_ARCHITECTURE.md                ← ce document
├── include/Weather/
│   ├── WeatherSystem.h               point d'entrée, cycle de vie, ordre des passes
│   ├── WeatherState.h                état météo interpolé + presets (clair, orage…)
│   ├── WeatherHost.h                 interface IWeatherHost à implémenter par le moteur
│   ├── QualitySettings.h             préréglages Bas/Moyen/Ultra
│   ├── GpuTypes.h                    handles RHI minces (Texture, Buffer, Pipeline)
│   ├── Water/
│   │   ├── OceanFFT.h                spectre, cascades, IFFT
│   │   ├── ShallowWaterGrid.h        grille eulérienne locale
│   │   ├── HullInteraction.h         coques, sillage, flottabilité
│   │   └── WaterSurface.h            rendu + LOD (grille / FFT / Gerstner)
│   ├── Clouds/
│   │   ├── CloudNoiseBaker.h         Perlin-Worley 3D, carte météo
│   │   └── VolumetricClouds.h        march, reprojection, composite
│   ├── Atmos/
│   │   ├── WindField.h               champ 3D
│   │   ├── RainSystem.h              particules + occlusion
│   │   └── WetnessSystem.h           humidité, flaques, séchage
│   └── Runtime/
│       ├── SimulationLOD.h           rayons, transitions
│       ├── WeatherStreamer.h         DirectStorage, priorités caméra
│       └── GpuProfiler.h             timestamps par passe, budget
├── src/                              un .cpp par .h ci-dessus
│   └── Backend/D3D12/                seul dossier dépendant de l'API
│       ├── D3D12Device.cpp
│       └── D3D12Streaming.cpp        DirectStorage
└── shaders/
    ├── Common/
    │   ├── WeatherCommon.hlsli       constantes partagées C++/HLSL, échantillonnage vent
    │   ├── Noise.hlsli               Perlin, Worley, hash
    │   ├── FFT.hlsli                 butterfly radix-2 / Stockham en groupshared
    │   └── WetSurface.hlsli          à inclure par les matériaux PBR
    ├── Water/
    │   ├── OceanSpectrum_cs.hlsl     initial h0(k) + évolution h(k,t)
    │   ├── OceanFFT_cs.hlsl          IFFT horizontale puis verticale
    │   ├── OceanAssemble_cs.hlsl     déplacement, normales, Jacobien, écume
    │   ├── ShallowInject_cs.hlsl
    │   ├── ShallowAdvect_cs.hlsl
    │   ├── ShallowPressure_cs.hlsl   divergence, Jacobi, projection
    │   ├── ShallowBoundary_cs.hlsl
    │   ├── Buoyancy_cs.hlsl
    │   └── WaterSurface_vs_ps.hlsl
    ├── Clouds/
    │   ├── CloudNoiseBake_cs.hlsl
    │   ├── CloudMarch_cs.hlsl
    │   ├── CloudReproject_cs.hlsl
    │   └── CloudComposite_ps.hlsl
    └── Atmos/
        ├── WindUpdate_cs.hlsl
        ├── RainSimulate_cs.hlsl
        ├── RainDraw_vs_ps.hlsl
        └── WetnessUpdate_cs.hlsl
```

---

## 6. Interfaces du cœur (C++20)

### 6.1 Couche RHI minimale

Le module n'appelle jamais D3D12 directement en dehors de `Backend/`. Si ton moteur a déjà un RHI, on remplace `GpuTypes.h` par des alias vers tes types.

```cpp
// include/Weather/GpuTypes.h
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace wx {

struct TextureHandle  { uint32_t id = UINT32_MAX; bool valid() const { return id != UINT32_MAX; } };
struct BufferHandle   { uint32_t id = UINT32_MAX; bool valid() const { return id != UINT32_MAX; } };
struct PipelineHandle { uint32_t id = UINT32_MAX; };

enum class Format : uint8_t { R16F, RG16F, RGBA16F, R32F, RG32F, RGBA32F, R8, RGBA8 };
enum class Queue  : uint8_t { Graphics, AsyncCompute };

struct TextureDesc {
    uint32_t width = 1, height = 1, depth = 1, mips = 1, arraySize = 1;
    Format   format = Format::RGBA16F;
    bool     uav = true;
    std::string_view debugName;
};

// Enregistreur de commandes : volontairement minuscule.
class ICommandList {
public:
    virtual ~ICommandList() = default;
    virtual void setPipeline(PipelineHandle) = 0;
    virtual void setConstants(const void* data, uint32_t bytes) = 0;     // root CBV
    virtual void bindSRV(uint32_t slot, TextureHandle) = 0;
    virtual void bindUAV(uint32_t slot, TextureHandle) = 0;
    virtual void bindSRV(uint32_t slot, BufferHandle) = 0;
    virtual void bindUAV(uint32_t slot, BufferHandle) = 0;
    virtual void dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;
    virtual void uavBarrier(TextureHandle) = 0;
    virtual void beginPass(std::string_view name) = 0;   // timestamp + marqueur PIX
    virtual void endPass() = 0;
};

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;
    virtual TextureHandle  createTexture(const TextureDesc&) = 0;
    virtual BufferHandle   createBuffer(uint32_t stride, uint32_t count, bool uav, std::string_view name) = 0;
    virtual BufferHandle   createReadbackBuffer(uint32_t bytes, std::string_view name) = 0;
    virtual PipelineHandle createComputePipeline(std::string_view shaderFile, std::string_view entry) = 0;
    virtual void           destroy(TextureHandle) = 0;
    virtual void           destroy(BufferHandle) = 0;
    virtual const void*    mapReadback(BufferHandle) = 0;   // valide seulement si la fence est passée
};

} // namespace wx
```

### 6.2 État météo

```cpp
// include/Weather/WeatherState.h
#pragma once
#include <DirectXMath.h>

namespace wx {
using float3 = DirectX::XMFLOAT3;

// Tout ce qui pilote le module. Interpolable linéairement champ par champ.
struct WeatherState {
    float  cloudCoverage   = 0.35f;  // [0,1] seuil appliqué à la carte météo
    float  cloudDensity    = 0.6f;   // multiplicateur d'extinction
    float  cloudType       = 0.4f;   // 0 stratus → 1 cumulonimbus (profil vertical)
    float3 windDirection   = {1,0,0};// normalisé, plan XZ
    float  windSpeed       = 6.0f;   // m/s à 10 m d'altitude (U10, utilisé par JONSWAP)
    float  gustStrength    = 0.3f;   // [0,1]
    float  precipitation   = 0.0f;   // [0,1] → mm/h via courbe
    float  temperatureC    = 15.0f;  // séchage, (neige plus tard ?)
    float  sunIntensity    = 1.0f;   // séchage solaire, éclairage nuages
    float  fetchKm         = 300.0f; // JONSWAP : distance sur laquelle le vent a soufflé

    static WeatherState lerp(const WeatherState& a, const WeatherState& b, float t);
};

// Transition douce entre deux états sur une durée donnée.
class WeatherTimeline {
public:
    void  setTarget(const WeatherState& target, float transitionSeconds);
    void  update(float dt);
    const WeatherState& current() const { return m_current; }
private:
    WeatherState m_from{}, m_to{}, m_current{};
    float m_t = 1.0f, m_duration = 0.0f;
};
} // namespace wx
```

### 6.3 Interface vers le moteur hôte

```cpp
// include/Weather/WeatherHost.h
#pragma once
#include "GpuTypes.h"
#include <DirectXMath.h>

namespace wx {

struct CameraView {
    DirectX::XMFLOAT4X4 view, proj, viewProj, prevViewProj, invViewProj;
    DirectX::XMFLOAT3   position, forward;
    float               nearZ, farZ;
    uint32_t            width, height;
    uint32_t            frameIndex;          // pour le jitter / la reprojection
};

// Ce que le moteur fournit au module.
class IWeatherHost {
public:
    virtual ~IWeatherHost() = default;
    virtual IRenderDevice& device() = 0;
    virtual TextureHandle  sceneDepth() = 0;           // depth de la frame courante
    virtual TextureHandle  sceneColor() = 0;           // HDR, pour composite et réfraction
    virtual TextureHandle  motionVectors() = 0;        // optionnel, sinon reprojection par depth
    // Physique : le module renvoie les forces, le moteur les applique à ses corps rigides.
    virtual void applyBuoyancy(uint32_t hullId, const DirectX::XMFLOAT3& force,
                               const DirectX::XMFLOAT3& torque) = 0;
};
} // namespace wx
```

### 6.4 Réglages qualité

```cpp
// include/Weather/QualitySettings.h
#pragma once
#include <cstdint>

namespace wx {
enum class QualityLevel : uint8_t { Low, Medium, Ultra };

struct QualitySettings {
    // Eau
    uint32_t oceanCascades, oceanFFTSize;     // 2/128, 3/128, 3/256 (puissances de 2 uniquement)
    uint32_t gridSize, jacobiIterations;      // 128/20, 192/30, 256/40
    bool     maccormackAdvection;
    // Nuages
    float    cloudResolutionScale;            // 0.25, 0.25, 0.5
    uint32_t cloudMaxSteps, cloudLightSteps;  // 48/4, 64/6, 96/6
    bool     cloudCheckerboard;               // mise à jour 1/16 des pixels par frame
    // Atmos
    uint32_t windRes[3];                      // 32×16×32 … 64×32×64
    uint32_t maxRainParticles;                // 16k, 48k, 128k
    uint32_t wetnessMapSize;                  // 512, 1024, 2048

    static QualitySettings preset(QualityLevel);
};
} // namespace wx
```

### 6.5 Point d'entrée

```cpp
// include/Weather/WeatherSystem.h
#pragma once
#include "WeatherState.h"
#include "WeatherHost.h"
#include "QualitySettings.h"
#include <memory>

namespace wx {
class OceanFFT; class ShallowWaterGrid; class HullInteraction; class WaterSurface;
class CloudNoiseBaker; class VolumetricClouds;
class WindField; class RainSystem; class WetnessSystem;
class SimulationLOD; class WeatherStreamer; class GpuProfiler;

// Ressources publiques que d'autres systèmes (végétation, tissu, VFX, matériaux) lisent.
struct WeatherPublicResources {
    TextureHandle windVolume;       // RGBA16F : xyz vitesse m/s, w turbulence
    TextureHandle wetnessMap;       // RG8 : x humidité, y profondeur de flaque
    TextureHandle oceanDisplacement[3];
    BufferHandle  weatherConstants; // WeatherFrameConstants, b-slot réservé
};

class WeatherSystem {
public:
    WeatherSystem(IWeatherHost& host, QualityLevel q);
    ~WeatherSystem();
    WeatherSystem(const WeatherSystem&) = delete;
    WeatherSystem& operator=(const WeatherSystem&) = delete;

    void setQuality(QualityLevel q);                 // recrée les ressources dépendantes
    WeatherTimeline& timeline() { return m_timeline; }

    // CPU, début de frame.
    void update(float dt, const CameraView& cam);

    // Enregistrement GPU, dans l'ordre de la section 3.
    void recordAsyncCompute(ICommandList& cmd);      // C1–C9
    void recordPreGBuffer(ICommandList& cmd);        // G1–G3
    void recordWater(ICommandList& cmd);             // G5
    void recordClouds(ICommandList& cmd);            // G6–G7
    void recordCompositeAndRain(ICommandList& cmd);  // G9–G10

    const WeatherPublicResources& resources() const { return m_public; }
    const GpuProfiler& profiler() const { return *m_profiler; }

private:
    IWeatherHost&     m_host;
    QualitySettings   m_quality;
    WeatherTimeline   m_timeline;
    WeatherPublicResources m_public;

    std::unique_ptr<WindField>        m_wind;
    std::unique_ptr<OceanFFT>         m_ocean;
    std::unique_ptr<ShallowWaterGrid> m_grid;
    std::unique_ptr<HullInteraction>  m_hulls;
    std::unique_ptr<WaterSurface>     m_waterSurface;
    std::unique_ptr<CloudNoiseBaker>  m_noiseBaker;
    std::unique_ptr<VolumetricClouds> m_clouds;
    std::unique_ptr<RainSystem>       m_rain;
    std::unique_ptr<WetnessSystem>    m_wetness;
    std::unique_ptr<SimulationLOD>    m_lod;
    std::unique_ptr<WeatherStreamer>  m_streamer;
    std::unique_ptr<GpuProfiler>      m_profiler;
};
} // namespace wx
```

### 6.6 Constantes partagées C++ ↔ HLSL

Une seule définition, incluse des deux côtés, pour éviter les décalages de padding (piège n°1 des modules GPU).

```hlsl
// shaders/Common/WeatherCommon.hlsli  (inclus aussi en C++ via #include + macros)
#ifndef WEATHER_COMMON_HLSLI
#define WEATHER_COMMON_HLSLI

#ifdef __cplusplus
  #include <DirectXMath.h>
  #define float2   DirectX::XMFLOAT2
  #define float3   DirectX::XMFLOAT3
  #define float4   DirectX::XMFLOAT4
  #define float4x4 DirectX::XMFLOAT4X4
  #define uint     uint32_t
  #define CBUFFER_BEGIN(name) struct alignas(16) name {
  #define CBUFFER_END };
#else
  #define CBUFFER_BEGIN(name) struct name {
  #define CBUFFER_END };
#endif

// Règle : regrouper par 16 octets, jamais un float3 suivi d'un float3.
CBUFFER_BEGIN(WeatherFrameConstants)
    float4x4 viewProj;
    float4x4 prevViewProj;
    float4x4 invViewProj;
    float3   cameraPos;        float  time;
    float3   windDir;          float  windSpeed;
    float3   sunDir;           float  sunIntensity;
    float    cloudCoverage;    float  cloudDensity;  float cloudType;  float precipitation;
    float    globalWetness;    float  temperatureC;  float deltaTime;  uint  frameIndex;
    float3   windVolumeOrigin; float  windVolumeCellSize;   // volume centré caméra, aligné sur la grille
CBUFFER_END

#ifndef __cplusplus
// Échantillonnage du vent utilisable partout (végétation, tissu, particules).
Texture3D<float4> g_WindVolume : register(t0, space1);
SamplerState      g_LinearClamp : register(s0, space1);

float4 SampleWind(float3 worldPos, WeatherFrameConstants c)
{
    float3 uvw = (worldPos - c.windVolumeOrigin) / (c.windVolumeCellSize * float3(64, 32, 64));
    return g_WindVolume.SampleLevel(g_LinearClamp, uvw, 0);
}
#endif
#endif
```

---

## 7. Pièges d'architecture à éviter dès maintenant

- **Padding des constantes** : un `float3` suivi d'un `float3` en HLSL ne s'aligne pas comme en C++. D'où le header unique et un `static_assert(sizeof(...) % 16 == 0)` côté C++.
- **Volumes centrés caméra** (vent, grille d'eau, humidité) : les déplacer par **pas entiers de cellule** et décaler le contenu (« toroidal addressing »), sinon les données glissent et scintillent.
- **Async compute** : attention aux transitions d'état des ressources partagées entre files (COMMON obligatoire en D3D12 pour certaines transitions inter-files). Prévoir un mode « tout sur la file graphique » pour déboguer.
- **Readback de flottabilité** : jamais attendre la fence dans la même frame. Ring buffer de 3 et latence assumée de 2 frames, compensée par extrapolation côté physique.
- **Pas de temps** : simulation d'eau à pas fixe (ex. 1/60 s, sous-pas si dt plus grand), sinon instabilité dès qu'une frame dure plus longtemps.
- **Précision du temps** : `time` en float perd en précision après quelques heures. Le passer modulo la période de répétition de la FFT (ex. `time % 1000`) pour l'océan.

---

## 8. Suite prévue

1. **Eau** : maths (Phillips/JONSWAP, IFFT, Navier-Stokes en eau peu profonde), structures C++, shaders HLSL complets, dispatches, pièges.
2. **Nuages** : bake Perlin-Worley, ray marching, Beer-Lambert, Henyey-Greenstein double lobe, powder, reprojection.
3. **Vent, pluie, surfaces mouillées**.
4. **Optimisation** : LOD de simulation, streaming DirectStorage, profileur et préréglages.
