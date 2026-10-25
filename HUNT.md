# Notas del port — estado al 2026-07-29 (sesión 4)

> **Registro histórico; revisión para release de octubre de 2026.** Las notas
> de abajo se conservan tal como quedaron y contienen hipótesis y estados ya
> superados. El estado vigente y los criterios de aceptación están en
> [README.md](README.md) y [docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md).
> La candidata sigue pendiente de validación en consola.

Puntos que hay que tener presentes al leer este registro:

- Sí hubo pruebas en Vita real: el usuario confirmó Narshe fluido con
  `inplacetex=1`. Falta verificar los menús, el combate y los bordes del scroll
  después del último arreglo del puntero de colores.
- El código incluye la traducción de flags Bionic y rutas de guardado
  (`034c0c4`) y la corrección de lectura de todas las ranuras (`d603fcd`). La
  sección antigua que dice que el quicksave falla antes de escribir no prueba
  el estado actual: sigue pendiente un ciclo completo de guardar, salir,
  arrancar y cargar, además de guardado rápido y reanudación.
- Los únicos filtros admitidos son `bilinear=0` y `bilinear=1`; no hay opción
  `aspect`. `offscreen=0` y `fixedcolor=0` son los valores de referencia. Las
  alternativas experimentales no cuentan como verificadas por compilar.
- El cambio de filtrado de octubre conserva `GL_LINEAR` en las dos páginas
  de atlas de glifos registradas incluso con `bilinear=0`; el resto sigue la
  opción general. Los handles se retiran al borrar o sustituir una página.
  La ruta de texto completo `createTextTexture`/`drawText00` aún no se
  clasifica como atlas. Hay regresión de host de la vida de los handles y
  traza debug de MIN/MAG `LINEAR` para ambas páginas con `bilinear=0`;
  la nueva build release también muestra legible el menú de reanudación,
  sin letras perdidas ni solapamiento observado en esa pantalla. No se
  aceptó la reanudación ni se validaron otras escenas con ese cambio.
- La comparación con el checkout local de FF3 encontró las mismas fuentes
  Noto y fuentes de stb por hash, pero `bilinear=0` allí conserva los filtros
  pedidos por el motor. No se identificó un shader sharp-bilinear en ese
  checkout. El blit offscreen de FF6 usa `GL_LINEAR` ordinario; no implementa
  sharp-bilinear y no corrige una contaminación entre celdas ya compuesta.
- La revisión de octubre vuelve a usar tamaño EM con
  `stbtt_ScaleForMappingEmToPixels`: `fontscale=100` es el tamaño que pide el
  motor Android. Se corrigen el descarte completo de glifos con bordes fuera
  del bitmap, el límite artificial de subida de 64 píxeles y el wrap de fila
  del atlas en el límite de capacidad. Hay regresiones de host y un menú de
  reanudación revisado en Vita3K sin letras perdidas ni solapamiento observado;
  faltan otras escenas y la validación en consola de esta versión.
- No reutilizar el factor histórico 1,36 como medida de la fuente actual.
  La JP incluida tiene ascent/descent `1160/-288` y `1000` unidades por EM:
  el cambio desde pixel-height a EM da un factor de escala `1,448` sin el
  límite de seguridad. El nuevo `fontscale=69` aproxima el antiguo `100`;
  no es una calibración contra una captura Android. La caja vertical usa
  ascent y descent redondeados por separado de Noto.
- Los resultados de Vita3K documentan la investigación, pero no sustituyen
  las pruebas de guardado, gráficos, rendimiento y suspensión en consola.

---

## Sesión 4: el rendimiento de Narshe, CAZADO (pendiente de confirmar en consola)

### La causa: la orfandad de texturas de vitaGL, no los draws ni las subidas en sí
El compositor AGB mete tiras de tiles con `glTexSubImage2D` en DOS texturas de
**1024x2048 RGBA8 (8 MB cada una)** — medido con el desglose `[glsub]` del
loader: `3(1024x2048x4):212 4(1024x2048x4):212` por ventana de 60 frames
durante el paseo por el pueblo. Y `_glTexSubImage2D` de vitaGL
(`textures.c:982`), si la textura se dibujó en los últimos `FRAME_PURGE_FREQ=4`
frames, la **huerfanea: reserva una superficie nueva del tamaño COMPLETO,
copia los 8 MB enteros (RAM sin caché en ambos lados), y difiere la vieja al
recolector** — todo antes de aplicar el rectángulo de 128 KB. Medido con el
estimador `orphan=` de la línea `[gl]`: **~5,1 MB de copia por frame** durante
el scroll (una textura de 8 MB huerfaneada cada ~1,6 frames; llegan en ráfagas
al entrar fila nueva de tiles). A las tasas de memcpy sin caché de la Vita eso
son 15-40 ms/frame solo en copias, más el churn de alloc/free de bloques de
8 MB. Ahí está el 10-20 fps de Narshe.

