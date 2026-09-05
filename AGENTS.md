# Reglas del proyecto — Resonance3D (fork de Xash3D-FWGS)

Este repositorio **es** Xash3D-FWGS. Ver `docs/r3d/README.md` para el
contexto y `docs/r3d/fork-plan.md` para el plan.

## La regla central: minimizar la divergencia

Esto invierte la regla que gobernaba el proyecto Rust anterior. Allá había que
**replicar** la referencia y citarla en cada función. Acá **somos** la
referencia, y el trabajo es no separarnos de ella más de lo estrictamente
necesario.

- **Toda desviación de upstream se anota en `docs/r3d/divergences.md` el
  mismo día que se escribe.** No al cerrar la fase, no en el PR: el mismo día.
  Un fork sin registro de divergencias se vuelve inmanejable en el tercer
  merge y se abandona.
- **Preferir siempre la forma más barata de divergir**, en este orden:
  archivo nuevo en directorio nuestro → archivo nuevo en directorio de
  upstream → `#ifdef` → reescritura de función → cambio de comportamiento sin
  guardas (prohibido salvo bug real de upstream, y en ese caso el parche va
  upstream primero).
- **No mover ni renombrar nada del árbol de upstream.** Lo nuestro vive en
  `tools/` y `docs/r3d/`. `scripts/` y `Documentation/` ya existen en
  upstream: no poner nada nuestro ahí.
- **Antes de escribir una abstracción, buscar si upstream ya la tiene.** Ya
  están hechos: el renderer polimórfico (`ref_api.h`), el enlazado estático
  (`XASH_STATIC_LIBS`), la capa de plataforma (16 backends), los pools de
  memoria (`zone.c`) y la vtable de searchpaths (`searchpath_t`). Ver la
  sección final de `divergences.md`.
- **Mandar upstream lo que sea genéricamente útil.** La abstracción de audio
  es el ejemplo claro: si upstream la acepta, la divergencia desaparece y el
  coste de mantenimiento con ella.

## Xbox Original: lo que condiciona todo

- **64 MB compartidos con la GPU.** Las texturas de la NV2A necesitan memoria
  físicamente contigua, fuera del heap normal: un presupuesto que solo cuente
  el heap es ciego a la mitad del consumo. Un solo libro contable, dos
  allocators.
- **El presupuesto se hace cumplir al hornear, no en runtime.** Un mapa que no
  entra tiene que romper el build, no la consola.
- **Nunca filtrar memoria por mapa.** Todo lo que carga un nivel se libera al
  salir, y eso se mide, no se supone.
- **Sin carga dinámica de librerías.** Build estático de una sola imagen.
- **La NV2A es función fija más VP20 y register combiners.** Sin compute, sin
  draw indirecto. Todo lo que asuma shaders programables modernos no aplica.

## Código

- C99 y el estilo de upstream. `uncrustify.cfg` de la raíz es el árbitro.
- Nada de reformatear código de upstream: ensucia el diff del merge y esconde
  las divergencias reales.
- Comentar el *por qué*, no el *qué*. En una divergencia, el comentario cita
  la entrada de `divergences.md`.
- Constantes y flags: buscarlos en `common/const.h` o en la fuente. Nunca
  adivinarlos.

## Leer los ports hermanos antes de inventar

Ningún problema de este port es nuevo. Antes de derivar una solución, leer la
que ya funciona:

- **`engine/platform/psvita/`, `nswitch/`, `dos/`** — los backends de consola
  que ya están en este árbol. Son la plantilla de `engine/platform/xbox/`.
- **`ref/soft/`** — la prueba de que `ref_interface_s` aguanta un backend que
  no es GL. Cuando algo de la interfaz parezca imposible fuera de OpenGL,
  mirar cómo lo resolvió el rasterizador por software.
- **[xashPS3](https://github.com/Mayo1970/xash3d-fwgs)** — port hermano del
  mismo motor a PS3, con un registro de portado exhaustivo. Ya se comió los
  problemas de enlazado estático (colisiones de símbolos, el global `FI`,
  deduplicación de vtables), los supuestos del toolchain y las trampas de
  endianness. Consultarlo antes de depurar cualquiera de esas familias.

Citar en el commit qué se leyó y por qué la solución de acá difiere, si
difiere.

## No adivinar

- **Confirmar cada supuesto** leyendo el código o observando el hardware.
  Nunca por memoria y nunca por analogía.
- **Ante un fallo, aislar, no reescribir.** Producir el caso de prueba mínimo
  que lo reproduce. Reescribir una implementación entera cuando algo falla
  destruye la información de qué estaba mal.
- **Tras dos intentos fallidos, parar y escalar con datos** — salida de `nm`,
  `objdump`, un volcado, una traza. No con una hipótesis nueva.
- Cuando una hipótesis se demuestra falsa, dejarlo escrito. Saber qué NO era
  vale tanto como saber qué era.

## Verificación

- Compilar y arrancar antes de dar nada por terminado. No avanzar con el árbol
  roto.
- **xemu no es validación.** Sirve para iterar rápido, pero el timing del DMA,
  el canal serial y el comportamiento real de la memoria unificada no se
  emulan fielmente. Una fase no se cierra hasta que corre en una Xbox real.
- El build de PC es la red de seguridad: **toda fase tiene que dejar PC
  funcionando**. Si PC se rompe, se perdió el único punto de comparación
  contra el comportamiento correcto.
- Añadir el build de Xbox como paso de verificación obligatorio en cuanto F0
  cierre, aunque el binario todavía no haga nada útil. El target del proyecto
  anterior acumuló seis campos obsoletos en siete meses porque nada lo
  ejercitaba.
- Smoke test tras desplegar, no solo copiar.

## Flujo de trabajo

- Rama por fase o feature, desde `main`.
- Micro-commits: cada paso pequeño y validado se commitea y se empuja.
- Revisión de sub-agentes especialistas antes de cerrar una fase, eligiendo
  los que el cambio toca. Nunca pedir permiso primero.
- Actualizar `CHANGELOG.md` y `docs/r3d/divergences.md`, y abrir PR. No
  avanzar a la fase siguiente hasta que esté mergeado.
- Tras mergear, `git fetch upstream` y evaluar si toca sincronizar.

## Sincronización con upstream

- `git fetch upstream && git merge upstream/master`.
- Resolver conflictos **contra `divergences.md`**: si un conflicto toca una
  divergencia registrada, la entrada dice por qué está ahí. Si toca algo que
  no está registrado, es un bug de proceso — anotarlo antes de resolver.
- Empujar a `upstream` está deshabilitado a propósito
  (`git remote set-url --push upstream no-push`).

## Atribución en git

Los mensajes de commit, títulos y descripciones de PR describen el cambio y
por qué, y nada más. Sin trailers de herramientas ni pies de "generado con".
Lo mismo aplica a `CHANGELOG.md` y a los comentarios del código.
