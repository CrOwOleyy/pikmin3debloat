# Pikmin 3 Deluxe - Native Windows 11 Port & Static Recompilation

Projet de reverse engineering, décompilation et recompilation statique de **Pikmin 3 Deluxe** (Nintendo Switch, binaire AArch32) vers un exécutable natif Windows 11 (x86_64 / Vulkan).

> **Avertissement légal :** Ce projet ne contient aucun fichier protégé par le droit d'auteur (ROM, NSP, NCA, RomFS, clés de chiffrement ou binaires propriétaires). Vous devez dumper votre propre copie légale du jeu et vos clés (`prod.keys`).

---

## Architecture du projet

- `src/` :
  - `src/lifter/` : Lifter CPU ARM32 (A32) vers C avec validation différentielle (Capstone + Unicorn vs binaire compilé MSVC).
  - `src/shader/` : Traducteur de shaders Nvidia Maxwell SASS vers GLSL / SPIR-V (basé sur le compilateur de shaders de `ssbu-recomp` avec correctifs).
- `scripts/` :
  - `scripts/setup_tools.ps1` : Script d'installation automatique des outils externes (Ghidra, SwitchLoader, nstool, nvdisasm, nx2elf, etc.).
  - `scripts/bahs_extract.py` : Extraction des shaders depuis les archives de shaders du jeu (`.sharcb` / conteneur BAHS).
  - `scripts/sass2glsl.py` : Pipeline expérimental de décompilation des shaders.
  - `scripts/ExportStats.java`, `ExportSyms.java`, `ExportIsaStats.java` : Scripts headless Ghidra pour exporter l'inventaire des fonctions, symboles et statistiques ISA.
- `notes/` :
  - Documentation technique détaillée (`CONTEXT.md`, `EMULATION_SURFACE.md`, `LIFTER_PLAN.md`, `EXISTING_TOOLS.md`).
  - Inventaire des symboles, fonctions identifiées, imports dynamiques et chaîne de caractères.

---

## Installation des outils

Exécutez le script PowerShell pour télécharger et installer tous les outils requis :

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup_tools.ps1
```

Ce script prépare :
1. L'environnement Python virtuel (`tools/venv`) avec Capstone et Unicorn.
2. Ghidra 12.1.2 PUBLIC (`tools/ghidra/`).
3. L'extension Ghidra-Switch-Loader 1.6.1 (`Adubbz/Ghidra-Switch-Loader`).
4. `nstool` v1.9.2 (`Jakcron/nstool`).
5. `nvdisasm` 12.9.88 (Nvidia CUDA redistributable).
6. Le dépôt source `nx2elf` (`shuffle2/nx2elf`).
7. Le dépôt de référence `ssbu-recomp` (`Denkishi/ssbu-recomp`).

---

## État d'avancement

- [x] Extraction NSP, exefs, romfs et cartographie initiale
- [x] Analyse Ghidra headless (binaire AArch32 identifié, ~45 000 fonctions, inventaire des imports nn::* et NVN)
- [x] Extraction complète des shaders Maxwell SASS (2 618 shaders traduits et validés en SPIR-V)
- [x] Lifter ARM32 vers C (prototype validé par tests différentiels Unicorn, support appels, imports PLT, jump tables clang)
- [ ] Support des instructions scalaires VFP et vectorielles NEON
- [ ] Runtime minimal & chargeur (relocations R_ARM_RELATIVE, stubs d'initialisation)
- [ ] Couche de compatibilité Horizon OS (services audio `nn::audio`, `nn::os`, `nn::hid`)
- [ ] Traduction NVN vers Vulkan
