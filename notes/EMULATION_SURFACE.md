# Surface à émuler pour le port Windows (Pikmin 3 Deluxe, `main`)

Source : analyse Ghidra de `work/exefs/main` (ARM32, base 0x60000000, 45 017 fonctions). Données brutes : `notes/imports.txt` (608 imports), `notes/symbols.tsv`, `notes/strings.tsv`, `notes/functions.tsv`.
Build d'origine (chaîne trouvée) : projet interne `carrot3s`, Nintendo SDK NX Addon 8.2.0, lib `NX-NXFP2-a32`.

## 1. Fondations
- **Architecture :** AArch32 (ARM + Thumb), NSO unique `main`, plus `sdk` et `subsdk0` à analyser (le SDK Nintendo en partie là-dedans). Recompilation à cibler ARM32.
- **Runtime C/C++ :** libc musl (`__nnmusl_*`), libc++ (`std::...`), helpers EABI (`__aeabi_*`, 23). Ce sont des imports de bibliothèques : remplaçables directement par la libc/STL Windows, sans émulation.

## 2. Services Horizon (nn::*), par ordre de volume d'API
| Famille | Imports | À faire sur Windows |
|---|---|---|
| `nn::audio` | 134 | **Plus gros chantier non graphique** : audio renderer complet (voix, submix, finalmix, reverb, aux, sinks, memory pools, wave buffers). Réimplémenter un mixeur (ex. via WASAPI/miniaudio). Sons : `.bfstm` (129) et `CarrotAudio.bfsar`. |
| `nn::os` | 72 | Threads, mutex, events, message queues, multiwait, TLS, mémoire (QueryMemoryInfo, heap). Mappable sur les primitives Windows/C++. |
| `nn::hid` | 57 | Manettes Npad (états, six-axis, **vibration** : classes VibrationPlayer/Mixer…), clavier/souris/debugpad. XInput/SDL3 ; six-axis (gyro) utile pour viser. |
| `nn::socket` / `nifm` / `ssl` / `nsd` | 28/7/2/1 | Réseau (reliquat SDK, jeu quasi hors ligne) : stubs renvoyant « pas de réseau ». |
| `nn::fs` | 25 | **MountRom** → dossier `work/romfs` ; **MountSaveData** → dossier de sauvegarde local. Simple. |
| `nn::util` | 16 | Utilitaires purs (BinaryFileHeader, RelocationTable, SNPrintf, UTF-8) : réimplémentables/portables. |
| `nn::image` | 16 | Encodeur/décodeur JPEG (captures) : libjpeg/stb. |
| `nn::account` | 14 | Profil utilisateur : stubs (un utilisateur factice). |
| `nn::mem` | 10 | `StandardAllocator` : réimplémentation simple. |
| `nn::oe` | 8 | Mode portable/docké, langue (`GetDesiredLanguage`), CPU boost, notifications : stubs. |
| `nn::prepo`, `nn::album`, `nn::time`, `nn::crypto`, `nn::lmem`, `nn::vi`, `nn::diag`, `nn::err`, `nn::settings` | 1–7 chacun | Télémétrie (prepo), captures (album), horloge, MD5/SHA1, fenêtre (`vi` → fenêtre Win32/SDL) : stubs ou petits wrappers. |

