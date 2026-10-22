; Variante « cadre de fenêtre » (programme 3) du shader de sommets nw::lyt :
; uv0 = (i ou 1-i) * taille du morceau (uTransform.xy) * uRcpTexSize0, miroirs selon
; uFrameSpec (bit 17 = horizontal, bit 18 = vertical). Le reste est identique à lyt.vsh.
;
; Shader de sommets de remplacement pour nw::lyt (lyt_BuildinShader.gsh, abîmé dans le dump).
; Écrit à la main d'après les noms et les valeurs que le jeu envoie (voir README.md du dossier).
;
; Un pane est un quad unitaire : aVertexIndex = (0,0) (1,0) (0,1) (1,1), coins HG HD BG BD.
;   local  = (uTransform.z + i.x * uTransform.x, uTransform.w - i.y * uTransform.y, 0, 1)
;   vue    = uModelView[0..2] . local          (matrice 3x4, lignes)
;   pos    = uProjection . vue                 (matrice 4x4, lignes)
;   couleur= bilinéaire(uVertexColor[0..3], i) * uColor
;   uv0    = bilinéaire(uVertexTexCoord0[0..3], i).xy, puis (u,v) = (uTexMtx0_xz, uTexMtx0_yw) . (s,t,0,1)
;
; Registres de constantes (vec4) : C0-3 uProjection, C4-6 uModelView, C7 uTransform,
; C8 uFrameSpec, C9-14 uTexMtx{0,1,2}_{xz,yw}, C15 uColor, C16-19 uVertexColor,
; C20 uFrameSize, C21 uPaneSize, C22-25 uVertexTexCoord0, C26-29 uVertexTexCoord1,
; C30-33 uVertexTexCoord2, C34 uGeneratingTexCoord, C35 uRcpTexSize0.
;
; $MODE = "UniformRegister"
; $UNIFORM_VARS[0].NAME = "uProjection"
; $UNIFORM_VARS[0].TYPE = "mat4"
; $UNIFORM_VARS[0].OFFSET = 0
; $UNIFORM_VARS[1].NAME = "uModelView[0]"
; $UNIFORM_VARS[1].TYPE = "vec4"
; $UNIFORM_VARS[1].COUNT = 3
; $UNIFORM_VARS[1].OFFSET = 16
; $UNIFORM_VARS[2].NAME = "uTransform"
; $UNIFORM_VARS[2].OFFSET = 28
; $UNIFORM_VARS[3].NAME = "uFrameSpec"
; $UNIFORM_VARS[3].OFFSET = 32
; $UNIFORM_VARS[4].NAME = "uTexMtx0_xz"
; $UNIFORM_VARS[4].OFFSET = 36
; $UNIFORM_VARS[5].NAME = "uTexMtx0_yw"
; $UNIFORM_VARS[5].OFFSET = 40
; $UNIFORM_VARS[6].NAME = "uTexMtx1_xz"
; $UNIFORM_VARS[6].OFFSET = 44
; $UNIFORM_VARS[7].NAME = "uTexMtx1_yw"
; $UNIFORM_VARS[7].OFFSET = 48
; $UNIFORM_VARS[8].NAME = "uTexMtx2_xz"
; $UNIFORM_VARS[8].OFFSET = 52
; $UNIFORM_VARS[9].NAME = "uTexMtx2_yw"
; $UNIFORM_VARS[9].OFFSET = 56
; $UNIFORM_VARS[10].NAME = "uColor"
; $UNIFORM_VARS[10].OFFSET = 60
; $UNIFORM_VARS[11].NAME = "uVertexColor"
; $UNIFORM_VARS[11].COUNT = 4
; $UNIFORM_VARS[11].OFFSET = 64
; $UNIFORM_VARS[12].NAME = "uFrameSize"
; $UNIFORM_VARS[12].OFFSET = 80
; $UNIFORM_VARS[13].NAME = "uPaneSize"
; $UNIFORM_VARS[13].OFFSET = 84
; $UNIFORM_VARS[14].NAME = "uVertexTexCoord0"
; $UNIFORM_VARS[14].COUNT = 4
; $UNIFORM_VARS[14].OFFSET = 88
; $UNIFORM_VARS[15].NAME = "uVertexTexCoord1"
; $UNIFORM_VARS[15].COUNT = 4
; $UNIFORM_VARS[15].OFFSET = 104
; $UNIFORM_VARS[16].NAME = "uVertexTexCoord2"
; $UNIFORM_VARS[16].COUNT = 4
; $UNIFORM_VARS[16].OFFSET = 120
; $UNIFORM_VARS[17].NAME = "uGeneratingTexCoord"
; $UNIFORM_VARS[17].OFFSET = 136
; $UNIFORM_VARS[18].NAME = "uRcpTexSize0"
; $UNIFORM_VARS[18].OFFSET = 140
; $ATTRIB_VARS[0].NAME = "aVertexIndex"
; $ATTRIB_VARS[0].TYPE = "vec2"
; $ATTRIB_VARS[0].LOCATION = 0
; $SQ_PGM_RESOURCES_VS.NUM_GPRS = 10
; $SQ_PGM_RESOURCES_VS.STACK_SIZE = 1
; $SPI_VS_OUT_CONFIG.VS_EXPORT_COUNT = 1
; $NUM_SPI_VS_OUT_ID = 1
; $SPI_VS_OUT_ID[0].SEMANTIC_0 = 0
; $SPI_VS_OUT_ID[0].SEMANTIC_1 = 1
; $SQ_VTX_SEMANTIC_CLEAR = 0xFFFFFFFE
; $NUM_SQ_VTX_SEMANTIC = 1
; $SQ_VTX_SEMANTIC[0].SEMANTIC_ID = 0
; $VGT_VERTEX_REUSE_BLOCK_CNTL.VTX_REUSE_DEPTH = 14
; $VGT_HOS_REUSE_DEPTH.REUSE_DEPTH = 16

