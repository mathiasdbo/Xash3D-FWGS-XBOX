# Plan del port a Xbox Original

Fases sobre el fork de Xash3D-FWGS. Cada una es una rama, con micro-commits,
revisión de especialistas y PR mergeado antes de avanzar (ver `/AGENTS.md`).

Toda divergencia que introduzca una fase se anota en `divergences.md` el
mismo día.

## Supuestos

- **PC sigue siendo producto de primera clase**, no solo entorno de
  desarrollo. `ref/gl` se mantiene vivo y el `.mpak` sirve a dos consumidores
  con necesidades distintas — cosa que el formato ya anticipa, porque
  `MpakHeader6::flags` distingue `1 = Xbox, 0 = PC`.
- **Ironwail queda fuera de alcance** hasta que el motor arranque en hardware.
  Sus optimizaciones estrella son GPU-driven (culling por compute shader,
  `glMultiDrawElementsIndirect`, OpenGL 4.3+) y la NV2A no tiene ni compute ni
  draw indirecto: es función fija más VP20 y register combiners. Sirven al
  build de PC, no al objetivo de consola. Mezclarlas ahora agranda el riesgo
  sin acercar la meta.

---

## F0 — Toolchain y build estático

Que el árbol compile con nxdk antes de cambiar una línea de lógica.

- `engine/platform/xbox/` a partir de `platform/psvita` como plantilla
  (`sys_*.c`, `in_*.c`, `vid_*.c`).
- Integrar nxdk en el build de waf: `wscript` de nivel raíz, toolchain
  `clang -target i386-pc-win32 -march=pentium3 -fuse-ld=nxdk-link`.
- Activar `XASH_STATIC_LIBS` + `--single-binary`. **No hay trabajo de
  abstracción acá**: el modo ya existe y ya está contemplado en
  `ref_common.c`, `filesystem_engine.c`, `cl_gameui.c`, `host.c` y
  `lib_common.c`.
- Enlazar el HL SDK estáticamente (el proyecto anterior ya resolvió esto con
  el toolchain de nxdk y una ruta sin export table).
- `vgui_support` a stub nulo. `engine/client/vgui/vgui.c:345` ya reporta el
  fallo de carga y continúa, así que no es fatal. HL1 en un jugador
  prácticamente no lo usa; solo vuelve a ser trabajo real si se quiere
  multijugador.

**Salida:** un XBE que arranca en xemu con `ref/null` y llega a la consola del
motor.

---

## F1 — `.mpak` como searchpath

- `filesystem/mpak.c` implementando `searchpath_t` (7 punteros:
  `pfnOpenFile`, `pfnLoadFile`, `pfnFindFile`, `pfnFileTime`, `pfnSearch`,
  `pfnPrintInfo`, `pfnClose`), calcado de `pak.c`.
- `pfnLoadFile` acepta `pfnAlloc`/`pfnFree` propios: usarlos para que cada
  recurso caiga en el pool correcto de `zone.c`.
- **Carga por recurso, no por archivo.** Esto resuelve de raíz el problema
  del `global.mpak` de 62,9 MB del proyecto anterior, que se cargaba entero
  (99,1 % de ese archivo es `lenAssetData`).

**Salida:** el motor lee un mapa desde un `.mpak` con `ref/null`, sin tocar
un WAD.

---

## F2 — Techo de memoria de 64 MB

Tres capas, una responsabilidad cada una:

- **`tools/asset-studio` decide y rechaza.** Emite un manifiesto de memoria
  por archivo (bytes residentes por categoría tras la carga) y **falla el
  build** si `global + max(mapa)` supera el presupuesto declarado. En hardware
  fijo el presupuesto se hace cumplir al hornear, no en runtime: un mapa que
  no entra rompe el build, no la consola. El baker ya escribe todas las
  longitudes de lump en la cabecera; hoy no las lee nadie.
- **`zone.c` mide.** Añadir marca de agua y límite por pool. La contabilidad y
  la atribución `file:line` ya existen.
- **Un solo libro contable, dos allocators.** En Xbox los 64 MB son
  compartidos con la GPU, y las texturas de la NV2A necesitan memoria
  físicamente contigua (`MmAllocateContiguousMemoryEx`), fuera del heap
  normal. Un presupuesto que solo cuente el heap es ciego a la mitad del
  consumo. Diseñarlo así desde el principio o los dos números nunca cuadran.
- Lightmaps a RGB565: 8 MB → 4 MB en el peor mapa. `MpakHeader.h:94` ya
  reserva el campo. Es la mayor ganancia por línea escrita de todo el plan.

**Salida:** presupuesto declarado, verificado al hornear y medido en runtime.

---

## F3 — Recortes para consola

Xash3D-FWGS asume un host moderno. Preferir flags de build existentes antes
que `#ifdef` nuevos.

- Fuera: descargas HTTP, grabación/reproducción de demos, voz, servidor
  dedicado, masterlist.
