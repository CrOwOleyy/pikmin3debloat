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
- programmes vus à l'écran : 12 = fond plein écran, 14/15 = contenu de fenêtre et boutons.
  Tous reçoivent `lyt.vsh` (`lyt_frame.vsh` n'est plus utilisé, voir « Cadres de fenêtre »).

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
2 = Replace (étage 0 : P = T, par exemple la capture floue du fond derrière une fenêtre ; étage 1 : P inchangé, sinon le texte des dialogues perd sa couleur), 3 = P × T, 4 = P + T, 5 = P − T, 6 = P + T − 0,5, 9 = Overlay ; bit 0x80 :
alpha × alpha de T. 0xE–0x10 (Indirect) : (r, g, b, 1) de T · `uIndirectMtx0`/`uIndirectMtx1`
(C3, C4 ; par exemple (2, 0, 0, −1) et (0, −2, 0, 1)) décale uv0 avant la lecture de la
texture 0 - les bulles du HUD réfractent ainsi le décor flou capturé derrière elles. Dodge (7)
et Burn (8) ne sont pas encore traités.

## Coordonnées générées (uGeneratingTexCoord)

`uGeneratingTexCoord` (C34, un entier par jeu de coordonnées) non nul : `uVertexTexCoordk` ne
contient pas les 4 coins mais une projection, (s, t) = lignes 0 et 1 · (x, y, 0, 1) en
coordonnées locales du pane (par exemple (1/80, 0, 0, 0,5) pour un pane de 80 de large). Utilisé
pour le fond flou des bulles et leurs reflets.

## Orientation de V

nw::lyt donne des coordonnées dont v croît vers le haut de l'écran (`uTexMtx0_yw` = (0, -1, 0, 1),
soit v = 1 - t ; idem pour les coordonnées générées), alors que les textures, fichiers comme
cibles de rendu, ont leur ligne 0 en haut. `lyt.psh` retourne donc v pour les trois jeux de
coordonnées, avant le décalage indirect (dont le sens est ainsi le même en u et en v : la bulle
grossit le visage). Auparavant seules les cibles de rendu étaient retournées (registre C255 écrit
par le pont) : le visage de la bulle du HUD et les portraits du communicateur étaient à l'envers
(les deux moitiés des hexagones interverties dessinaient des « X »).

## Cadres de fenêtre (uFrameSpec)

Une fenêtre nw::lyt est dessinée en morceaux (haut, droite, bas, gauche, puis contenu), chacun
avec le programme de son matériau - pas seulement le programme 3 : les boîtes de dialogue
utilisent des programmes à 3 textures (halo, voile, lignes). `uFrameSpec` (C8, entier) :
bits 8-11 = morceau (8 à B, D = contenu), bit 17 = miroir horizontal, bit 18 = vertical, octet
bas non nul pour un morceau de cadre. Dans ce cas `lyt.vsh` remplace uv0 par
(i ou 1-i) × taille du morceau × `uRcpTexSize0`, borné à 0,99 : le coin garde sa taille et son
bord est étiré sur le reste du morceau. Le bornage se fait par pixel dans `lyt.psh` (uv0.z = 1) :
borné aux sommets, le dégradé du coin était étiré en biais sur toute la longueur. Auparavant ces morceaux étiraient tout le coin (bandes
grises, cadre décalé, bande noire en bas des dialogues).
