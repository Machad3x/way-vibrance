# Way Vibrance

**Digital Vibrance para Linux + Wayland (KDE Plasma)** — o equivalente ao
[vibrance-gui](https://github.com/x64christ/Vibrance-GU) /
[VibranceGUI](https://vibrancegui.com) do Windows, onde as ferramentas de
driver (NVIDIA Digital Vibrance / AMD Saturation) não existem.

No Wayland nenhum aplicativo de usuário pode escrever no CTM (Color
Transformation Matrix) do display: o compositor (KWin) é o único dono do DRM
master. A solução nativa é fazer o ajuste de cor acontecer **dentro do
compositor**, como um efeito de pós-processamento — visualmente equivalente
ao vibrance do driver.

## Como funciona

- **`effect/`** — plugin C++ de *desktop effect* do KWin 6. Aplica o multiplicador
  de saturação via `WindowPaintData::multiplySaturation()` no fim da cadeia de
  efeitos (`chainPosition 99`). O renderizador de cena do KWin aplica isso no
  **espaço linear** com seu shader embutido: sem grab por janela, sem passe
  extra de framebuffer.
  - Valor persiste em `kwinrc` (`[Effect-wayvibrance] Vibrance=…`).
  - Controle ao vivo (sem flicker/`reconfigure`) via **D-Bus**:
    serviço `org.wayvibrance`, objeto `/Vibrance`, interface `org.wayvibrance`
    com métodos `vibrance`, `setVibrance`, `enabled`, `setEnabled` e sinais
    `vibranceChanged`, `enabledChanged`.
- **`tray/`** — `wayvibrance-tray`: app de bandeja em Qt 6 (slider ao vivo
  com throttle, presets, liga/desliga, autostart) — é o "GUI" do vibrance-gui.
- **`cli/`** — `wayvibrance`: script de terminal (`wayvibrance 1.25`,
  `wayvibrance toggle`, `wayvibrance get`).

## Requisitos

- KDE Plasma 6 com KWin ≥ 6.2 (testado em **6.8.2**), sessão **Wayland**,
  composição OpenGL.
- Para compilar: Arch/CachyOS → `scripts/build.sh` instala tudo
  (`base-devel cmake extra-cmake-modules kwin qt6-base kconfig vulkan-headers`).

## Instalação

```
./scripts/build.sh
```

Depois: **logout/login** para KWin carregar o plugin (em sessão já aberta use
`scripts/dev.sh load`; mudanças binárias posteriores exigem reiniciar a sessão).
Ative também em *Configurações do Sistema → Efeitos de Área de Trabalho →
Appearance → Way Vibrance*, ou deixe o tray carregá-lo sozinho via D-Bus.

## Uso

- Bandeja/menu: slider 0–250%, presets (100/125/150/200%), toggle, "Start with Plasma".
- CLI: `wayvibrance 1.5` · `wayvibrance toggle` · `wayvibrance get`
- Reset rápido: valor `1.0` = neutro (efeito fica inativo, custo zero).

## Equivalência com o vibrance-gui

| Recurso | vibrance-gui (Windows) | Way Vibrance | status |
|---|---|---|---|
| Boost de vibrância global | driver CTM | shader do KWin | ✅ MVP |
| Ajuste ao vivo sem reiniciar | driver API | D-Bus `org.wayvibrance` | ✅ MVP |
| Persistência por sessão | registro | `kwinrc` | ✅ MVP |
| Tray + autostart | ✅ | ✅ | ✅ MVP |
| Presets | perfis | presets 100–200% | ✅ MVP |
| Hotkey para alternar | ✅ | KGlobalAccel no tray | 🔜 roadmap |
| Perfis automáticos por app/jogo | ✅ (foco de janela) | `windowActivated` + regras | 🔜 roadmap |
| Ajuste por monitor | ✅ | scene pass por-output | 🔜 roadmap |
| Som/OSD ao trocar perfil | ✅ | OSD layer-shell | 🔜 roadmap |

## Limitações conhecidas

- **Cursor do mouse não é afetado** (é um hardware plane à parte no KMS).
  Hyprland (`decoration:saturation`) e o efeito de daltonismo do próprio KWin
  têm a mesma limitação. No driver NVIDIA isso também pode variar.
- Composição OpenGL apenas (Vulkan no roadmap — API pública ainda limitada).
- Plugin compilado exige a **mesma versão do KWin** (API de efeitos do KWin
  não tem garantia binária; o CI/pacote deve ser rebuildado a cada update).
- Com efeito ativo, janelas em fullscreen exclusivo podem perder o
  *direct scanout* no compositor (leve custo de latência; medir).

## Roadmap

1. Hotkey global (KGlobalAccel) para toggle — paridade com o feature mais
   usado do vibrance-gui.
2. Perfis por aplicação: valor automático no `windowActivated`
   (por `desktopFileName`/`resourceClass`/fullscreen) com reversão ao sair.
3. Vibrância "de verdade" (protege tons de pele — algoritmo do
   [libvibrant](https://github.com/libvibrant/vibrant-cli)) como modo
   opcional, via shader GLSL próprio em scene pass.
4. Controle por saída/monitor + HDR (respeitar space linear em BT.2020).
5. Explorar portabilidade para wlroots (Hyprland já tem nativo; KWin é o
   foco primeiro).

## Estado

MVP funcional (esqueleto pronto para compilar). Veja `docs/RESEARCH.md` para
a pesquisa de API do KWin 6.8 que fundamentou as escolhas.

## Licença

GPL-2.0-or-later (compatível com o KWin).
