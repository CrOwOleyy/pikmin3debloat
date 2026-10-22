# lyt_BuildinShader.gsh de remplacement

Le fichier du dump (`content/CMCmn/shader/lyt_BuildinShader.gsh`) est abîmé (un hexdump texte
l'a écrasé). Ces sources en reconstruisent un équivalent pour nw::lyt (fenêtres, images,
boutons de l'interface) :

```bat
recomp\build.bat --target latte_assembler
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
- `uFrameSpec` est un entier : bit 17 = miroir horizontal, bit 18 = miroir vertical du morceau
  de cadre ; `uFrameSize`, `uPaneSize`, `uRcpTexSize0` (1/taille de texture) accompagnent les
  cadres ; `uGeneratingTexCoord`, `uTextureMode` sont des entiers pas encore exploités ;
- programmes vus à l'écran : 3 = morceaux de cadre (`lyt_frame.vsh`), 12 = fond plein écran,
  14/15 = contenu de fenêtre et boutons. Les autres reçoivent le shader générique.

Shader de pixels : couleur = couleur du sommet × (`uInterpolateOffset` + texture 0 ×
`uInterpolateWidth`) - l'interpolation noir/blanc des matériaux (Offset = « noir »,
Width = « blanc » − « noir ») ; c'est elle qui donne leur teinte aux fenêtres et au curseur cyan.
Les cadres bornent la coordonnée à 0,99 : au-delà de la texture du coin, le bord est étiré.

Limites : `uTextureMode` (entier, un octet par étage de texture semble-t-il : valeurs vues
0x8E82, 0x8E02, 0x8E, 0x8E83, 0x2, 0xE), les textures 1–2 et les matrices indirectes sont
ignorés ; un petit carré gris parasite reste près des boutons. `--psh=<fichier>` permet de
remplacer le shader de pixels de tous les programmes par un shader de diagnostic.

`make_lyt_gsh.py --debug-colors` remplace chaque shader de pixels par une couleur unique
(R = bits 0–1 du numéro, V = bits 2–3, B = bit 4) pour voir quel programme dessine quoi.

## uTextureMode : calcul côté jeu (0x03460AD0)

Envoyé seulement pour les matériaux à 2 textures ou plus (2 bits de poids fort du mot de
drapeaux du matériau, +0xC) ; sinon le jeu envoie le nombre de textures (0 ou 1). Pour chaque
étage de combinaison k (4 octets chacun) : octet k de la valeur = `table[combineRgb]`, plus
0x80 si l'octet suivant (alpha) vaut 1. Table (0x103E0C68), indexée par le mode
Replace, Modulate, Add, AddSigned, Interpolate, Subtract, Dodge, Burn, Overlay, Indirect,
BlendIndirect, EachIndirect : 2, 3, 4, 6, 2, 5, 7, 8, 9, 0xE, 0xF, 0x10. Pas encore exploité
par `lyt.psh` (texture 0 seulement).