- Revisar red: un port de un jugador no la necesita, pero cortarla toca
  mucho código. Evaluar coste real antes de decidir.
- Revisar `MAX_MODELS` / `MAX_SOUNDS` (`protocol.h:193-194, 211`) contra el
  presupuesto de F2.

**Salida:** huella de código y datos residentes medida, con el recorte
justificado en `divergences.md`.

---

## F4 — Carga de brush models desde `.mpak`

La divergencia grande, y la más delicada de mantener. El `.mpak` guarda los
lumps estructurales **crudos** (`BspParser.cpp:681-686`,
`getLumpData(LUMP_NODES)` y compañía) y pre-digiere solo la mitad de render.
`mod_bmodel.c` se parte exactamente por esa línea:

| Intactas (lumps crudos) | A sustituir (mitad de render) |
|---|---|
| `Mod_LoadPlanes:2424`, `Mod_LoadNodes:3438`, `Mod_LoadLeafs:3546`, `Mod_LoadClipnodes:3785`, `Mod_LoadMarkSurfaces:2537`, `Mod_LoadVisibility:3830`, `Mod_LoadSubmodels:2214`, `Mod_LoadEntities:2298`, `Mod_LoadTexInfo:3247` | `Mod_LoadVertexes:2456`, `Mod_LoadEdges:2490`, `Mod_LoadSurfEdges:2525`, `Mod_LoadSurfaces:3291`, `Mod_LoadTextures:3104`, `Mod_LoadLighting:3883` |

Nueve funciones siguen funcionando tal cual, incluidas **todas** las de
colisión y PVS. Seis se reemplazan por una ruta que lee lo pre-horneado.

Mirar antes `Mod_LoadLump:756`: ya recibe un parámetro `loadlump_source_t
source`, así que puede que el punto de enganche esté hecho y la divergencia
sea mucho menor.

**Salida:** un mapa carga desde `.mpak` y se dibuja con `ref/gl` en PC, sin
tocar un WAD ni un BSP original.

---

## F5 — `ref/nv2a`

El trabajo genuinamente específico de la consola. Encaja donde encaja
`ref/soft`; `ref_interface_s` no se toca.

- Envío al pushbuffer vía `pb_begin`/`pb_end` y fence manual contra el DMA en
  memoria unificada.
- Atlas único por mapa en vez de texture arrays (la NV2A no las tiene). **El
  baker ya lo produce**: `PakManager.cpp:22-60`, borde de 1 px y UVs
  normalizados en `Vertex::rect`.
- Formato de vértice sin atributos enteros: la NV2A solo admite FLOAT1-4,
  D3DCOLOR y SHORT normalizado. Toca también el baker.
- Register combiners para `diffuse × lightmap × 2` (bit de escala nativo) y
  alpha test nativo.
- Skinning por paleta de matrices en VP20.
- **Rediseño de iluminación dinámica**: de N luces por píxel a 8 por vértice
  en T&L fija. No es un port, es una decisión de aspecto visual que hay que
  tomar mirando la pantalla. Es la fase con la varianza real de todo el plan.
- Corregir `swizzleNV2A` sobre bloques DXT (`TextureOpt.cpp:110-132`): en
  Xbox `D3DFMT_DXT1` es lineal por filas de bloques; el swizzle Morton aplica
  a superficies sin comprimir. Bug latente, solo alcanzable por la ruta Xbox.

**Salida:** un mapa se dibuja en xemu con lightmaps, entidades animadas y
sprites, comparado cuadro a cuadro contra la captura de PC.

---

## F6 — Audio

La única de las piezas grandes que upstream no abstrajo: 6.284 líneas en
`engine/client/sound/` acopladas al mixer de PC.

- Extraer una interfaz de backend de audio, al estilo de `ref_interface_s`.
- Backend NVAPU con ADPCM.
- **Candidato a mandar upstream**: la abstracción es útil para todos los ports
  de consola, no solo para este. Si upstream la acepta, la divergencia
  desaparece.

**Salida:** sonido posicional y sentencias en xemu.

---

## F7 — Empaquetado, medición y ajuste

- Empaquetado XBE con `cxbe` e imagen ISO con `extract-xiso`.
- Contadores por subsistema con `rdtsc` en el backend de plataforma.
- Medir en `c1a3` y `crossfire`, no en `c0a0` — es de los mapas más vacíos.
- Auditoría de memoria contra los 64 MB: pico por mapa, fragmentación tras
  varios `changelevel`.
- **Validar en hardware real**, no solo en xemu: el timing del DMA y el canal
  serial no se emulan fielmente.

**Salida:** un mapa jugable de principio a fin en una Xbox real, con
presupuesto de frame y pico de memoria documentados.

---

## Fuera de alcance por ahora

- **Ironwail** (ver Supuestos). Fase propia después de F7, si PC sigue siendo
  producto.
- **VGUI real.** Stub nulo en F0. Solo vuelve a ser trabajo si se quiere
  multijugador.
- **Multijugador en consola.**
