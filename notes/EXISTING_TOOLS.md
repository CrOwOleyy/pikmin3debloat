# Outils existants (recherche du 2026-10-19)

## Le dossier SSBU du pote (`C:\Users\youne\Downloads\SSBU\SSBU`)
Build compilé (pas de sources) : `SSBU.exe`, `jeu/ssbu_game_fl.dll` (576 Mo = le code du jeu recompilé), `jeu/nro_bin_eh_o2/lua2cpp_*.dll` (95 modules de scripts), `ssbu_glslang.dll`, `SDL3.dll`, cache `cache/vk/spv` (7 787 `.spv`), `config/graphics.ini` commenté (« gfx_options.c »). Les logs (`jeu/derniere_partie.log`) montrent `sdklift`, `nvn_vk`, et « shaders … with Maxwell machine code only: 6751 » → shaders Maxwell traduits au chargement.
Il correspond au projet public **Denkishi/ssbu-recomp** (GitHub, **GPL-3.0**, C, mis à jour le 2026-10-19). Pas d'exécution de l'exe : seuls les fichiers texte ont été lus.

## Denkishi/ssbu-recomp - ce qui est réutilisable pour Pikmin 3
| Partie | Fichiers | Réutilisable ? |
|---|---|---|
| Traducteur de shaders Maxwell → GLSL → SPIR-V | `runtime/mx_translate.c` (113 Ko), `mx_ops.h` (table d'opcodes générée depuis suyu), `vk_glslang_shim.c`, `build_glslang.bat` | **Oui, directement** : générique (flot de contrôle SSY/PBK/SYNC, prédicats, textures, BRX…). Remplace notre prototype `sass2glsl.py`. |
| NVN → Vulkan / OpenGL | `runtime/nvn_vk*.c` (~600 Ko), `nvn_gl.c`, `nvn_front.h` | **En grande partie** (même API NVN), mais structures et pointeurs à adapter au **guest 32 bits**. |
| Décodage des textures | `runtime/tex_decode.c` | Oui (formats Tegra/BNTX). |
| Services nn::* (~1 100 points d'entrée) | `runtime/nn*.c` (fs, audio, hid, time, account, applet…) | **Comme base**, à adapter : ABI 32 bits (tailles de pointeurs, layouts), SDK 8.2 vs 13.x. |
| Lifter CPU | `tools/lifter/recomp.py` (AArch64 → C, vérifié avec Unicorn) | **Non tel quel** : Pikmin 3 est en **ARM32/Thumb**. Il faut un lifter A32/T32 (on peut reprendre l'architecture : une fonction C par fonction du jeu, tests différentiels Unicorn). |

## Autres pistes trouvées
- **LASRecomp** (Zelda Link's Awakening Switch → PC, NVN → Vulkan/Metal) : autre recompilation statique.
- **suyu 0.0.4** : mode recompilation statique (AArch64 → x86-64), projet arrêté.
- **bnsh-decoder** (maierfelix) : BNSH → SPIR-V basé sur yuzu.
- **Ryujinx / yuzu** : recompilateurs de shaders Maxwell (runtime, GPL).
- **Port PC de Super Mario Odyssey** (WIP, outil non publié).

## Recommandation
Ne pas recréer le traducteur de shaders : **réutiliser `mx_translate.c` de ssbu-recomp** (tester d'abord sur nos 2 600 blobs). Pour le reste, s'appuyer sur son runtime (NVN→Vulkan, nn::*) en l'adaptant au 32 bits. Le vrai travail nouveau pour Pikmin 3 : **un lifter ARM32/Thumb** et l'adaptation ABI 32 bits du runtime.
Licence : ssbu-recomp est GPL-3.0. Usage personnel : aucun problème. Si le port est un jour diffusé, le code dérivé devra être publié sous GPL-3.0.

## Test de `mx_translate.c` sur les shaders de Pikmin 3 (2026-10-19)
- Clone : `tools/ssbu-recomp` (commit d876f64). Harnais : `scripts/mxharness.c`, compilé avec MSVC (`tools/mxharness/build.bat`) avec `runtime/mx_translate.c` seul, cible Vulkan (`target = 1`), sans contenu de c1 (`cdata = NULL`).
- Entrée : les 2 618 blobs des 6 archives AGL release (`work/mx_list.txt`). Résultats : `work/mx_results.tsv`, GLSL dans `work/mx_glsl/`.
- **Traduction : 2 617 / 2 618 OK** (332 vertex, 2 285 fragment). 1 échec : opcode `SHFL` (shuffle entre threads, non géré).
- **Compilation SPIR-V (glslangValidator, Vulkan 1.3) : 2 521 / 2 617 OK.** Les 96 échecs ont tous la même cause : `textureLodOffset` avec un **décalage de texel calculé dans un registre** (GLSL exige une constante). Correction possible dans le traducteur : décaler la coordonnée à la main (`coord + vec2(offset) / textureSize(...)`), ou passer par l'extension/capability SPIR-V `ImageGatherExtended`.
- **spirv-val** : 100 SPIR-V pris au hasard, 0 invalide.
- Non vérifié : rendu réel sur GPU (aucune exécution), correspondance des bindings (UBO = `c3+k`, textures `mx_t<u>`) avec ce que fera notre couche NVN→Vulkan, et les jump tables BRX (c1 non fourni).
- Conclusion : **pas besoin de recréer un traducteur de shaders** ; celui de ssbu-recomp couvre ~96 % des shaders de Pikmin 3 tel quel, et ~100 % après deux petits correctifs (offset dynamique, SHFL).

## Après correctifs (copie du projet `src/shader/`, voir `src/shader/README.md`)
- **2 618 / 2 618 shaders traduits**, **2 618 / 2 618 compilés en SPIR-V** (glslangValidator, Vulkan 1.3), **2 618 / 2 618 valides** (`spirv-val` sur la totalité).
- Correctifs : offset de texel dynamique → coordonnées décalées (96 shaders) ; `SHFL.BFLY` de quad → `subgroupQuadSwap*` ; `FSWZADD` → tables de suyu. Les deux derniers ne concernaient qu'un shader (`agl_technique_pfx` prog 004).
- Restent non vérifiés : rendu réel sur GPU ; exactitude de l'approximation d'offset en LOD implicite ; ordre des voies de quad Maxwell = Vulkan (hypothèse partagée avec suyu) ; support des opérations de quad par le pilote AMD de la Radeon 880M (très probable en fragment, non testé).
