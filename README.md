# Click Between Frames — port a GD 2.113 / Geode v1.3.10

Reescritura del mod de [theyareonit/Click-Between-Frames](https://github.com/theyareonit/Click-Between-Frames) (MIT) para
Geometry Dash **2.113** (32 bits, Windows) con **Geode v1.3.10**.

> **Sin probar.** El código se escribió contra los bindings de Geode v1.3.10 pero no se pudo compilar ni ejecutar
> (no hay MSVC ni GD 2.113 en el entorno donde se escribió). Hay que compilarlo con el workflow y probarlo en el juego.

## Por qué no es un port directo

El CBF original (v1.5.0, GD 2.2081) se apoya en cosas que no existen en 2.113: `PlayerButtonCommand`,
`m_queuedButtons`, `GJBaseGameLayer::processCommands/getModifiedDelta/handleButton`, `m_gameState`, y parches en
offsets x64 de 2.2081. En 2.113 la física es la de 2.1 (`PlayLayer::update(dt)`, sin cola de inputs ni timestamps),
así que la técnica es otra:

1. `src/input.cpp`: un hilo con raw input (QPC) timestampea salto (Espacio, ↑, W, clic izquierdo) al instante.
2. `GJBaseGameLayer::pushButton/releaseButton` se interceptan en partida y se guardan con su timestamp.
3. `PlayLayer::update(dt)` se parte: `update(dt·f)` → se aplica el input → `update(dt·(1−f))`.
   Los frames sin input se ejecutan igual que en vanilla.

Quitado respecto a v1.5.0: Click on Steps, Physics Bypass, Precision Fix, Mouse Fix, Wine workaround/linux-input,
Right Click P2, marca "CBF" en la pantalla final, plataformas Android/macOS/iOS.

## Compilar (GitHub Actions)

1. Sube esta carpeta a un repo de GitHub.
2. Actions → *Build (Geode v1.3.10 / GD 2.113)* → Run workflow.
3. Descarga el artefacto `local.click_between_frames_2113.geode` y ponlo en la carpeta `geode/mods` de GD 2.113.

El workflow fija el SDK a `v1.3.10`, descarga `Geode.lib` del release oficial, compila con
`-A win32` (como el CI de Geode) y empaqueta el `.geode` a mano (un `.geode` es un zip con `mod.json` + `<id>.dll`).

## Si algo falla

- **Errores de compilación con MSVC reciente**: Geode v1.3.10 es de diciembre de 2023. Si da errores de la STL,
  prueba con `windows-2019` en `runs-on`, o con clang-cl como indica el comentario de `build.yml` del propio Geode.
- **Crash o comportamiento raro al entrar a un nivel**: activa "Disable CBF" en los ajustes del mod para comprobar
  que es el mod. La causa más probable es que `PlayLayer::update` de 2.113 no tolere partirse en dos llamadas
  (la parte más arriesgada del port, en `PlayLayer::update` de `src/main.cpp`).
- **El salto va retrasado**: bajo Wine/Linux el raw input de teclado no llega; el mod cae a vanilla.
