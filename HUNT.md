# Caza del área de juego negra — estado al 2026-07-28 (sesión 2)

## Síntoma
El port arranca, suena, responde, los paneles laterales (reloj/batería, columnas)
se ven; el área de juego (816×544 pillarbox) jamás muestra un píxel — verificado
con capturas exactas del emulador (60 frames en 30 s, máx. de luminancia < 40).
Igual en Vulkan y en OpenGL de Vita3K.

## Herramientas nuevas (imprescindibles — todo por fichero-trigger)
Vita3K instrumentado en `~/Fun/vita3k`, rama **ff6-debug** (commit 6740f75),
binario en `build/macos/bin/Vita3K.app`. Configurar/compilar:
`cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build/macos --target vita3k -j8`
(necesita Qt6 ≥6.11: `brew install qt`, ya instalado).

Con `B=~/Library/Application Support/Vita3K/Vita3K`:
- `touch "$B/screenshot.trigger"` → guarda el frame presentado en
  `$B/screenshots/Final Fantasy VI/*.jpg` (frame EXACTO del emulador, sin ventanas).
- `touch "$B/drawlog.trigger"` → vuelca los siguientes 80 draws a nivel GXM
  (stdout de Vita3K): vértices crudos del stream 0, primeras 4 floats del
  uniform buffer por defecto (fila 0 de la WVP), region clip, viewport,
  fragment-program mode, dirección de textura.
- `VITA3K_NO_SURFACE_TEX=1` (env) → desactiva el redirect
  surface-como-textura del camino Vulkan (para descartar esa heurística).

Loader (este repo):
- `touch "$B/fs/ux0:data/ff6"`… es decir `touch "$B/fs/ux0/data/ff6/dump.trigger"`
  → el SIGUIENTE frame se vuelca completo en `boot.log` (`[frm]`, con quads
  GL_FLOAT y GL_FIXED decodificados, matrices, scissor/alpha/depth/cull/fog/
  unidad activa/matriz de textura/UVs) y arma el log por-draw interno de vitaGL
  (`[vgl]`: máscara de variante FFP, textura, viewport sceGxm).
- Disparando los tres triggers a la vez se obtiene el MISMO frame en 3 capas →
  ver `frame_trace.md` (ejemplo ya capturado).

vitaGL en `~/Fun/vitaGL` (tu fork): compilar SIEMPRE
`make clean && make SHARED_RENDERTARGETS=1 NO_SPLASHSCREEN=1 LOG_ERRORS=1 -j8 && make … install`
(sin NO_SPLASHSCREEN la splash crea contexto gxm duplicado y crashea en Vita3K).
Lleva añadidos: `vgl_debug_scene_splits` (contador de splits de escena),
`vgl_debug_log_draws` (log por draw en `reload_ffp_shaders`).

## Hecho real encontrado y ARREGLADO esta sesión
`TextGlyphTexture::drawGlyph` del .so jamás llegaba a subir los glifos al atlas
(el texto entero del juego era invisible; el splash es logo + texto).
Reimplementado en el loader (`drawGlyphHook`, hook_addr sobre
`_ZN16TextGlyphTexture9drawGlyphEiPi`): sube el glifo w×h (stride fuente 1024)
a la celda `idx` del atlas (celda = `[this+0x40005C]`, página `[this+0x400064]`,
handle GL en `[self + página*4 + 4]`). Verificado: '©',dígitos,letras suben con
contenido a tex 6. OJO: el porqué el original no subía sigue sin explicar
(llega hasta ahí con idx≥0 y todo válido) — probablemente el mismo bug de fondo.

## Eliminado CON EVIDENCIA (no volver a probar)
- Estado GL en el draw: scissor OFF, alpha test OFF, depth OFF, stencil OFF,
  colormask 1111, cull OFF, fog OFF, lighting OFF, unidad activa TEXTURE0,
  matriz de textura identidad, UVs 0..1, matrices modelview/projection
  correctas (volcadas con glGetFloatv en el frame).
- Splits de escena GXM: 0 en frames del juego (solo los provocaban mis
  glReadPixels; en Vita3K un split mata el frame entero — bug del emulador y
  razón de que glReadPixels devuelva ceros).
- Cambio de viewport a mitad de frame (pin experimental: no cambia nada).
- Contenido de texturas: el juego sube contenido real (logo tex100 256×128
  brillante y opaco; muestreo con paso IMPAR — con paso múltiplo de 4 solo se
  ve un canal, error mío inicial).
- Caché de texturas de Vita3K (texture-cache: false → igual).
- Redirect surface-como-textura de Vita3K (0 hits + kill-switch → igual).
- Backend: Vulkan y OpenGL idénticos → no es el backend.
- Nivel GXM verificado draw a draw (capa 3): vértices exactos, WVP textbook
  (0.004167 = 2/480), viewport (480,408/272,-272, z 0.5+0.5), clip full,
  fragment mode ENABLED. El draw del logo llega PERFECTO a renderer::draw.
- Nota de Rinnegatamante: GL_FIXED en FFP es correcto (unpack en shader);
  los draws VISIBLES son los GL_FIXED, los invisibles los GL_FLOAT.

## SOSPECHOSO ACTUAL (siguiente paso concreto)
Diff dentro del renderer de Vita3K entre dos draws del MISMO frame
(frame_trace.md): STRIP de escena (fragprog 0x85518ad8, mask vitaGL
0x200000EF: MODULATE+color array+float) vs STRIP de panel (0x85e38388,
mask 0x250200C9: REPLACE+alpha test+fixed):
1. **Blend state del pipeline Vulkan** creado para cada SceGxmFragmentProgram
   (GXM hornea el blend al crear el programa vía shader patcher; si Vita3K
   traduce mal ESA combinación → RGB×0 con todo lo demás perfecto, en ambos
   backends…(el estado de blend GL se traduce del mismo sitio)). Loguear en
   la creación del pipeline (vulkan/pipeline_cache.cpp) los factores/op de
   blend + hash de shader, correlacionar con las direcciones de fragprog.
2. Fragment shaders traducidos: 16 SPIR-V decompilados a GLSL en el scratchpad
   de la sesión anterior (regenerables: `spirv-cross $B/cache/shaders/FF6000000/eboot.bin/vk13-*.spv --vulkan-semantics`).
   Falta mapear dirección fragprog→hash (loguear en cache_and_bind).

## Datos del frame de referencia (frame_trace.md)
- Draws 1–15 escena (invisibles), 16–25 paneles (visibles), viewport switch
  tras el 14.º; el "batch AGB" de 360 vértices es EL TEXTO (tex 6/7 = atlas
  de glifos, celdas de 19 px).
- El texto se dibuja MODULATE × color actual; los paneles REPLACE (por eso
  el alfa residual 0 no les afecta).
- Colores de arrays de escena: blanco opaco en los draws de fondo/logo del
  splash; alfa 0 en muchos otros momentos del bucle (fades legítimos).