Por qué las pistas anteriores no lo veían:
- El **throttle de CPU** de Vita3K (abajo) no lo reproduce: la copia son pocas
  instrucciones y muchos stalls de memoria; Dynarmic la ejecuta a velocidad de
  memcpy del host. A 111 MHz el pueblo aguantaba 60 fps mientras una escena de
  118 draws caía a 10 — el perfil opuesto al hardware, y la pista decisiva de
  que el coste no era de instrucciones.
- `vglUseCachedMem` no ayudó porque cambia el TIPO de los pools, no la copia
  de 8 MB por frame ni su churn.
- El dato "a 1x pasaba de 10 a 60 fps" que apuntaba a coste de fragmento **era
  falso** (desmentido por el usuario esta sesión).

### El arreglo: `vglInPlaceTexSubImage` (fork de vitaGL) + `inplacetex=1`
Nuevo ajuste runtime en el fork (`vgl.c`, `textures.c`, `vitaGL.h`,
`lookup.c`): con `vglInPlaceTexSubImage(GL_TRUE)`, `glTexSubImage2D` escribe
la superficie EN EL SITIO y no huerfanea nunca. Solo toca ese bloque: los
purge lists, `last_frame` y los borrados seguros quedan como estaban (a
diferencia del `TEXTURES_SPEEDHACK` de compilación, que también hace
inmediatos los free de texturas que la GPU aún puede leer — demasiado
agresivo). El loader lo activa tras `vglInitExtended` con la opción
`inplacetex` de options.cfg (**por defecto 1**; `inplacetex=0` restaura la
orfandad para el A/B en consola). El riesgo teórico es que la GPU muestree
mezcla de texels viejos y nuevos durante un frame; las tiras que escribe el
compositor entran por el borde del scroll, así que no deberían estar
muestreadas por los frames en vuelo. Verificado en Vita3K que arranca y el
pueblo se ve bien; **el veredicto de fps y de artefactos es de consola**.
Candidato a PR upstream de vitaGL (opt-in, gana en cualquier app que streamee
rects a texturas grandes).

### Herramienta nueva: limitador de CPU invitada en Vita3K (`VITA3K_CPU_MHZ`)
Fork `~/Fun/vita3k` rama ff6-debug (`cpu/src/dynarmic_cpu.cpp`): con la
variable de entorno puesta se enciende el contador de ciclos de Dynarmic y
cada hilo invitado se frena a esa frecuencia — cuantos de 1 ms de tiempo
invitado, durmiendo el resto del ms de pared; al reanudar tras una espera no
acumula crédito (rebase de la ventana). Sin la variable no cambia nada.
OJO: son ticks de Dynarmic (~instrucciones), no ciclos de Cortex-A9, y sin
stalls de memoria — sirve para comparar coste de CPU relativo entre escenas,
no para reproducir cuellos de memoria (por eso el pueblo pasaba el test).
A 444 MHz todas las escenas medidas van a 60 fps salvo hipos de carga.

### Instrumentación nueva del loader (build FF6_DEBUG)
- Línea `[gl]`: añade `orphan=<KB>/f(<n>/f)` (estimación del tráfico de
  orfandad que vitaGL haría en consola: primera sub-subida del frame a cada
  textura = una copia de superficie completa) y `redup=<n>/60f` (subidas
  idénticas a una reciente: mismo id, rect y contenido, anillo de 32 hashes).
  Durante el scroll real la redundancia es baja (6/60) — el grueso con la
  pantalla quieta — así que saltarse repetidas NO es el arreglo.
- Línea `[glsub]` cada 60 frames: qué texturas recibieron subs y su tamaño
  completo (`id(WxHxbpp):n`), que es lo que cuesta cada orfandad.
- `glCompressedTexImage2DHook` registra ahora dimensiones en `tex_info` (los
  atlas nacen paletizados y no pasaban por `glTexImage2DHook`).
- **Bug propio arreglado**: el rewrite del bind-skip de la sesión 3 borró la
  asignación `gl_bound_texture = texture` — TODA la contabilidad por textura
  de hoy caía en el id 0 hasta arreglarlo. Y de paso el skip-cache quedó
  blindado: `glDeleteTexturesHook` lo invalida si se borra la textura atada
  (el id puede reutilizarse y el skip se comería un bind real) y
  `glActiveTextureHook` lo invalida al cambiar de unidad.

### Regresión en consola: menús de personajes y combate con texturas/texto rotos
Reportada por el usuario tras confirmar que Narshe ya va fluido con
`inplacetex=1`. Revisión completa de los parches del loader hecha (sesión 4);
**sospechoso número uno, ya arreglado**: `expand_fixed_array` reconvertía el
array de colores `GL_FIXED` en CADA draw mientras `GL_COLOR_ARRAY` siguiera
habilitado — y `drawImagePriBlend` sale dejándolo habilitado apuntando a su
propia pila muerta. En Vita3K esa pila conserva valores plausibles (por eso
allí se veía bien); en consola cualquier interrupción la machaca → colores
por vértice basura en menús y combate, que son exactamente los caminos que
pasan por `drawImagePriBlend`. Ahora se convierte UNA vez por registro del
puntero; los draws posteriores reutilizan lo convertido y el exceso se
rellena con blanco opaco (identidad bajo MODULATE). El campo/mapa no usa
ese camino y no se ve afectado. Commit "Never reconvert GL_FIXED colours
from a stale pointer". La historia entera está ahora troceada en commits por
feature para poder bisectar si no fuera esto (rama `main` reescrita; la
antigua sobrevive en `backup-pre-restructure`).
De la misma revisión: fuera el `force_agb_alpha` (probe de sesión 2 que
seguía armado en builds de debug), y el skip-cache de binds quedó blindado
contra borrado de textura atada y cambio de unidad (ids reutilizados podían
comerse un bind real).

