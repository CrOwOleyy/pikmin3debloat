# src/shader - traducteur de shaders Maxwell → GLSL (Vulkan)

Copie de `runtime/mx_translate.c`, `mx_translate.h` et `mx_ops.h` de **Denkishi/ssbu-recomp** (commit d876f64, licence **GPL-3.0**, voir `LICENSE.GPL-3.0`). Les modifications propres au port Pikmin 3 sont marquées `Pikmin 3 port` dans le code :

1. **TEX.AOFFI à décalage dans un registre** (96 shaders) : GLSL Vulkan exige une constante pour `textureOffset` / `textureLodOffset`. Pour `target == 1`, le décalage est appliqué aux coordonnées : `coords + vec(offset) / vec(textureSize(sampler, niveau))` (`tex_dyn_offset_coords`). Le niveau est la LOD explicite, sinon 0 : **approximation** pour un échantillonnage à LOD implicite sur une texture mipmappée.
2. **SHFL** : seul le papillon de quad (fragment, `BFLY`, voie immédiate 1..3, `c = 0x1c03`) est géré, via `subgroupQuadSwapHorizontal/Vertical/Diagonal` (`GL_KHR_shader_subgroup_quad`). Le reste échoue toujours.
3. **FSWZADD** : addition « swizzlée » dans un quad (dérivées), tables de suyu `A = (-1, 1, -1, 0)`, `B = (-1, -1, 1, -1)`, voie = `gl_SubgroupInvocationID & 3`. NDV / CC non gérés.

Test : `tools/mxharness/build.bat` puis `mxharness.exe work/mx_list.txt work/mx_glsl`.