## 3. Graphique : le gros morceau
- API GPU : **NVN** (≈517 fonctions `nvn*` référencées : Command 144, Texture 96, Sampler 48, Queue 34, Device 30, Window 24, Memory/Buffer/Blend/Depth…). Chargée dynamiquement via `nvnBootstrapLoader` (table de pointeurs de fonctions) → le port fournit sa propre table NVN et traduit en **Vulkan**.
- Fenêtre : `nn::vi` (5 fonctions) → fenêtre Windows + swapchain Vulkan.
- **Shaders :** le jeu inclut `glslc*` (compilateur GLSL de NVN), mais aucun source GLSL (`#version`) dans les chaînes → les shaders sont des **binaires NVN pré-compilés** (format GPU Maxwell, dossier `.gsh`/`.ntx` dans le romfs). Il faudra soit un traducteur de binaires shader Maxwell → SPIR-V (gros chantier, c'est ce que font les émulateurs), soit récupérer/reconstruire les shaders. **Point de risque n°1.**
- Formats d'assets Nintendo (lus par le moteur lui-même, donc rien à émuler côté OS) : `.szs` (3 108, archives SARC/Yaz0), `.bfres`, `.bntx` (textures), `.bfsar/.bfstm` (audio), `.mp4` (146 vidéos → décodeur à fournir, ex. Media Foundation/ffmpeg).

## 4. Taille et "debloat"
`work/romfs` ≈ 6,5 Go dont presque tout dans `CMCmn` (commun). Les dossiers de langue (USEng, EUFre, JPJpn…) ne pèsent que 4–12 Mo chacun : l'essentiel du poids est dans les assets communs (vidéos mp4, audio, `szs`), pas dans les langues.

## 5. Risques et ordre de travail conseillé
1. **Shaders NVN binaires** (risque majeur) – décider tôt de la stratégie.
2. Recompilation statique ARM32 → x86-64 de 45 017 fonctions (outil à trouver/écrire ; code quasi strippé : 1 208 noms).
3. Audio renderer (134 API) puis HID/vibration puis le reste en stubs.
4. Analyser `sdk` et `subsdk0` (non encore chargés dans Ghidra) : une partie du SDK s'y trouve et peut changer la liste des imports.

## 6. Prochaines étapes concrètes
- Importer `sdk` et `subsdk0` dans le projet Ghidra et regénérer les listes.
- Inspecter les fichiers shader du romfs (`.gsh`, `.ntx`) et identifier le format exact.
- Chercher un recompilateur statique ARM32 existant ; sinon prototyper sur un petit sous-ensemble de fonctions.

## 7. Addendum : shaders (inspection du romfs)
- Le seul `.gsh` (`CMCmn/system/primitive_renderer_cafe.gsh`, 2,2 Ko, magic `Gfx2`) est un **reliquat Wii U (GX2/Cafe)** : probablement inutilisé sur Switch.
- Les vrais shaders Switch sont dans : `CMCmn/system/agl_root/agl_resource.Nin_NX_NVN.release.sarc` (10,5 Mo, SARC contenant 6 archives `*.sharcb` : technique, lght, lght_lpp, pfx, shdw, common), `CMCmn/system/primitive_drawer_nvn_shader.bin`, `tmp/nvn_font_shader_jis1.bin` et `CMCmn/nw_shader/nwsh_model.szs` / `nwsh_proc.szs`. Ce sont les archives de shaders du moteur **AGL** de Nintendo (binaires NVN précompilés, pas de source GLSL).
- Conséquence : il faudra décoder le format `sharcb` et traduire le code GPU NVN (Maxwell) en SPIR-V/GLSL, ou retrouver une voie de contournement. Le sous-ensemble minimal (`agl_resource_min`, 1,2 Mo) est un bon point de départ.
- Ghidra : `sdk` et `subsdk0` sont maintenant importés (sans analyse) dans `work/ghidra_project/Pikmin3`.

## 8. Format `sharcb` (BAHS) - premier décodage (`work/shaders/agl_common.sharcb`, extrait de `agl_resource_min`)
- En-tête (u32 little-endian) : `'BAHS'`, version `9`, taille du fichier (`0x11e000`), nombre `5` (programmes ?), `0`, `0x1000`, `0xCC000`, `0x52000`. Le fichier est aligné par pages de 0x1000 ; `0xCC000 + 0x52000` = taille du fichier.
- Table de chaînes à `0x150` : nom de l'archive (`agl_common`), `RegisterUBO`, attributs (`aPos`, `aNrm`, `aTexCoord`), uniformes (`cWorldViewMatrix[0]`, `cDiffuseColor0`…) → **la réflexion (noms de variables) est lisible**, utile pour reconstruire les pipelines.
- Région `0xCC000`–fin : **code machine Maxwell (SASS)** - motif répétitif de mots d'ordonnancement (`…1f00`) tous les 4 instructions, typique du GPU Tegra X1. Région `0x52000` commence par la constante `0x12345678` (section de contrôle NVN).
- Aucun SPIR-V, GLSL, ni magic NVN reconnaissable (`NVNs`, `NVNg`, `GFLG`) dans ce fichier : le format est l'emballage propre à AGL autour du binaire GLSLC.
- **Conclusion :** pour afficher quoi que ce soit il faudra traduire du SASS Maxwell vers SPIR-V. Des recompilateurs de shaders existent dans les émulateurs Switch open source (licences GPL : les consulter comme référence ; en réutiliser le code rend le dérivé GPL - pas de problème pour un usage strictement personnel, mais à garder en tête si le projet est un jour partagé).
- Suite possible : écrire un parseur Python du conteneur BAHS (offsets de chaque programme, stage vertex/fragment, tailles) et extraire chaque programme SASS dans des fichiers séparés.

## 9-10. Format BAHS résolu (`scripts/bahs_extract.py`)
(Corrige les hypothèses du §8 : la région de code est `@0x1c`=début, `@0x18`=taille, soit `0x52000 + 0xCC000` pour `agl_common`.)
- En-tête : `'BAHS'`, version 9, taille fichier, n, 0, `0x1000`, **code_size @0x18**, **code_off @0x1c**.
- Zone `0x1000..code_off` : un descripteur par programme, contenant un en-tête GLSLC (`0x98761234`) ; le dword à `magic-96` = offset du programme **relatif à code_off**.
- Zone de code : blobs alignés sur 0x100, chacun commençant par `0x12345678` (en-tête NVN de 0x30 octets) puis du SASS Maxwell ; un blob par étage (vertex/fragment/…). Le tag à `blob+0x30` distingue l'étage : `0x02060461` (63) et `0x00061462` (146) sont les plus fréquents (vertex / fragment probables, à confirmer).
- Vérification : 100 % des octets de la zone de code sont attribués (225 blobs = 0xCC000 octets dans `agl_common`). Le script traite les 6 archives de `agl_resource.release` : 126 + 345 + 245 + 57 + 166 + 432 = **1 371 programmes, ~2 600 blobs SASS** (sorties dans `work/shaders/**/*_out/`, `index.json` par archive).
- Noms de variables : seules les références qui pointent exactement sur un début de chaîne sont résolues ; la plupart des programmes n'en ont pas dans leur descripteur (réflexion probablement dans le blob ou la table de chaînes globale) → à creuser.
- Bonus : `primitive_drawer_nvn_shader.bin` (7 168 o, format v8) embarque en clair un **listing assembleur NV (`MOV.F result_color0, fragment.attrib[0]; END`)** avant le SASS : si ce listing existe aussi dans d'autres `.bin`, il facilitera la traduction vers SPIR-V. Absent des `.sharcb` (aucune occurrence).
- Prochaine étape : décoder un blob SASS (désassembler Maxwell) et valider une traduction SASS → SPIR-V sur le plus petit programme.

## 11. Structure d'un blob de code
- `+0x00` en-tête NVN (magic `0x12345678`, 0x30 octets).
- `+0x30` **SPH Maxwell** (Shader Program Header, 0x50 octets). Les 5 bits bas du 1er mot = type : `0x61` → 1 = **vertex** (VTG), `0x62` → 2 = **fragment** (PS). Les tags `0x02060461` / `0x00061462` sont donc vertex / fragment.
- `+0x80` **instructions SASS Maxwell** (groupes de 4 × 8 octets : 1 mot de contrôle d'ordonnancement + 3 instructions), terminées par `EXIT` (`…e3`) puis `BRA` sur soi et des `NOP` (`…b050`) de remplissage.

## 12. Désassemblage (`scripts/sass_disasm.py`, nvdisasm 12.9.88 `-b SM53`)
- nvdisasm (outil NVIDIA orienté CUDA) n'accepte que **455 blobs sur 2 618** (~17 %) : il rejette (« Illegal instruction ») dès qu'un shader contient une instruction purement graphique (ALD, textures TEX/TEXS…). Les `.sass` produits sont à côté des blobs dans `work/shaders/**/*_out/`.
- Exemple réussi : `agl_common` prog 001 (fragment) = `IPA.PASS a[0x7c]` (1/w), `MUFU.RCP`, 4× `IPA a[0x80..0x8c]` → `R0..R3`, `EXIT` : recopie l'attribut 0 dans la couleur de sortie.
- Il faut un désassembleur qui gère les instructions graphiques Maxwell : **envydis** (envytools, MIT, cible `gm107`), à compiler sous Linux (Docker ou WSL).

## 13. Désassemblage complet avec envydis (Docker)
- Image Docker `envydis:f102b823` (envytools, MIT, commit f102b823 ; Dockerfile `tools/envydis-docker/Dockerfile`). WSL non touché.
- Lancement : `docker run --rm -v <work>:/w -v <scripts>:/s --entrypoint python3 envydis:f102b823 /s/envydis_all.py /w/shaders` → un fichier `.envy` à côté de chaque blob.
- Résultat : **tous les blobs sont désassemblés**. Les `???` restants sont à 96 % du **remplissage à zéro** après `EXIT`/`BRA`/`NOP` (fin de blob alignée sur 0x100). Seulement ~1 950 instructions inconnues avant `EXIT` sur ~600 000 (≈0,3 %), surtout de la famille `0x508…` (variantes de `MUFU`) et quelques `0x7a0/0x7b0/0x5d2`.
- Contenu typique : `ld a[...]` (attributs vertex), `st a[...]` (sorties), `ipa` (interpolation fragment), `c3[...]` (UBO), `tex`/`texs` (≈24 000 échantillonnages de textures), beaucoup de `ffma/fmul/fadd`. Quelques blobs contiennent plusieurs `exit` (branches), un seul en contient 39.
- Exemple vertex (`agl_common` prog 000) : lit position/normale (`a[0x80..]`, `a[0x90..]`), les multiplie par des matrices de `c3`, normalise (`mufu rsq`), écrit les sorties.
- Prochaine étape : choisir la stratégie SASS → SPIR-V (traducteur maison ou réutiliser un recompilateur de shaders existant comme référence) et prototyper sur prog 001 (fragment) + prog 000 (vertex) de `agl_common`.

## 14. Prototype SASS → GLSL → SPIR-V (`scripts/sass2glsl.py`)
- Entrée : un `.envy` (envydis). Sortie : GLSL 450 Vulkan, compilé par `glslangValidator -V --target-env vulkan1.3` (Vulkan SDK 1.4.363) puis **validé par `spirv-val`**.
- Résultats dans `work/glsl/` : `agl_common_000.vert(.spv)` et `agl_common_001.frag(.spv)` - **les deux compilent et passent la validation SPIR-V**.
- Ce que font ces shaders (reconstitué) : le vertex lit position (`vin0` = a[0x80]) et normale (`vin1` = a[0x90]), transforme la normale par une matrice 3×3 de `cb3` puis la normalise, projette la position par une matrice 4×4 de `cb3` (lignes `0x30..0x6c`, vraisemblablement `cWorldViewProjectionMatrix`), calcule un éclairage diffus avec un vecteur de `cb3[0xa0..]` (`cViewLightDir`) et interpole entre deux couleurs (`cDiffuseColor0/1` à `0x70`/`0x80`) → `vout0`. Le fragment recopie `vin0` dans `color0`. Cohérent avec les noms de la table de chaînes d'`agl_common`.
- Couverture : seulement `ld/st/ipa/mufu/fmul/fadd/ffma/mov/exit`, code linéaire. Manquent : branches (`bra/ssy/sync`, prédicats `$pN`), entiers (`isetp/lop/shl/iscadd…`), conversions (`f2i/i2f/f2f`), textures (`tex/texs` ≈ 24 000 occurrences), `fsetp/fmnmx`, `depbar`.
- Hypothèses à vérifier : correspondance a[0x80+16k] → location k (doit venir de l'état d'attributs NVN du jeu), sortie couleur = r0..r3 (à lire dans l'omap du SPH), conventions de clip NVN vs Vulkan (axe Y, profondeur [-1,1] vs [0,1]), liaison UBO (cb3 → binding 3).
- Conclusion : la chaîne complète (extraction → désassemblage → traduction → SPIR-V valide) fonctionne sur des cas simples. Pour 2 600 shaders il faudra un vrai traducteur (graphe de flot, prédicats, textures) : soit étendre ce prototype, soit s'inspirer d'un recompilateur de shaders Maxwell existant.
