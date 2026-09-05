# Registro de divergencias con upstream

Toda desviación deliberada del árbol de Xash3D-FWGS se anota acá **el mismo
día que se escribe**. Sin esto, el tercer merge con upstream es una
arqueología de por qué tal línea está distinta, y el fork se abandona.

Base del fork: `c7768c48` (2026-08-08).

## Formato

Una entrada por divergencia, con:

- **Qué**: archivo(s) y función(es) tocadas.
- **Por qué**: la razón concreta, en una frase.
- **Forma**: `#ifdef` / archivo nuevo / reemplazo / recorte.
- **Riesgo de merge**: alto si toca código que upstream modifica seguido.
- **Salida**: qué haría falta para eliminar la divergencia (a veces "ninguna,
  es permanente").

Preferencia de forma, de menos a más costoso de mantener:

1. **Archivo nuevo en un directorio nuestro** — coste de merge cero.
2. **Archivo nuevo dentro de un directorio de upstream** (p. ej. un
   `filesystem/mpak.c` junto a `pak.c`) — coste casi cero, solo el `wscript`.
3. **`#ifdef XASH_XBOX` alrededor de código de upstream** — coste bajo, pero
   ensucia diffs.
4. **Reescritura de una función de upstream** — coste alto. Requiere
   justificación explícita acá.
5. **Cambio de comportamiento sin `#ifdef`** — prohibido salvo que sea un bug
   real de upstream, en cuyo caso se manda el parche upstream primero.

## Divergencias activas

| # | Qué | Por qué | Forma | Riesgo de merge | Salida |
|---|-----|---------|-------|-----------------|--------|
| 1 | `.gitignore` — bloque añadido al final | El `.gitignore` de upstream ignora globalmente `AGENTS.md` (:367), `CMakeLists.txt` (:68) y `build-*` (:312), lo que dejaba fuera del control de versiones, en silencio, tres archivos nuestros | Añadido al final del archivo, con negaciones explícitas | Bajo — upstream edita este archivo de vez en cuando, pero el conflicto sería trivial y siempre al final | Ninguna, es permanente |

## Divergencias previstas

Lo que el plan ya sabe que va a divergir, para que no sorprenda. Se mueven a
la tabla de arriba cuando se escriben.

| Área | Forma prevista | Riesgo | Nota |
|---|---|---|---|
| `filesystem/mpak.c` | Archivo nuevo + entrada en `filesystem/wscript` | Bajo | `searchpath_t` ya es una vtable de 7 punteros; se implementa igual que `pak.c` |
| `mod_bmodel.c`: mitad de render | Reescritura de 6 funciones bajo `#ifdef` | **Alto** | `Mod_LoadVertexes`, `Mod_LoadEdges`, `Mod_LoadSurfEdges`, `Mod_LoadSurfaces`, `Mod_LoadTextures`, `Mod_LoadLighting`. Las 9 funciones estructurales (`Mod_LoadPlanes/Nodes/Leafs/Clipnodes/MarkSurfaces/Visibility/Submodels/Entities/TexInfo`) quedan intactas porque el `.mpak` guarda esos lumps crudos |
| `ref/nv2a/` | Backend nuevo | Ninguno | Encaja donde encaja `ref/soft`. `ref_interface_s` no se toca |
| Techo de memoria sobre `zone.c` | `#ifdef` + funciones nuevas | Medio | `zone.c` ya tiene pools nombrados con atribución `file:line`; falta el límite |
| Audio polimórfico | Refactor de `engine/client/sound/` | **Alto** | Es la única de las piezas grandes que upstream no abstrajo. Candidato a mandar upstream |
| Recortes para Xbox (demos, HTTP, voz, red) | `#ifdef` / flags de build | Bajo | Preferir flags de `wscript` existentes antes que `#ifdef` nuevos |

## Cosas que NO son divergencias (y no hay que tocar)

Verificado en el árbol. Registrado acá para que nadie "arregle" lo que ya
funciona:

- **El renderer ya es polimórfico.** `engine/ref_api.h`, `REF_API_VERSION 17`,
  `ref_interface_s` con ~59 punteros, y tres backends en producción:
  `ref/gl`, `ref/soft` (17.422 líneas — la prueba de que la interfaz aguanta
  un backend radicalmente distinto), `ref/null`.
- **El enlazado estático ya es un modo de build de primera clase.**
  `XASH_STATIC_LIBS` en `common/defaults.h:102-105` activa `XASH_LIB
  LIB_STATIC` y `XASH_INTERNAL_GAMELIBS`, que ya está contemplado en
  `ref_common.c:620`, `filesystem_engine.c:218`, `cl_gameui.c:1013`,
  `host.c:905` y `lib_common.c:133,153`. Existe además `--single-binary` en
  `engine/wscript:53`.
- **La capa de plataforma ya existe**, con 16 backends, entre ellos `psvita`,
  `nswitch` y `dos`. Un `engine/platform/xbox/` es un backend más.
- **El menú no hay que reescribirlo.** `3rdparty/mainui` incluye
  `menus/Gamepad.cpp` y `menus/Gyro.cpp`, y es lo que se usa en los ports de
  consola. Reproduce el menú WON clásico.
- **Los pools de memoria ya existen.** `zone.c` (726 líneas):
  `Mem_AllocPool`/`Mem_FreePool`/`Mem_PrintStats`/`Mem_Check`, con
  atribución `__FILE__:__LINE__` y pools separados (`mempool`, `imagepool`,
  `soundpool`).
