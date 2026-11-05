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
  afetado. Mesmo trade-off do efeito de daltonismo (`colorblindnesscorrection`)
  do próprio KWin.

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

## 2.5 Caminho offscreen p/ shaders próprios (API pública, verificado 6.8)

- `OffscreenEffect` (header instalado `effect/offscreeneffect.h`) é o padrão
  do próprio KWin (daltonismo): `redirect(w)` + `setShader(w, shader)` por
  janela; **janelas não redirecionadas são encaminhadas a
  `effects->drawWindow` normalmente** — ou seja, dá para ligar/desligar o
  custo por estado, e a vibrância por uniform continua ativa no caminho
  normal. O grab aplica o `WindowPaintData` (inclusive saturação) antes do
  shader custom.
- `ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
  QString(), ":/...frag")` → `unique_ptr<GLShader>`; uniforms por nome via
  `ShaderBinder` + `setUniform("nome", QVector3D/float/…)`. Tudo público.
- Shaders `.frag` de plugin podem `#include "colormanagement.glsl"` (resolve
  no qrc do próprio kwin, ligado ao processo) e usar
  `sourceEncodingToNitsInDestinationColorspace()` /
  `nitsToDestinationEncoding()` — ganhos RGB aplicados em espaço linear,
  consistente com a vibrância nativa.
- Shader gerado **lazy** (só quando canais ≠ 1.0; nunca em construtor se o
  contexto GL não garantir), com `setUniform` imediato após criação.

## 2.6 Tolerância de versão da parte C++ (sem pin numérico)

- `Effect/OffscreenEffect::drawWindow`: `void`→`bool` no 6.8 e
  `QRegion`→`Region` no 6.6 — resolvido **com uma única definição**:
  `auto drawWindow(…) -> decltype(effects->drawWindow(…)) override`
  (handler público evolui em lockstep com a base; `return <expr void>` é
  válido em função `cv void`). Tipo do parâmetro de região via
  `WvPaintRegion` chaveado por `KWIN_EFFECT_API_VERSION_MINOR >= 237`.
- `&OffscreenEffect::drawWindow` (ponteiro-para-membro) **não** pode ser
  usado para extrair o retorno dentro do corpo da classe derivada — GCC
  trata como acesso protected ao tipo incompleto; a variante via objeto
  público (`effects->`) compila.

### Escopo "monitor primário" (0.3)

- A API pública de efeitos **não expõe um conceito de "saída primária"**
  (nada de `primaryScreen()` em `options.h`/`effecthandler.h` instalados).
  Proxy usado: `EffectsHandler::activeScreen()` — a saída sob o cursor —,
  público e estável, que em setups de jogo com um monitor "principal"
  corresponde ao que o usuário espera.
- O escopo é decidido **por janela, a cada `drawWindow`** (`w->screen()`),
  então janelas que se movem entre telas entram/saem do efeito
  naturalmente, sem precisar de sinais de reposicionamento (inexistentes
  na API pública 6.8). Fora de escopo: bypass total (nem uniform, nem
  offscreen). O set `redirect()` (ganhos RGB) continua global — texturas de
  janelas fora de escopo ficam ociosas e o base as descarta/redesenha.

## 3. Alternativas mapeadas e descartadas

| Caminho | Veredito |
|---|---|
| CTM KMS direto (libdrm) | ❌ impossível no Wayland (DRM master exclusivo) |
| Efeito JS/QML (`ShaderTrait.AdjustSaturation` scripting) | ⚠️ viável mas por-grab (texturas extras), controle ao vivo fraco (reconfigure), sem serviço D-Bus próprio |
| `OffscreenEffect` com GLSL próprio (via kwin-daltonismo) | ⚠️ mais pesado (grab por janela) — usado agora **apenas e de forma condicional** p/ ganhos RGB (ver §2.5); o modo *vibrance real* (pele protegida) pode movê-lo p/ scene pass no futuro |
| `multiplySaturation` no renderer embutido | ✅ **escolhido** — linear, sem passe extra, custo zero em neutro |
| Portar para outros compositores | ❌ fora de escopo — o projeto é **exclusivo KDE/Plasma** |

## 3.1 Estratégia de compatibilidade de versão (requisito do projeto)

- **Nenhum número de versão do Plasma/KWin é fixado em código.** O único
  gate é comportamental: `supported()` checa composição OpenGL em runtime, e
  um `#error` de piso exige apenas `KWIN_EFFECT_API_VERSION_MINOR >= 236`
  (o que a API expõe desde o KWin 6.0).
- Tray e CLI usam **somente D-Bus** (`org.wayvibrance`,
  `org.kde.kwin.Effects`) — estáveis em todo Plasma 6, zero pin.
- O plugin é compilado contra o KWin instalado (`find_package(KWin REQUIRED)`
  sem teto de versão). A API pública usada
  (`WindowPaintData::multiplySaturation`, `drawWindow`,
  `KWIN_EFFECT_FACTORY_SUPPORTED`, `requestedEffectChainPosition`) existe
  desde o KWin 6.0 e não evolui de forma destrutiva dentro do Plasma 6 —
  rebuild via `scripts/build.sh` é o único passo necessário após updates.

### Mapeamento das assinaturas de `drawWindow` (verificado no fonte, por release)

| KWin | effects API minor | 5º parâmetro | retorno |
|---|---|---|---|
| 6.0 – 6.5 | 236 | `QRegion` | `void` |
| 6.6 – 6.7 | 237 | `KWin::Region` | `void` |
| 6.8 | 237 | `KWin::Region` | `bool` |
| master (6.9-git) | 237 | `KWin::Region` | `bool` |

O efeito absorve as duas mudanças sem pin de versão:

- **Tipo de região**: `using WvPaintRegion = Region` quando
  `KWIN_EFFECT_API_VERSION_MINOR >= 237`, senão `QRegion` (chaveia pela macro
  que o próprio header define, não por palpite de release).
- **Tipo de retorno**: assinatura declarada como
  `auto drawWindow(...) -> decltype(effects->drawWindow(...))`. O tipo
  derivado é `void` em 6.0–6.7 e `bool` em 6.8+, casando com a base. Como
  `return <expr-void>` é válido dentro de função `cv void`, **uma única
  definição** compila em todo o range.

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

## 5. Números de escala

- Fator 1.0 = neutro (Windows: NV 576/1023 ≈ 100%).
- 2.0 ≈ vibrance máximo equivalente ao driver (NV 1023).
- UI do tray expõe 0–200% em todos os sliders (clamp interno [0, 2] no
  efeito — 2.0 é o teto real da cadeia de cor do KWin; 0 = P&B).