### Pendiente de la sesión 4
1. **Probar en consola** (a mano, sin red): fps en Narshe con `inplacetex=1`
   vs `inplacetex=0`, y mirar artefactos en los bordes del scroll.
   **Hecho por red con el usuario delante: Narshe fluido confirmado.**
   Pendiente el A/B de artefactos y la validación de menús/combate tras el
   arreglo del puntero muerto.
2. Si queda corto: el resto del coste por sub-subida es el propio write de
   128 KB a RAM sin caché; opciones: pool de staging + `sceGxmTransferCopy`,
   o `vglUseCachedMem` AHORA SÍ (con la orfandad fuera, la copia restante es
   el write del rect, que cacheado + flush podría salir mejor).
3. `vglInPlaceTexSubImage` como PR a vitaGL upstream.
4. GL_FIXED en `glColorPointer` de vitaGL: HECHO en el fork (caso `GL_FIXED`
   declarado F32 + campo `fixed_color` en `shader_mask` + `fixed_mode_color`
   con `GLFixed4ToFloat4` en `ffp_v.h`/`ffp_ext_v.h`; requiere size 4, que es
   lo que GLES1 exige). El loader lo usa con `fixedcolor=1` en options.cfg
   (default 0 = conversión en el loader, que sigue funcionando en vitaGL de
   serie). **VALIDADO en Vita3K**: combate real con `fixedcolor=1`, sprites y
   colores perfectos y cero `GL_INVALID_ENUM` con `LOG_ERRORS`
   (captura `…2026-07-29-070211.jpg`). PR upstream junto al punto 3.

---
# Notas de la sesión 3 (2026-07-28)

## Trabajo de la sesión 3, después de arreglar la pantalla negra

### Compilación: la instrumentación ya es opcional
`cmake -S . -B build -DFF6_DEBUG=ON` compila con toda la instrumentación
(contadores GL, volcado por frame, sondas, watchdog, boot.log, autoplay).
Sin la opción — build normal — todo eso desaparece: `trace()` es un inline
vacío, los hooks de diagnóstico se sustituyen por la función real vía la macro
`DBG_ALT`, y no se crea `boot.log`. Verificado: el eboot release es ~121 KB
más pequeño y no toca `boot.log`.

### Texto: los glifos se dibujaban NEGROS (arreglado)
`rasterize_text()` en bridge.c escribía solo cobertura (`alpha << 24`) dejando
RGB a 0, con un comentario que afirmaba que el `.so` ORea su propio RGB encima.
**Ese comentario era falso.** El volcado del ARGB que llega a `drawGlyph` da
`0xXX000000` en todos los píxeles: el motor no toca el bitmap. Como el glifo se
dibuja MODULATE contra el color actual, negro × cualquier cosa = negro, y todo
el texto del atlas (copyright, menús) era invisible. Medido en el título:
pico de luminancia 42 sobre un fondo de 35.
Arreglado escribiendo el RGB de `setTextColor` (que se ignoraba por completo)
en el bitmap, respetando el orden R,G,B,A en memoria de ARGB_8888 de Android.
Ahora: pico 255, 464 píxeles > 200. `setTextColor` además invalida el
rasterizado cacheado.

### Texto: la letra era 1.36x demasiado pequeña (arreglado)
`update_metrics()` usaba `stbtt_ScaleForPixelHeight`, que hace que
(ascent − descent) valga el tamaño pedido. `Paint.setTextSize()` de Android es
un tamaño de EM, que para Noto Sans es ~1.36x mayor — exactamente el factor que
medí contra la captura de referencia (2.02% del alto de pantalla frente a
2.76%). Cambiado a `stbtt_ScaleForMappingEmToPixels`. El motor deriva el tamaño
de celda del atlas de lo que devuelve `getTextFontSize()`, así que toda la
fuente escala con esto: celda 19 px → 27 px, altura medida 3.12%.

### Filtrado y escalado (opciones nuevas en options.cfg)
El build de Android pide `GL_LINEAR` en todo, lo que emborrona pixel art.
- `bilinear=0` (por defecto): `GL_NEAREST` en todo. Nítido.
- `bilinear=1`: `GL_LINEAR` en todo (comportamiento anterior).
- `bilinear=2`: el mundo se dibuja en un buffer intermedio a múltiplo entero
  (2x de 480x320 = 960x640) con NEAREST y se escala **una sola vez** al
  pillarbox de 816x544 con LINEAR. Muestreo exacto y un único paso filtrado.
  Los paneles laterales no entran: el motor los dibuja en espacio de pantalla
  con otro viewport, así que van directos y quedan exactos.
