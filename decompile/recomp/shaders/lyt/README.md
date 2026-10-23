# lyt_BuildinShader.gsh de remplacement

Le fichier du dump (`content/CMCmn/shader/lyt_BuildinShader.gsh`) est abîmé (un hexdump texte
l'a écrasé). Ces sources en reconstruisent un équivalent pour nw::lyt (fenêtres, images,
boutons de l'interface) :

```bat
recomp\build.bat --target latte_assembler
:: après une modification du combineur (gen_lyt_psh.py réécrit le code de lyt.psh) :
python recomp\shaders\lyt\gen_lyt_psh.py recomp\build\gpu\latte_assembler.exe
python recomp\shaders\lyt\make_lyt_gsh.py recomp\build\gpu\latte_assembler.exe recomp\content_override\CMCmn\shader\lyt_BuildinShader.gsh
```

`content_override/` est lu en priorité par le FS du runtime ; le vrai fichier, s'il est
retrouvé, s'y dépose à la place.

Ce qu'on sait (établi avec `--probe` et `--debug-colors`) :
- le jeu charge **30 programmes** (boucle de 0x1E dans 0x0345BBxx) et cherche les variables
  par leur nom (tables en 0x103DFAxx : type, emplacement, nom) ;
- un pane = quad unitaire `aVertexIndex` ; `uTransform` = (largeur, hauteur, x, y) du quad,
  y vers le haut ; `uModelView[0]` 3×4 et `uProjection` 4×4 par lignes ; `uVertexColor[4]`
  (0–255) × `uColor` (qui contient le 1/255 et l'alpha du pane) ; `uTexMtx0_xz/_yw` :
  u = xz · (s, t, 0, 1), v = yw · (s, t, 0, 1) ;
- `uIndirectMtx0/1` (pixels, C3/C4) : lignes de la matrice indirecte (voir plus bas) ;
- `uFrameSpec` est un entier : bit 17 = miroir horizontal, bit 18 = miroir vertical du morceau
  de cadre ; `uFrameSize`, `uPaneSize`, `uRcpTexSize0` (1/taille de texture) accompagnent les
  cadres ; `uGeneratingTexCoord` et `uTextureMode` sont des entiers (voir plus bas) ;
- programmes vus à l'écran : 3 = morceaux de cadre (`lyt_frame.vsh`), 12 = fond plein écran,
  14/15 = contenu de fenêtre et boutons. Les autres reçoivent le shader générique.

Shader de pixels : combineur de textures (`lyt.psh`, produit par `gen_lyt_psh.py`), puis
couleur = couleur du sommet × (`uInterpolateOffset` + P × `uInterpolateWidth`) - l'interpolation
noir/blanc des matériaux (Offset = « noir », Width = « blanc » − « noir ») ; c'est elle qui donne
leur teinte aux fenêtres et au curseur cyan. Les cadres bornent la coordonnée à 0,99 : au-delà
de la texture du coin, le bord est étiré.

`--psh=<fichier>` remplace le shader de pixels de tous les programmes par un shader de
diagnostic ; `--debug-colors` peint chaque programme d'une couleur unique (R = bits 0–1,
V = bits 2–3, B = bit 4) ; `--tag-programs` glisse une constante inutilisée (R20.w = 100 +
numéro) qui rend le programme d'un dessin lisible dans le désassemblage de RenderDoc.

## Programmes (0x0345F8F4, 0x034614F0)

Numéro = indice de base du matériau (+0x14) + 15 × variante. Indice de base, d'après le nombre
de textures (mot +0xC, 2 bits de poids fort) et les étages de combinaison :
1 = aucune texture, 2 = une texture ; 2 textures : 3 Replace/Interpolate, 4 Modulate, 5 Add,
6 Subtract, 7 AddSigned, 8 Dodge, 9 Burn, 10 Overlay ; 3 textures : 11 (ou une valeur
calculée plus haut) ; avec un étage Indirect/BlendIndirect/EachIndirect : 12 (2 textures),
13 et 14 (3 textures). `make_lyt_gsh.py` spécialise les bases 0 à 2 : elles ignorent
uTextureMode (voir plus bas) et la base 1 prend une texture blanche.

## uTextureMode : calcul côté jeu (0x03460AD0)

Pour 2 textures ou plus, un octet par étage de combinaison k (4 octets chacun) : octet k de la
valeur = `table[combineRgb]`, plus 0x80 si l'octet suivant (alpha) vaut 1. Table (0x103E0C68),
indexée par le mode Replace, Modulate, Add, AddSigned, Interpolate, Subtract, Dodge, Burn,
Overlay, Indirect, BlendIndirect, EachIndirect : 2, 3, 4, 6, 2, 5, 7, 8, 9, 0xE, 0xF, 0x10.
Sinon le nombre de textures (0 ou 1, entier), **seulement si l'octet +0x40 du programme est
non nul** (0x0345AE9C) : un matériau à une texture peut donc hériter du mode du matériau
précédent - d'où la spécialisation des programmes 0 à 2 (sans elle, le visage de la bulle du
HUD disparaissait sous le masque de la lentille dessinée juste avant).

Combineur (`lyt.psh`) : l'étage k combine le résultat P à la texture k+1 :
2 = P inchangé, 3 = P × T, 4 = P + T, 5 = P − T, 6 = P + T − 0,5, 9 = Overlay ; bit 0x80 :
alpha × alpha de T. 0xE–0x10 (Indirect) : (r, g, b, 1) de T · `uIndirectMtx0`/`uIndirectMtx1`
(C3, C4 ; par exemple (2, 0, 0, −1) et (0, −2, 0, 1)) décale uv0 avant la lecture de la
texture 0 - les bulles du HUD réfractent ainsi le décor flou capturé derrière elles. Dodge (7)
et Burn (8) ne sont pas encore traités.

## Coordonnées générées (uGeneratingTexCoord)

`uGeneratingTexCoord` (C34, un entier par jeu de coordonnées) non nul : `uVertexTexCoordk` ne
contient pas les 4 coins mais une projection, (s, t) = lignes 0 et 1 · (x, y, 0, 1) en
coordonnées locales du pane (par exemple (1/80, 0, 0, 0,5) pour un pane de 80 de large). Utilisé
pour le fond flou des bulles et leurs reflets.

## Textures rendues par le GPU (registre C255)

Une texture d'interface peut venir d'une cible de rendu (bulle de dialogue, lueur des titres) :
son axe V est alors inversé. Le pont (`gpu/bridge/decaf_bridge.cpp`, `afterSetPixelTexture`)
écrit `C255 = (1, -1)` dans ce cas, `(0, 1)` sinon ; `lyt.psh` calcule `v = C255.x + v * C255.y`.
