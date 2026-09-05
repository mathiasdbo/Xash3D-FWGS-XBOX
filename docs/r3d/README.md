# Resonance3D — fork de Xash3D-FWGS

Este repositorio **es** Xash3D-FWGS, con un conjunto acotado de cambios para
llevarlo a la Xbox Original (2001) sin perder el build de PC.

Base del fork: `c7768c48` (upstream/master, 2026-08-08).

## Por qué existe

El objetivo es una Xbox Original corriendo Half-Life con paridad de features.
El proyecto anterior (`ferrum56/`, Rust) reimplementaba el motor desde cero:
llegó a ~35.000 líneas y seguía lejos de la paridad. Este fork parte del motor
completo (~221.000 líneas) y le añade lo que la consola necesita.

Lo que se conserva del proyecto anterior es lo único que era genuinamente
original: el baker offline, ahora en `tools/asset-studio/`.

Ver `migration.md` para el detalle de qué se archivó y por qué.

## Estructura

```
<raíz>              ← árbol de upstream, sin mover nada
  engine/  ref/  filesystem/  public/  common/  3rdparty/  wscript  ...

tools/asset-studio/ ← NUESTRO. Baker offline C++/CMake que produce .mpak
docs/r3d/        ← NUESTRO. Plan, registro de divergencias, migración
```

El árbol de upstream se mantiene **en su sitio exacto**. Todo lo nuestro vive
en directorios que upstream no usa (`tools/`, `docs/r3d/`), de modo que
`git merge upstream/master` nunca conflictúe por layout. Esto no es
cosmético: es lo que decide si el fork se puede mantener al día o no.

> `scripts/` y `Documentation/` YA EXISTEN en upstream. No poner nada nuestro
> ahí.

## Remotes

```
upstream  https://github.com/FWGS/xash3d-fwgs.git   (fetch)
upstream  no-push                                    (push deshabilitado a propósito)
origin    <repo privado, pendiente>
```

Traer cambios de upstream:

```bash
git fetch upstream
git merge upstream/master        # resolver contra docs/r3d/divergences.md
```

## Reglas del fork

Están en `/AGENTS.md`, en la raíz. La regla central invierte lo que decía el
proyecto Rust: antes había que **replicar** la referencia, ahora **somos** la
referencia, y lo que hay que hacer es **minimizar y documentar cada
divergencia**. Toda desviación de upstream va al registro
(`divergences.md`) el mismo día que se escribe, no después.

## Estado

Nada portado todavía. El árbol es upstream limpio más el baker. El plan por
fases está en `fork-plan.md`.