- `integerscale=1`: recorta el área de juego al mayor múltiplo entero del
  espacio de composición del motor. OJO: ese espacio es 480x320 y la pantalla
  544 de alto, así que el único múltiplo que cabe entero es 1x → 480x320
  centrado, que usa media pantalla. Por eso `bilinear=2` es la opción práctica
  para pantalla completa.

### ✅ Personajes y enemigos invisibles en combate — RESUELTO
**Causa: vitaGL no acepta `GL_FIXED` en `glColorPointer`.** El log de hardware
lo cantaba: `source/ffp.c:1795: glColorPointer set GL_INVALID_ENUM (type: 0x140C)`
— y 0x140C es `GL_FIXED`.

GLES 1.1 permite `GL_FIXED` (coma fija 16.16) en arrays de color, y el build de
Android lo usa para los sprites de combate: `drawImagePriBlend` @0x130488 llama
a `glColorPointer(4, GL_FIXED, 0, sp+80)`. El `switch` de vitaGL
(`source/ffp.c` ~1772) contempla FLOAT, SHORT, UNSIGNED_SHORT, BYTE y
UNSIGNED_BYTE, y cae al `default` con `SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM)`
**retornando sin asignar `attributes->format`**. El atributo de color se queda
con el layout del draw anterior → colores por vértice basura → sprites
invisibles. Afecta igual a personajes y enemigos, que es justo el síntoma.

vitaGL sí soporta GL_FIXED en el resto de atributos, pero con un truco: como la
GPU del Vita no tiene formato de vértice en coma fija, declara el atributo
`SCE_GXM_ATTRIBUTE_FORMAT_F32` y enciende un bit de máscara para que el vertex
shader generado lo desempaquete (`GLFixed2ToFloat2` / `GLFixed4ToFloat4` en
`source/shaders/ffp_v.h`, con `fixed_mode_pos` para posición y
`fixed_mode_mask` 0x01 normales / 0x02 y 0x04 texcoords). Para color no existe
ni el bit ni el caso.

**Arreglado en el loader, sin tocar vitaGL** (`glColorPointerHook` +
`apply_fixed_colours` en main.cpp): si el tipo es `GL_FIXED` no se le pasa a
vitaGL — se retiene, y en el draw se expande el rango dibujado a float
(`raw / 65536.0`) y se le entrega un array `GL_FLOAT` normal. Verificado en
Vita3K: el combate muestra a Terra y los dos soldados en Magitek, los enemigos
y los ataques, y desaparece el `GL_INVALID_ENUM` del log.

Arreglarlo *bien* sería en vitaGL: añadir el caso `GL_FIXED` a
`glColorPointer` con un bit libre de `ffp_vertex_attrib_fixed_mask` y su
correspondiente `#if` de desempaquetado en `ffp_v.h`. Es candidato a PR
upstream — el bug afecta a cualquier port GLES1 que use colores en coma fija.

**Y una trampa en ese arreglo, que costó un crash en hardware.** La primera
versión convertía siempre que el puntero fuese `GL_FIXED`. `drawImagePriBlend`
apunta el array de color a **su propio marco de pila** y sale dejando
`GL_COLOR_ARRAY` habilitado, así que cuando después llegaba un lote grande del
compositor AGB (más de mil vértices) se leía muy por encima de una pila ya
muerta: *data abort*. El volcado lo señaló exactamente —
`glDrawArraysFixed` en `main.cpp`, instrucción `vldr s15, [r2, #8]` con
`r2 = 0x8e200ff8`, ocho bytes antes del final de la página de pila.
La conversión ahora comprueba `glIsEnabled(GL_COLOR_ARRAY)` primero, que además
es lo semánticamente correcto: si el array no está en uso, no hay nada que
convertir.

### Quicksave: falla ANTES de tocar disco (abierto)
El usuario pulsa guardado rápido y el juego muestra "Quicksave failed.".
Medido, no supuesto:
- **No ocurre ningún `fopen` en modo escritura.** Se instrumentó `fopen_mark`
  para registrar toda apertura `w`/`a`: ni una sola en toda la sesión.
- Llamar directamente a `cMobileSaveLoadMngr::writeToFile()` (0x175b74) sobre
  una instancia válida **retorna normal y tampoco escribe nada**, así que no
  es cosa de la navegación de menús. Para reproducirlo: build `FF6_DEBUG` y
  `touch ux0:data/ff6/save.trigger`.
- **No falta ningún método JNI.** Los únicos sin resolver en toda la ejecución
  son `GetIsAndroidSignIn()Z` y `UnlockAchievement(...)V`, ninguno de guardado.
Conclusión: se rinde antes de intentar escribir. El quicksave es la *halt
data* del motor (`MobileSaveHalt` 0x1760d4 → `cMobileSaveLoadMngr::saveHalt`
0x175b90), no la ruta de ranuras normales.

