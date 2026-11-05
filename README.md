# Way Vibrance

**Digital Vibrance para Linux + Wayland (KDE Plasma)** — o equivalente ao
[vibrance-gui](https://github.com/x64christ/Vibrance-GU) /
[VibranceGUI](https://vibrancegui.com) do Windows, onde as ferramentas de
driver (NVIDIA Digital Vibrance / AMD Saturation) não existem.
Funciona em qualquer sessão Wayland do **KDE Plasma 6**: o projeto usa
apenas a API pública e documentada de efeitos do KWin, sem depender de uma
versão específica do Plasma.

No Wayland nenhum aplicativo de usuário pode escrever no CTM (Color
Transformation Matrix) do display: o compositor (KWin) é o único dono do DRM
master. A solução nativa é fazer o ajuste de cor acontecer **dentro do
compositor**, como um efeito de pós-processamento — visualmente equivalente
ao vibrance do driver.

## Compatibilidade de versão

- O `wayvibrance-tray` e o `wayvibrance` (CLI) conversam apenas com D-Bus
  (`org.wayvibrance` próprio + `org.kde.kwin.Effects`), que é estável em todo
  Plasma 6 — **não dependem de versão nenhuma do KDE**.
- O plugin do efeito é compilado **contra o KWin instalado na sua máquina**.
  Essa é a regra do gênero (a API de efeitos do KWin não garante
  compatibilidade binária entre versões), e é justamente o que dá a
  portabilidade de *fonte*: o mesmo código compila em qualquer KWin 6.x
  (6.0 → 6.9-git) que exponha `WindowPaintData::multiplySaturation`,
  adaptando automaticamente as variações de assinatura do `drawWindow`
  (`QRegion`→`Region` no 6.6, `void`→`bool` no 6.8) — sem pin de versão.
  Em atualizações grandes do Plasma, basta rodar `scripts/build.sh` de novo
  — nada além disso.

## Como funciona

- **`effect/`** — plugin C++ de *desktop effect* do KWin 6, com dois caminhos
  de render escolhidos por custo:
  - **Saturação/vibrance** via `WindowPaintData::multiplySaturation()` no fim
    da cadeia de efeitos (`chainPosition 99`): o renderizador de cena do KWin
    aplica com seu shader embutido, no **espaço linear**, sem grab de janelas
    e sem passe extra.
  - **Ganhos RGB por canal** (vermelho/verde/azul independentes — como a aba
    "Cor" do painel NVIDIA): quando algum canal ≠ 100%, as janelas passam
    pelo caminho *offscreen* (`OffscreenEffect`) com shader GLSL próprio que
    multiplica os canais em espaço linear. Com os três canais neutros, nada é
    redirecionado e o custo volta a ser zero.
  - Valores persistem em `kwinrc` (`[Effect-wayvibrance]` → `Vibrance`,
    `Enabled`, `PrimaryOnly`, `RedGain`, `GreenGain`, `BlueGain`).
  - Controle ao vivo (sem flicker/`reconfigure`) via **D-Bus**:
    serviço `org.wayvibrance`, objeto `/Vibrance`, interface `org.wayvibrance`
    com métodos `vibrance`, `setVibrance`, `enabled`, `setEnabled`,
    `toggleEnabled`, `primaryOnly`, `setPrimaryOnly`,
    `redGain`/`greenGain`/`blueGain`, `setRGBGains(r,g,b)` e sinais
    `vibranceChanged`, `enabledChanged`, `rgbGainsChanged`,
    `primaryOnlyChanged`.
- **`tray/`** — `wayvibrance-tray`: app de bandeja em Qt 6 (slider ao vivo
  com throttle, 3 sliders de ganho RGB, presets, liga/desliga, hotkey
  global configurável, autostart) — é o "GUI" do vibrance-gui.
- **`cli/`** — `wayvibrance`: script de terminal (`wayvibrance 1.25`,
  `wayvibrance toggle`, `wayvibrance get`, `wayvibrance rgb R G B`).

## Requisitos

- **KDE Plasma 6** (KWin 6.0 ou mais novo) em sessão **Wayland**, composição OpenGL.
- Para compilar (Arch/CachyOS): `scripts/build.sh` instala tudo
  (`base-devel cmake extra-cmake-modules kwin qt6-base kconfig kcoreaddons
  kdbusaddons kglobalaccel vulkan-headers`).

## Instalação

```
./scripts/build.sh
```

Depois: **logout/login** para KWin carregar o plugin (em sessão já aberta use
`scripts/dev.sh load`; mudanças binárias posteriores exigem reiniciar a sessão).
Ative também em *Configurações do Sistema → Efeitos de Área de Trabalho →
Appearance → Way Vibrance*, ou deixe o tray carregá-lo sozinho via D-Bus.

## Uso

- Bandeja/menu: slider 0–250%, **3 sliders de ganho por canal (R/G/B,
  0–200%) com botão "Reset RGB"**, presets (100/125/150/200%), toggle,
  "Start with Plasma".
- **Hotkey global**: sem atalho padrão (não brigamos com o Plasma). Clique no
  campo *Global hotkey* da janela do tray e digite a combinação — aplicada e
  persistida na hora (em `wayvibrancerc`, re-registrada no KGlobalAccel a cada
  sessão), com detector de conflito e botão "Clear". Também editável em
  *Configurações do Sistema → Atalhos de Teclado → Atalhos da Aplicação →
  Way Vibrance* (mudanças feitas lá são absorvidas pelo tray). Requer o
  tray rodando — use "Start with Plasma".
- CLI: `wayvibrance 1.5` · `wayvibrance toggle` · `wayvibrance get` ·
  `wayvibrance rgb 1.2 1 0.9` · `wayvibrance rgb reset` · `wayvibrance getrgb` ·
  `wayvibrance primary on|off|get`
- **Comportamento** (grupo na janela, um check cada): *Open directly to the
  tray on startup* (abre sem mostrar a janela; acesso pelo ícone), *Start
  with Plasma* (autostart), *Only affect the primary monitor* (a correção de
  cor vale só para a tela ativa — onde está o cursor; as demais janelas são
  compostas intocadas).
- Reset rápido: vibrância `1.0` e canais `1.0` = neutro (efeito fica
  inativo, custo zero).

## Equivalência com o vibrance-gui

| Recurso | vibrance-gui (Windows) | Way Vibrance | status |
|---|---|---|---|
| Boost de vibrância global | driver CTM | shader do KWin | ✅ MVP |
| Ganhos por canal R/G/B | driver (aba Cor) | passe offscreen com GLSL | ✅ |
| Ajuste ao vivo sem reiniciar | driver API | D-Bus `org.wayvibrance` | ✅ MVP |
| Persistência por sessão | registro | `kwinrc` | ✅ MVP |
| Tray + autostart | ✅ | ✅ | ✅ MVP |
| Presets | perfis | presets 100–200% | ✅ MVP |
| Hotkey para alternar | ✅ | KGlobalAccel no tray (tecla configurável) | ✅ |
| Perfis automáticos por app/jogo | ✅ (foco de janela) | `windowActivated` + regras | 🔜 roadmap |
| Aplicar só ao monitor primário | — | checkbox "Only affect the primary monitor" | ✅ |
| Ajuste por monitor com valores próprios | ✅ (perfis) | valores por-output | 🔜 roadmap |
| Som/OSD ao trocar perfil | ✅ | OSD layer-shell | 🔜 roadmap |

## Limitações conhecidas

- **Cursor do mouse não é afetado** (é desenhado em um hardware plane
  separado no KMS), mesmo trade-off do efeito de correção de daltonismo do
  próprio KWin.
- **Ganhos RGB ≠ 100% ativam o caminho offscreen** (grab por janela +
  shader): um pouco mais pesado que a vibrância pura, que continua no
  caminho de custo zero via uniform nativo. Deixe os três canais em 100%
  para voltar ao modo leve.
- Composição OpenGL apenas (Vulkan no roadmap).
- O binário do plugin é casado com a versão do KWin instalado (rebuild leve
  via `scripts/build.sh` após updates grandes — sem pin de versão no código).
- Com efeito ativo, janelas em fullscreen exclusivo podem perder o
  *direct scanout* no compositor (leve custo de latência; medir).

## Roadmap

1. ~~Hotkey global (KGlobalAccel) para toggle~~ ✅ feito (padrão Meta+V,
   configurável na janela do tray e em Configurações do Sistema → Atalhos).
2. Perfis por aplicação: valor automático no `windowActivated`
   (por `desktopFileName`/`resourceClass`/fullscreen) com reversão ao sair.
3. Vibrância "de verdade" (protege tons de pele — algoritmo do
   [libvibrant](https://github.com/libvibrant/vibrant-cli)) como modo
   opcional, via shader GLSL próprio em scene pass.
4. Controle por saída/monitor + HDR (respeitar space linear em BT.2020).
5. PKGBUILD (modelo AUR `-git`/`-bin` com rebuild) para que cada atualização
   do KWin no seu distro recompile o efeito automaticamente — o mecanismo
   padrão de manter compatibilidade com todo Plasma 6.x.

## Estado

MVP funcional (esqueleto pronto para compilar). Veja `docs/RESEARCH.md` para
a pesquisa de API do KWin 6.8 que fundamentou as escolhas.

## Licença

GPL-2.0-or-later (compatível com o KWin).
