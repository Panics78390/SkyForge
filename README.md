# SkyForge

SkyForge est un démonstrateur temps réel **DirectX 12 / HLSL / C++20** d'un module météo et eau pour moteur de jeu : océan FFT en cascades, nuages volumétriques, vent 3D, pluie GPU et surfaces mouillées. **Tout est réglable en direct** depuis un panneau, avec un profileur GPU par passe et un budget cible de 4 ms pour le module.

## Télécharger et lancer

1. Ouvrir l'onglet **Releases** du dépôt, version **« Dernière version »**, et télécharger `SkyForge-windows-x64.zip` (compilé automatiquement par GitHub à chaque modification).
2. Décompresser, puis lancer `SkyForge.exe` (le dossier `shaders` doit rester à côté).

Configuration requise : Windows 10/11 64 bits, carte graphique compatible DirectX 12 (NVIDIA GTX 900+, AMD RX 400+, Intel Arc / Iris Xe).

## Commandes

| Action | Touche |
|---|---|
| Regarder autour | clic droit maintenu + souris |
| Avancer / reculer / côtés | Z Q S D (ou W A S D) |
| Monter / descendre | Espace / C |
| Aller vite | Maj |
| Masquer le panneau | F1 |

## Ce qu'on peut régler

- **Préréglages météo** : Clair, Nuageux, Pluie, Tempête, avec transition douce (durée réglable).
- **Météo** : couverture, densité et type des nuages, direction et vitesse du vent, rafales, précipitations, température, fetch, brouillard.
- **Soleil** : élévation, azimut, intensité, course automatique, exposition.
- **Océan** : vagues pointues, écume, tailles des 3 cascades, rayons de LOD (précision max / Gerstner au-delà).
- **Nuages** : altitude, tailles des motifs, érosion, extinction, Henyey-Greenstein avant/arrière, effet powder, ambiance, poids de l'historique temporel.
- **Pluie** : zone, vitesse de chute, longueur des traits, luminosité.
- **Surfaces mouillées** : vitesses de mouillage et de séchage (base, vent, soleil), flaques, ondulations, vue de debug.
- **Qualité** Bas / Moyen / Ultra, activation de chaque sous-système, vitesse du temps, pause, vsync.
- **Profileur GPU** : temps de chaque passe comparé à son budget (vert = dans le budget, rouge = dépassement).
- **Aperçus** des textures internes (humidité, carte météo, occlusion de la pluie, déplacement et pentes de l'océan).
- **Recharger les shaders** à chaud : modifier un `.hlsl` du dossier `shaders` puis cliquer le bouton.
- **Enregistrer / Charger** : les réglages vont dans `skyforge.ini` (enregistrés aussi à la fermeture).

## Ordre des passes dans une frame

```
Compute : Vent 3D -> Océan (spectre, IFFT lignes, IFFT colonnes, assemblage) x cascades
          -> Pluie (carte d'occlusion vue du ciel, simulation) -> Humidité du sol
Rendu   : Scène (PBR mouillé) + Surface d'eau  [HDR, Z inversé, plan lointain infini]
          -> Nuages (ray marching basse résolution -> reprojection temporelle)
          -> Composition ciel + nuages -> Gouttes (profondeur en lecture seule)
          -> Tonemapping ACES -> Interface
```

## Budget GPU visé (ms)

| Passe | Bas | Moyen | Ultra |
|---|---:|---:|---:|
| Océan FFT | 0,15 | 0,30 | 0,45 |
| Surface d'eau | 0,35 | 0,50 | 0,65 |
| Nuages (march + reprojection) | 0,55 | 0,90 | 1,35 |
| Pluie (sim + rendu) | 0,10 | 0,20 | 0,35 |
| Vent + humidité | 0,05 | 0,10 | 0,13 |

## Fichiers

```
src/Gpu.*         couche DX12 : device, ressources, liaison automatique des descripteurs et transitions
src/Pipelines.*   compilation HLSL à l'exécution, PSO, profileur GPU (timestamps)
src/Weather.*     état météo, préréglages, transitions, réglages qualité, constantes partagées
src/Ocean.*       spectre JONSWAP, IFFT en cascades, LOD FFT -> Gerstner
src/Clouds.*      bruit Perlin-Worley 3D, ray marching, reprojection, composition
src/Atmos.*       vent 3D, pluie GPU, humidité, scène de démonstration
src/main.cpp      fenêtre, caméra, boucle de frame, panneau de réglages
shaders/          tous les shaders HLSL commentés (maths en tête de fichier)
docs/ARCHITECTURE.md  architecture cible complète du module
tools/check_shaders.ps1  vérification de tous les shaders avec fxc
```

## Compiler soi-même

Visual Studio 2022 (charge de travail « Développement Desktop C++ ») et CMake 3.20+ :

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

L'exécutable est dans `build/Release/`, avec une copie du dossier `shaders`. Dear ImGui est téléchargé automatiquement par CMake.

## Pas encore implémenté (prochaines étapes)

- Grille eulérienne locale (advection, projection de pression) et interaction coque ↔ eau avec flottabilité renvoyée au CPU.
- Streaming asynchrone DirectStorage des données météo et textures.
- File async compute (aujourd'hui tout passe par la file graphique).
- Réflexions SSR dans les flaques (actuellement : ciel analytique seulement).

## Pièges connus

- Les constantes C++/HLSL doivent garder exactement le même ordre et le même alignement (16 octets) : des `static_assert` les vérifient côté C++.
- Le temps de l'océan est ramené modulo 200 s (fréquences quantifiées) pour ne pas perdre de précision en float.
- Les volumes centrés caméra (vent, occlusion de pluie) sont alignés sur leur grille pour éviter le scintillement.
- Les écritures UAV en lecture-modification (écume, humidité) nécessitent le support des « typed UAV loads » (présent sur tous les GPU DX12 récents).