Sonda pasiva añadida al bucle de entrada (`[halt]`): lee `savehaltflag` y
`MobileCheckIsEmptyHalt()` y registra cada cambio, sin sustituir ninguna
función del juego. Separa las dos ramas: si el flag cambia pero el buffer
sigue vacío, falla al preparar los datos (sospechoso `DateUtil_GetDateTime`
@0x1762b4, que `saveHalt` llama antes del checksum y depende de nuestros shims
de fecha, con un `cmp r1, #4` en 0x175bd8); si el flag ni se mueve, el menú no
llega a pedirlo.

**Corregido de camino, pero NO es la causa de esto**: `ReadSaveDataResultHook`
comprobaba solo `RockDat0.sav` y `RockDat1.sav`, y el motor escribe un fichero
por ranura (`RockDat<n>.sav`). Guardar en otra ranura hacía que el hook
reportase "no hay datos" en lugar del estado real, que es el valor del que
dependen los menús de guardar y cargar. Ahora ejecuta primero la función real
—que es una llamada de cola a `getResultReadFromFile(instance, -1)`— respeta su
respuesta, y solo interviene si iba a tomar el camino de datos dañados Y no
existe ningún fichero (16 ranuras más el `RockDat.sav` sin sufijo). Eso arregla
la *lectura*; la *escritura* sigue rota.

### Rendimiento en la ciudad: NO son los draws, son subidas de textura
Medido en Vita3K (el número de draws y de subidas es idéntico al hardware
aunque la velocidad no lo sea), con cada muestra etiquetada por número de mapa
—`ff6_current_map()` en la línea `[gl]`— porque midiendo sin etiquetar acabé
citando datos de un combate cuando la pregunta era sobre la ciudad:

| mapa        | draws/frame | binds/frame | subidas de textura/frame |
|-------------|-------------|-------------|--------------------------|
| 0 (combate) | 111         | 93          | 0,92                     |
| 18          | 28          | 20          | 0,29                     |
| **19**      | 31          | 23          | **9,22**                 |
| **20**      | 28          | 24          | **8,54**                 |
| 23          | 21          | 19          | 3,00                     |
| 244         | 23          | 20          | 2,16                     |

La ciudad dibuja **cuatro veces menos** primitivas que un combate, así que el
cuello no es el número de draws. Lo que dispara son las subidas parciales de
textura del compositor AGB al hacer scroll: 9 por frame de 1024x32 RGBA son
**1,12 MB por frame**.

Se probó `vglUseCachedMem(GL_TRUE)` antes de `vglInitExtended` — vitaGL reserva
sus pools como memoria NO cacheada por defecto (`has_cached_mem = GL_FALSE`,
`PHYCONT_NC_RW` en `utils/mem_utils.c:288`), así que cada uno de esos memcpy va
a RAM sin caché detrás. **NO arregla nada: probado en hardware y Narshe sigue
igual de lento.** No volver a intentarlo por ahí.

Así que el coste no está en el ancho de banda de esas subidas, o no solo ahí.
Lo que queda por descartar, en orden: qué hace vitaGL *por cada*
`glTexSubImage2D` además del memcpy (conversión de formato, invalidación de
caché de texturas, recreación del objeto); si el compositor AGB re-sube tiles
que no han cambiado; y medir de verdad dónde se va el tiempo con Razor
(`psp2/razor_hud.h`, `sceRazorGpuLiveStart` con `OVERVIEW_1/2/3`, que da
porcentajes de USSE de vértice y fragmento y de lecturas de textura), que es lo
único que separa CPU de GPU sin adivinar.

De paso: el motor rebindea una textura antes de casi cada draw (117 binds para
116 draws), y **19 de cada 114 nombran la que ya está atada**.
`glBindTextureHook` se los traga ahora; es una comparación y quita el 17% de
las llamadas de textura.

### Dibujar el mundo offscreen (`offscreen=1`) — NO se puede validar en Vita3K
La solución a las costuras es dibujar el mundo en un buffer a su propio tamaño
(480x320, muestreo 1:1, imposible que se forme costura) y estirar ESE buffer
una sola vez al pillarbox. Además debería ir **más rápido**, no más lento: se
rasterizan 480x320 en vez de 816x544, menos de la mitad de fragmentos.
**OJO: aquí decía que a 1x el hardware pasaba de 10 a 60 fps — ese dato era
FALSO** (desmentido por el usuario, sesión 4). Correr a 1x NO rescata el
framerate en hardware, así que el cuello no es (solo) el coste de fragmento
del escalado; la mejora de este camino queda en teoría hasta medirla.

Implementado en `bind_scene_fbo()` / `present_scene_fbo()` (main.cpp), opción
`offscreen=1`. Dos cosas que costaron entender:
1. El bind tiene que hacerse **entre frames**, justo después del swap. vitaGL
   solo adopta un cambio de render target al abrir la escena, así que atarlo a
   mitad de dibujado se ignora: el mundo seguía yendo a pantalla y el blit del
   buffer vacío lo tapaba (se veía un cuadrilátero negro encima del juego).
2. El quad del blit va en NDC con ambas matrices a identidad. Con una ortho
   propia salía deformado en trapecio, porque el estado de matrices del motor
   se colaba.