00 CALL_FS NO_BARRIER
01 ALU: ADDR(32) CNT(78)
   0  x: MULADD          R2.x, R1.x, C7.x, C7.z
      y: MULADD          R2.y, -R1.y, C7.y, C7.w
      z: MOV             R2.z, 0.0f
      w: MOV             R2.w, 1.0f
   1  x: DOT4            R3.x, R2.x, C4.x
      y: DOT4            ____, R2.y, C4.y
      z: DOT4            ____, R2.z, C4.z
      w: DOT4            ____, R2.w, C4.w
   2  x: DOT4            ____, R2.x, C5.x
      y: DOT4            R3.y, R2.y, C5.y
      z: DOT4            ____, R2.z, C5.z
      w: DOT4            ____, R2.w, C5.w
   3  x: DOT4            ____, R2.x, C6.x
      y: DOT4            ____, R2.y, C6.y
      z: DOT4            R3.z, R2.z, C6.z
      w: DOT4            ____, R2.w, C6.w
   4  w: MOV             R3.w, 1.0f
   5  x: DOT4            R4.x, R3.x, C0.x
      y: DOT4            ____, R3.y, C0.y
      z: DOT4            ____, R3.z, C0.z
      w: DOT4            ____, R3.w, C0.w
   6  x: DOT4            ____, R3.x, C1.x
      y: DOT4            R4.y, R3.y, C1.y
      z: DOT4            ____, R3.z, C1.z
      w: DOT4            ____, R3.w, C1.w
   7  x: DOT4            ____, R3.x, C2.x
      y: DOT4            ____, R3.y, C2.y
      z: DOT4            R4.z, R3.z, C2.z
      w: DOT4            ____, R3.w, C2.w
   8  x: DOT4            ____, R3.x, C3.x
      y: DOT4            ____, R3.y, C3.y
      z: DOT4            ____, R3.z, C3.z
      w: DOT4            R4.w, R3.w, C3.w
   9  x: ADD             R5.x, C17.x, -C16.x
      y: ADD             R5.y, C17.y, -C16.y
      z: ADD             R5.z, C17.z, -C16.z
      w: ADD             R5.w, C17.w, -C16.w
  10  x: MULADD          R5.x, R5.x, R1.x, C16.x
      y: MULADD          R5.y, R5.y, R1.x, C16.y
      z: MULADD          R5.z, R5.z, R1.x, C16.z
      w: MULADD          R5.w, R5.w, R1.x, C16.w
  11  x: ADD             R6.x, C19.x, -C18.x
      y: ADD             R6.y, C19.y, -C18.y
      z: ADD             R6.z, C19.z, -C18.z
      w: ADD             R6.w, C19.w, -C18.w
  12  x: MULADD          R6.x, R6.x, R1.x, C18.x
      y: MULADD          R6.y, R6.y, R1.x, C18.y
      z: MULADD          R6.z, R6.z, R1.x, C18.z
      w: MULADD          R6.w, R6.w, R1.x, C18.w
  13  x: ADD             R6.x, R6.x, -R5.x
      y: ADD             R6.y, R6.y, -R5.y
      z: ADD             R6.z, R6.z, -R5.z
      w: ADD             R6.w, R6.w, -R5.w
  14  x: MULADD          R5.x, R6.x, R1.y, R5.x
      y: MULADD          R5.y, R6.y, R1.y, R5.y
      z: MULADD          R5.z, R6.z, R1.y, R5.z
      w: MULADD          R5.w, R6.w, R1.y, R5.w
  15  x: MUL             R5.x, R5.x, C15.x
      y: MUL             R5.y, R5.y, C15.y
      z: MUL             R5.z, R5.z, C15.z
      w: MUL             R5.w, R5.w, C15.w
  16  x: AND_INT         R7.x, C8.x, (0x00020000, 0)
      y: AND_INT         R7.y, C8.x, (0x00040000, 0)
      z: ADD             R7.z, 1.0f, -R1.x
      w: ADD             R7.w, 1.0f, -R1.y
  17  x: CNDE_INT        R8.x, R7.x, R1.x, R7.z
      y: CNDE_INT        R8.y, R7.y, R1.y, R7.w
  18  x: MUL             R8.x, R8.x, C7.x
      y: MUL             R8.y, R8.y, C7.y
  19  x: MUL             R9.x, R8.x, C35.x
      y: MUL             R9.y, R8.y, C35.y
      z: MOV             R9.z, 0.0f
      w: MOV             R9.w, 1.0f
02 EXP_DONE: POS0, R4.xyzw
03 EXP: PARAM0, R5.xyzw NO_BARRIER
04 EXP_DONE: PARAM1, R9.xyzw NO_BARRIER
END_OF_PROGRAM
