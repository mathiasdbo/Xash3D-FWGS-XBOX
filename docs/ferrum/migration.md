# Migración desde Ferrum56 (Rust)

## Qué se archivó y por qué

El proyecto anterior reimplementaba Xash3D-FWGS en Rust desde cero. Llegó a
~35.000 líneas repartidas en siete crates, con 631 tests unitarios y unas
8.800 líneas de comentarios citando `file:line` de la referencia. Estaba en el
Track C/D de su plan de fidelidad, lejos todavía de la paridad de features.

La decisión de bifurcar no fue por el lenguaje. La disyuntiva real era
**reimplementar** un motor contra **adaptar** uno. Partiendo del fork se llega
a paridad el día uno, y los cuatro pedidos que motivaban la reescritura ya
estaban resueltos en upstream:

| Se necesitaba | Ya existía en upstream |
|---|---|
| Renderer polimórfico | `ref_api.h` v17, 3 backends en producción |
| Gestor de recursos en RAM | `zone.c`, pools con atribución `file:line` |
| VFS extensible para `.mpak` | `searchpath_t`, vtable de 7 punteros |
| Enlazado estático para consola | `XASH_STATIC_LIBS` + `--single-binary` |
| Menú con gamepad | `3rdparty/mainui`, con `menus/Gamepad.cpp` |
| Audio polimórfico | **No.** Es la única pieza que sigue siendo trabajo real |

El argumento más fuerte para no reescribir eran esos 631 tests y las
citas a la referencia. Se cae solo: existen para probar que el Rust coincide
con Xash3D. Si *sos* Xash3D, es cierto por construcción. Ese aparato era el
coste de no ser la referencia, y bifurcarla lo lleva a cero.

## Qué se conservó

`tools/asset-studio/` — 8.544 líneas de C++/CMake. Es la contribución
genuinamente original del proyecto anterior: el baker offline que produce
`.mpak` con atlas por mapa, compresión DXT, empaquetado de lightmaps y
alineación a sector de 2048 bytes.

Queda mejor servido acá que allá: `searchpath_t::pfnLoadFile` es por recurso,
no por archivo, lo que resuelve de raíz el problema del `global.mpak` de
62,9 MB que se cargaba entero (99,1 % de ese archivo es `lenAssetData`).

Se copiaron también los manifiestos (`manifests/`) y
`build-test-package.ps1`. **No** se copiaron los `.wad` ni el `.bsp` de
prueba que había sueltos en el directorio del baker: son assets retail, no
estaban trackeados en el repo anterior, y no son nuestros para redistribuir.

## Qué NO se trajo

Los siete crates de Rust (`ferrum-core`, `ferrum-math`, `ferrum-vfs`,
`ferrum-render`, `ferrum-pal`, `ferrum-engine`, `vgui-rs`). Todo eso es una
reimplementación parcial de código que ya existe y funciona en este árbol.

Vale la pena conservar el repo anterior en modo lectura por dos cosas
concretas que ya resolvió y que van a hacer falta:

- **Enlazado estático del HL SDK con el toolchain de nxdk**, incluida una ruta
  de spawn de entidades sin export table.
- **El análisis de la NV2A**: encoding de pushbuffer, register combiners y
  VP20, con tests.

Son referencias, no código a portar.

## Pasos pendientes (requieren tus credenciales)

### 1. Crear el repo privado y empujar

Un fork de un repo público en GitHub **siempre es público** y no se puede
pasar a privado. La forma correcta es un mirror desacoplado.

Este árbol local ya está configurado: `upstream` apunta a FWGS con push
deshabilitado, y la rama `main` sale de `c7768c48`. Falta solo el `origin`:

```bash
cd G:/Proyectos/Ferrum56/ferrum56-engine
git remote add origin git@github.com:<usuario>/<repo-privado>.git
git push -u origin main
git push origin upstream-master        # la base intacta, para diffear contra ella
```

`upstream-master` se conserva a propósito: es la única forma barata de
responder "¿qué cambiamos nosotros?" con un solo `git diff`.

### 2. Submódulos

No se inicializaron todavía (son cientos de MB). Antes de compilar:

```bash
git submodule update --init --recursive
```

`3rdparty/mainui` es el que importa para el menú.

### 3. Archivar el repo anterior

En GitHub: Settings → Archive this repository. Deja el repo en solo lectura
sin borrar nada, y es reversible.

Antes de archivar conviene dejar un puntero en su `README.md` hacia este
repo, porque un repo archivado no se puede editar después.

### 4. Licencia

Xash3D-FWGS es **GPL-3.0-or-later** (confirmado en las cabeceras de
`engine/common/host.c`). Desarrollar en privado no genera ninguna obligación:
la GPL se dispara al **distribuir binarios**, y solo hacia quien los recibe.
El momento a tener en cuenta es al repartir un XBE o un ISO, no antes.