**En Vita3K sale negro y eso NO significa que esté mal.** Un bind/unbind de
framebuffer a mitad de frame es un scene split, y ya está documentado más
abajo que en Vita3K un split se lleva por delante el frame entero (es el mismo
bug del emulador por el que `glReadPixels` devuelve ceros). El trazado
demuestra que el camino se ejecuta: se crea el buffer 480x320, hay bind y blit
cada frame al rect correcto (72,0 816x544). Esta opción **solo se puede probar
en hardware**.

### Costuras finas entre tiles (abierto)
Se ven líneas claras de un píxel en los bordes de tile, sobre todo en la nieve.
Medidas sobre una captura de hardware: aparecen cada **13,6 px**, que es un
tile de 8 unidades lógicas a la escala 1,7x del pillarbox (8 x 816/480).

Descartado con evidencia: **no es el wrap**. El juego ya pide
`GL_CLAMP_TO_EDGE` en S y en T (registrado con `[wrap]` en las 12 primeras
texturas), así que no es el caso clásico de `GL_REPEAT` sobre un atlas.

Lo que queda es precisión, y encaja con los números. Las coordenadas de
textura llegan en `GL_FIXED` (16.16) y vitaGL las reconstruye en el vertex
shader generado:

    GLFixedToFloat(fx) = short2(fx).y + ushort2(fx).x * (1.0 / 65536.0)

Con UVs en 0..1 la parte entera es 0 y todo el valor vive en los 16 bits
fraccionarios. Si ese cálculo, o la varying `TEXCOORD` que lleva su resultado,
acaba en media precisión, la mantisa son 10 bits: 1/1024, que en un atlas de
1024 es **exactamente un téxel**. En el borde de un tile la muestra cae en la
celda vecina y se ve la costura.

Se probó a convertir las UV a float en el loader (elimina el desempaquetado del
shader). **Retirado**: corre sobre cada vértice de cada lote y el compositor
AGB envía más de mil de golpe, así que cuesta framerate — mala idea cuando el
framerate ya es el problema mayor. El sitio correcto es el shader de vitaGL,
donde no cuesta nada en tiempo de ejecución. La conversión de **color** sí se
queda, porque ahí no es opcional (vitaGL rechaza `GL_FIXED`) y solo corre en
draws de 4 vértices.

### Leer un crash dump del hardware
`tools/parse_core.py volcado.psp2dmp build_rel/FF6.elf` dice qué hilo murió,
por qué, en qué símbolo, con qué registros, y saca los puntos de llamada que
quedan en las pilas. Los `.psp2dmp` de `ux0:data` son ELF core comprimidos con
gzip; **no se borran solos**, así que hay que mirar la fecha para saber cuál
corresponde a la ejecución que interesa, y el TTY que incluyen arrastra
mensajes de sesiones anteriores (me llevó a una conclusión falsa una vez).
Dos detalles que hacen falta para interpretarlos y que no son evidentes:
el módulo NO se carga en su dirección de enlace (aquí +0x25000, hay que
restarlo antes de pasar nada a `addr2line`), y las tablas de los notes no son
de paso fijo — cada entrada lleva su tamaño delante y el contador está en la
palabra 1; asumir un paso constante funciona para las primeras entradas y
descarta justo el hilo que ha petado.
`vita-parse-core` hace más cosas (desensamblado alrededor del PC, semáforos,
bloques de memoria) pero es de Python 2: necesita un `elftools.common.py3compat`
que ya no existe y un `c_str` que itera bytes como si fueran caracteres.

### Lo que se midió antes de encontrarlo (sigue siendo válido)
**Resumen: los sprites se dibujan perfectamente; lo que está vacío es su
contenido.** Llegué a un combate real (con el autoplay de abajo) y volqué el
frame. Los draws del combate están todos ahí y son correctos:

```
#04 tex=387 rgba=1000,1000,1000,1000 quad=(348,114)-(412,50) z=250 n=4 blend=1 tex2d=2
#05 tex=387  (misma geometría: la segunda pasada aditiva de drawBtlCharacter)
#08 tex=385 quad=(364,150)-(428,86)      #12 tex=386 quad=(380,186)-(444,122)
#02 tex=392 quad=(48,194)-(112,98)       (enemigos, lado izquierdo)
```
Geometría en su sitio (x 348..444 = grupo, x 48..112 = enemigos), color blanco
opaco, textura activa, blend on, dos pasadas por personaje tal y como describe
el `.so`. No falta ningún draw y no hay modulación a negro.

Lo que sí está mal es la textura. Muestreando cada byte de las subidas:
```
[tex] img tex=385 512x128 nonzero=0/4033 max=0     <- hoja de sprites VACIA
[tex] img tex=386 512x256 nonzero=0/4065 max=0
[tex] img tex=387 512x256 nonzero=0/4065 max=0
[tex] img tex=388 512x256 nonzero=0/4065 max=0
[tex] img tex=389 1024x1024 nonzero=3070/4093 max=255   <- este sí tiene arte
```
y de los refrescos por frame a texturas de combate, 37 llegan a cero y 21 con
contenido. Es decir: el juego compone los píxeles del personaje en su buffer de
trabajo y ese buffer sale en blanco.

