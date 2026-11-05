# Pesquisa de viabilidade — KWin 6.8 / Wayland (2026-11-05)

Notas técnicos que fundamentaram a arquitetura do Way Vibrance. Ambiente de
referência: **CachyOS, Plasma 6.31, KWin 6.8.2, Qt 6.12, sessão Wayland**
(`qdbus6 org.kde.KWin /KWin supportInformation`).

## 1. Por que "vibrance-gui puro" é impossível no Wayland

- No Windows o vibrance-gui escreve no **CTM (Color Transformation Matrix)**
  via API do driver (NVML/ADL) ou registra um filtro no pipeline.
- No Wayland o **compositor é o DRM master**; nenhum app de usuário pode
  programar CTM/gamma do KMS (por isso `xrandr --set CTM`, `vibrant-cli`,
  etc. não funcionam). O máximo fora do compositor é *color-management*
  ICC/HDR do próprio KWin — não serve para um boost global instantâneo.
- Conclusão: o lugar correto é **dentro do compositor**, como efeito de pós
  processamento da composição. KWin é o compositor do KDE → **KWin Effect
  API**.

## 2. Anatomia de efeitos no KWin 6.8 (verificado no fonte, `KDE/kwin@master`)

### Carregamento (src/effect/effectloader.cpp / effecthandler.cpp)

- 3 famílias:
  1. **Built-ins compilados**: `kwin_add_builtin_effect(...)` → linkados ao
     binário; metadata em `/usr/share/kwin/builtin-effects/*.json` com
     `"org.kde.kwin.effect": { "internal": true }`.
  2. **Scripted** (JS/QML, tipo `KWin/Effect` KPackage):
     `/usr/share/kwin/effects/<id>/{metadata.json,contents/code/main.js|ui/main.qml}`
     e, para usuários, `~/.local/share/kwin/effects/<id>/`
     (KPackage::PackageLoader, serviço `kwin/effects`).
  3. **Plugins Qt de terceiros** (nosso caso): `PluginEffectLoader` chama
     `KPluginMetaData::findPlugins("kwin/effects/plugins")` → qualquer `.so`
     com metadata embutida instalado em
     `<XDG libdir>/kwin/effects/plugins/` (no Arch:
     `/usr/lib/qt6/plugins/kwin/effects/plugins/` — o diretório já existe).
- **KWIN_EFFECT_API_VERSION = 0.237** — sem garantia binária; rebuild a cada
  update do KWin (padrão em todos os projetos da categoria).

### Efeito de terceiros: formato de plugin confirmado

- `metadata.json`: `KPlugin.{Id, Name, Description, Icon, Category:
  "Appearance", ServiceTypes:["KWin/Effect"], Version, EnabledByDefault:false}`.
- `main.cpp`: `KWIN_EFFECT_FACTORY_SUPPORTED(ClassName, "metadata.json",
  <suported code>)` (macro em `effect/effect.h`; gera um
  `EffectPluginFactory` com IID `org.kde.kwin.EffectPluginFactory<kwinversão>`)
  + `#include "main.moc"`.
- CMake: `find_package(KWin REQUIRED)` → alvo **`KWin::kwin`**
  (`/usr/lib/cmake/KWin/KWinConfig.cmake`); instala em
  `${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins`.

### O motor de cor (a descoberta central)

- `WindowPaintData::setSaturation/multiplySaturation` (range 1.0 = neutro;
  valores > 1 **boost**). Em
  `src/scene/itemrenderer_opengl.cpp`:
  - `if (data.saturation() != 1.0) traits |= ShaderTrait::AdjustSaturation;`
  - uniform `GLShader::FloatUniform::Saturation` aplicado **"in linear
    space"** (comentário no fonte) — correto color-wise, sem textura extra:
    usa o shader já existente da composição de janelas do renderer.
- `requestedEffectChainPosition() == 99` → roda por último, sobre a aparência
  final de todas as janelas.
- `isActive()` falso (valor 1.0 ou desligado) → fora da cadeia: **custo zero**
  quando neutro.
- Limitação herdada: janela a janela → o **cursor** (hardware plane) não é
  afetado. Mesmo trade-off do Hyprland `decoration:saturation` e do efeito
  daltonismo do KWin.

### D-Bus

- `org.kde.kwin.Effects` (objeto `/Effects`): `loadEffect`, `unloadEffect`,
  `toggleEffect`, `isEffectLoaded`, `isEffectSupported`, `reconfigureEffect`,
  `activeEffects` — **hot-load/hot-unload em sessão viva, sem logout**
  (confirmado por introspecção no KWin 6.8.2). Para *rebuild* do .so a sessão
  ainda precisa reiniciar (o plugin não é descarregável com segurança).
- Um efeito pode registrar seu **próprio serviço de sessão** (código roda
  dentro de `kwin_wayland`): usado `org.wayvibrance` + `/Vibrance` para
  controle ao vivo sem tocar em `kwinrc`/`reconfigure` (zero flicker).
- Sinal `windowActivated` do workspace: base para perfis por app (roadmap).

## 3. Alternativas mapeadas e descartadas

| Caminho | Veredito |
|---|---|
| CTM KMS direto (libdrm) | ❌ impossível no Wayland (DRM master exclusivo) |
| Efeito JS/QML (`ShaderTrait.AdjustSaturation` scripting) | ⚠️ viável mas por-grab (texturas extras), controle ao vivo fraco (reconfigure), sem serviço D-Bus próprio |
| `OffscreenEffect` com GLSL próprio (via Garward/kwin-daltonismo) | ⚠️ mais pesado (grab por janela) e quebra otimizações; guardado p/ modo *vibrance real* no futuro scene pass |
| `multiplySaturation` no renderer embutido | ✅ **escolhido** — linear, sem passe extra, custo zero em neutro |
| Portar para outros compositores | Hyprland já tem nativo; wlroots em geral precisa de patch — fora de escopo (pedido: foco KDE) |

## 4. Referências

- vibrance-gui (x64christ/Vibrance-GU) e VibranceGUI (vibrancegui.com) —
  modelo de paridade (driver API + perfis + hotkey + tray).
- libvibrant / vibrant-cli — algoritmo de vibrância (preserva pele) p/ futuro.
- EfeDursun125/kwin-saturation — efeito C++ `multiplySaturation` + GUI;
  confirmou a técnica do uniform embutido.
- Garward/kwin-saturation-effect — shader GLSL próprio por grab; documenta a
  imposibilidade do CTM no Wayland; 2.0 ≈ NV "1023".
- KDE/kwin `src/effect/effectloader.cpp`, `src/scene/itemrenderer_opengl.cpp`,
  `src/plugins/colorblindnesscorrection/*` — arquitetura acima.
- develop.kde.org/docs/plasma/kwin/api — scripting API (ShaderTrait etc.).
- Hyprland wiki: `decoration:saturation` — precedente de pós-processamento
  comparável em outro compositor Wayland.

## 5. Números de escala

- Fator 1.0 = neutro (Windows: NV 576/1023 ≈ 100%).
- 2.0 ≈ vibrance máximo equivalente ao driver (NV 1023).
- UI do tray expõe 0–250% (clamp interno 0–4 no efeito; 0 = P&B).
