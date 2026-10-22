# Correctifs des outils tiers

Les outils tiers ne sont pas dans le dépôt (`tools/` est ignoré) : on clone la version amont
indiquée, puis on applique le correctif correspondant.

| Outil | Amont | Commit | Correctif |
|---|---|---|---|
| decaf-emu (GX2, libgpu, licence GPL-3.0) | https://github.com/decaf-emu/decaf-emu | `e6c528a20a41c34e0f9eb91dd3da40f119db2dee` | `decaf-emu.patch` |
| XenonRecomp (recompilateur PPC → C++) | https://github.com/hedge-dev/XenonRecomp | `ddd128bcca99fe8bfbb99bea583c972351fa6ace` | `XenonRecomp.patch` |
| wiiurpxtool | https://github.com/0CBH0/wiiurpxtool | `6ff41a43201dcfa126b4b00379db39e94de64037` | `wiiurpxtool.patch` |
| Vulkan-Headers | https://github.com/KhronosGroup/Vulkan-Headers | tag `v1.3.268` | aucun |

```bash
git clone --recursive https://github.com/decaf-emu/decaf-emu tools/decaf-emu
git -C tools/decaf-emu checkout e6c528a20a41c34e0f9eb91dd3da40f119db2dee
git -C tools/decaf-emu apply ../../patches/decaf-emu.patch
```

Contenu des correctifs :
- **decaf-emu** : `GX2InitFetchShaderEx` masque `location` sur 7 bits (attribut absent = −1) ;
  option d'affichage `display.rotate180` (contournement temporaire, voir `recomp/README.md`).
- **XenonRecomp** : instructions PPC32/Gekko (paired-singles, lmw/stmw, etc.), tables de saut GHS,
  imports Cafe OS, `lwarx` volatile - détail dans `recomp/README.md`.
- **wiiurpxtool** : `<io.h>` remplacé par `<cstdio>` (compilation hors MSVC).