Ese contenido vacío resultó ser una pista falsa para este bug concreto: parte
de las hojas se rellenan por sub-upload más tarde y la causa real de la
invisibilidad era el array de color (arriba). Queda anotado por si reaparece.

Descartado con evidencia, no repetir: no falta ningún símbolo (el `.so` importa
219 y `default_dynlib[]` los cubre todos), no es el blend, no es el color, no
es la geometría, no es que no se dibuje, y toda la familia `cTs*`
(`cTsModel::drawModel`) es del mapamundi y no interviene en combate.

### Texturas paletizadas — bug real arreglado de camino (no era la causa)
Los sprites del grupo se crean como `GL_PALETTE8_RGBA8_OES` con
`glCompressedTexImage2D` (`drawDataCellBlend` @0x130cf8) y luego se refrescan
cada frame con `glTexSubImage2D(GL_RGBA, GL_UNSIGNED_BYTE)`
(`updateTextureDataCellWithTexID` @0x12ed84). En Android el driver expande las
texturas paletizadas a RGBA8 al subirlas, así que ese sub-upload RGBA cae sobre
una superficie RGBA8 y funciona. vitaGL en cambio mantiene una superficie real
de 1 byte por texel con paleta aparte, y su `glTexSubImage2D` guarda **solo el
byte rojo** de cada píxel como índice de paleta. El índice 0 es el color
transparente en estas paletas → el personaje entero sale transparente.
Los enemigos se ven porque `drawDataCellMonster` @0x131200 tiene una rama
distinta que crea la textura con `glTexImage2D(GL_RGBA)` de verdad; el fondo de
combate igual (`makeCellTexture` @0x1ef588). `drawDataCellBlend` no tiene esa
rama — de ahí la asimetría exacta del síntoma.
Arreglado en el loader: `glCompressedTexImage2DHook` expande las diez variantes
`GL_PALETTE{4,8}_*_OES` a RGBA8 en CPU y llama a `glTexImage2D` normal, que es
lo que hacía Android. El juego no llama nunca a `glGetError` ni consulta
`GL_EXTENSIONS`, así que no puede notar la diferencia. Confirmado que se
dispara en combate (`[pal] expanded 0x8b96 256x256 to RGBA8`, dos veces).
Es un arreglo de corrección real — vitaGL estaba corrompiendo esas texturas —
pero **NO hace visibles a los personajes**, porque el problema está antes, en
el contenido que el juego compone (ver arriba). Se queda porque es correcto.

Sospechosos secundarios que quedan anotados por si sirven al tirar del hilo:
1. `updateTextureDataCell` @0x12ef74 no valida `cell[0x1C]`, que vale −1 hasta
   que `drawDataCellBlend` crea la textura → un `glBindTexture` con id inválido
   deja atado el atlas anterior y el sub-upload escribe donde no debe.
2. `drawImagePriBlend` @0x130488 deja `GL_COLOR_ARRAY` habilitado apuntando a
   su propio marco de pila, al revés que el resto de helpers de dibujo.

### Autoplay para llegar a un combate sin manos
Con un build `FF6_DEBUG`, `touch ux0:data/ff6/autoplay.trigger`: el loader
pulsa confirmar y camina al norte solo, y desde el arranque llega al primer
combate guionizado de Narshe en unos 6-7 minutos. Sirve para reproducir todo
esto sin jugar a mano. Quitar el fichero lo desactiva.

---

# Caza del área de juego negra (resuelta)

## SESIÓN 3 — capas validadas como CORRECTAS (no volver a mirar)
Instrumentación nueva: `[pipelog]` en Vita3K (vulkan/pipeline_cache.cpp,
compile_pipeline) vuelca por pipeline: fragprog guest addr, blend horneado,
hashes sha256 de vert/frag shader (= nombres de los .spv en
`$B/cache/shaders/FF6000000/eboot.bin/vk13-<hash>.spv`). Y `[drawlog3]`
(SceGxm.cpp) vuelca por draw el layout de atributos + bytes crudos de los 2
primeros vértices de cada stream.

1. **Blend Vulkan** — ELIMINADO. Escena 0x85518ad8 y panel 0x85e38388 hornean
   EL MISMO blend: enable, SrcAlpha/OneMinusSrcAlpha, Add, writeMask=0xf.
2. **Alfa que llega al blender** — ELIMINADO. Kill-switch `VITA3K_NO_BLEND=1`
   (creation.cpp, fuerza blendEnable=false en todos los pipelines): el área
   sigue negro puro (0,0,0) ⇒ el RGB que sale del fragment shader YA es negro
   (o el fragmento nunca llega al color buffer).
3. **Shaders traducidos** — CORRECTOS a ojo (decompilados con spirv-cross):
   - frag escena e0bdaeeb…: MODULATE textbook `v_Color0 × tex`, pack/unpack
     f16, sin discard. El fma de UV con viewport_ratio/offset es identidad
     (TextureViewport default ratio=1, offset=0; solo cambia con redirect
     surface-as-texture, que dio 0 hits).
   - vert escena ed812c10…: WVP por dot-products correcto, v_Color0 = atributo
     color passthrough, UV por matriz de textura, z dentro de rango.
   - frag panel 50572d7b…: REPLACE + discard (alpha test). vert panel a24dfccd….
4. **Vertex fetch a nivel GXM** — CORRECTO. [drawlog3]: draws de escena
   (85518ad8) tienen 3 streams: pos float3 stride12, uv float2 stride8, color
   U8N×4 stride4 con bytes `ffffffff` (blanco OPACO) en los draws del logo
   (`ffffff00` en los fades, legítimo). El color array llega bien al emulador.
5. Mapeo fragprog→shader completado (paso pendiente de sesión 2):
   - escena 0x85518ad8 → frag e0bdaeeb… vert ed812c10…
   - panel  0x85e38388 → frag 50572d7b… vert a24dfccd…
   - gris #01 0x85e80850 → frag 89831886… ; #26 fade 0x855186a8 → 0be676f8…
   - internos vitaGL: 0x84148560 (clear, a veces fragdis=1), 0x84148618
     (MASKUPDATE, maskupd=1 blendEn=0 writeMask=0xf, full-screen u0=(-1,1,1,-1))

## SESIÓN 3 — dato clave del usuario
En el commit 4373caa la sonda `draw_viewport_probe()` (replay del PRIMER draw
de escena, verbatim, justo antes del swap) SÍ se veía en pantalla. OJO: el
draw #01 es SIN textura (tex2d=0, quad gris 125,125,94) — la sonda no probó el
camino texturado. Pero #01 in-frame NO se ve (área negra 0,0,0 exacto, no gris
32,32,24) ⇒ **el mismo draw es invisible a mitad de frame y visible al final**.
El estado y los datos son correctos; algo POSICIONAL dentro del frame suprime
o borra los draws de escena.

## ✅ RESUELTO (sesión 3, 2026-07-28): CAUSA RAÍZ ENCONTRADA Y ARREGLADA
**El juego se ve.** Splash con logo y copyright, intro completa de Narshe con
texto, sprites y scroll. Capturas:
`Final Fantasy VI_2026-07-28-145858.jpg` y `…-145958.jpg`.

### Causa raíz
Los draws MASKUPDATE de vitaGL (`scissor_test_fragment_program`, creado con
`sceGxmShaderPatcherCreateMaskUpdateFragmentProgram`; en este juego fragprog
guest 0x84148618) que `update_scissor_test()` (vitaGL tests.c) emite a mitad
de frame. En GXM real solo escriben el mask bit del tile (inofensivo). El
backend **Vulkan de Vita3K no tiene emulación del mask bit** (use_mask_bit
solo existe en GL) pero **NO saltaba esos draws**: los ejecutaba como draws
normales cuyo shader (generate_update_mask_body, spirv_recompiler.cpp:1819)
escribe `vec4(writing_mask)` en el color attachment ⇒ quad a pantalla completa
con writing_mask=0.0 ⇒ **wipe NEGRO OPACO del color buffer después de los
draws de escena y antes de los paneles**, cada frame. Por eso: paneles
visibles (post-wipe), escena invisible (pre-wipe), sonda pre-swap visible,
idéntico con blend on/off, y todos los datos GXM perfectos.

### Fix (fork Vita3K, rama ff6-debug)
`renderer/src/vulkan/scene.cpp draw()`: si `record.is_maskupdate` → return
(igual que hace GL con `!use_mask_bit`), + log `[maskdraw]` del writing_mask.
Env `VITA3K_KEEP_MASKUPDATE=1` restaura el comportamiento antiguo. Cambios
sin commitear en ~/Fun/vita3k: scene.cpp (fix), pipeline_cache.cpp ([pipelog]),
creation.cpp (VITA3K_NO_BLEND), SceGxm.cpp ([drawlog3]).
Candidato a PR upstream a Vita3K (bug genérico: afecta a todo lo que use
maskupdate en Vulkan, i.e. cualquier app vitaGL con scissor).

### Preguntas abiertas (no bloquean)
1. ¿Por qué writing_mask=0.0 en TODOS los maskdraws? update_scissor_test pone
   func ALWAYS (→1.0) antes de su draw. Sospecha: sceGxmSetFragmentProgram
   diferido en Vita3K ⇒ los sceGxmSetFrontStencilFunc se procesan con
   `record.is_maskupdate` del programa ANTERIOR y writing_mask se queda en su
   0.0 inicial. Verificar si el backend GL de Vita3K (use_mask_bit=true, pinta
   el mask FBO a 0 ⇒ los frag shaders GL descartan) se arregla igual — si es
   así, es la MISMA causa en ambos backends y explica el "idéntico en GL".
2. El bug original del .so (`TextGlyphTexture::drawGlyph` nunca subía glifos)
   sigue sin explicación de fondo; el drawGlyphHook lo tapa y funciona.
3. En Vita REAL nada de esto aplica (mask bit nativo): el port podría haber
   estado dibujando bien en hardware todo este tiempo.

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
