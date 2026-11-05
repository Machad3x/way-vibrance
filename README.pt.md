**Português** | [English](README.md)

# Way Vibrance

**Digital Vibrance / boost de saturação + ganhos RGB por canal + perfis por
aplicação para Linux no Wayland (KDE Plasma)** — o equivalente ao
[vibrance-gui](https://github.com/x64christ/Vibrance-GU) /
[VibranceGUI](https://vibrancegui.com) do Windows, onde as ferramentas de
driver não existem. Funciona em **qualquer sessão Wayland do KDE Plasma 6**
com composição OpenGL: o ajuste de cor acontece dentro do próprio KWin (o
único compositor dono do display) como um efeito de desktop padrão —
visualmente equivalente à vibrância de driver.

| Janela principal | Editor de perfil | Menu da bandeja |
|---|---|---|
| ![Janela principal](docs/screenshots/app.png) | ![Editor de perfil](docs/screenshots/profiles.png) | ![Menu da bandeja](docs/screenshots/tray.png) |

## O que você recebe

- Vibrância global (0–200%) + ganhos R/G/B por canal, presets, bandeja + autostart
- **Perfis por aplicação/jogo**: globs `app:` (app_id/WM_CLASS) ou `title:`,
  cada um com vibrância + ganhos próprios, aplicados **por janela** (mesmo
  sem foco) com custo extra de GPU zero
- Hotkey global configurável, seletor de monitor para "só o monitor primário"
- CLI, estado persistente (`kwinrc`) e pegada **só em espaço do usuário: sem
  rede, sem daemon, sem telemetria**
- Guia completo, API D-Bus e todos os detalhes: **[docs/FEATURES.md](docs/FEATURES.md)** (em inglês)

## Requisitos

- KDE Plasma 6 (KWin ≥ 6.0), sessão Wayland, composição OpenGL.
- Validado: Arch (KWin 6.8), Fedora, Debian 13 e Ubuntu 25.04 (6.3.4) — o
  mesmo fonte compila de 6.0 até a versão mais recente, sem pin de versão.

## Instalação

**Pacotes prebuilt** (da próxima release em diante): baixe o artefato da sua
distro na [última release](https://github.com/Machad3x/way-vibrance/releases)
— `.rpm` (Fedora; no Silverblue/Kinoite: `rpm-ostree install ./way-vibrance-*.rpm`),
`.deb` (Debian 13+/Ubuntu 24.10+/Neon), `.pkg.tar.zst` (Arch).

**Ou compile (qualquer distro suportada):**

```
./scripts/build.sh          # instala só o que falta de dependências — nunca atualiza o sistema
sudo cmake --install build  # (o build.sh faz isso sozinho quando roda com sudo)
```

Depois faça **logout + login** para o KWin carregar o plugin (binários de
plugin não podem ser trocados a quente). Ative em *Configurações do Sistema
→ Efeitos da Área de Trabalho → Aparência → Way Vibrance* ou apenas rode
`wayvibrance-tray`.

## Uso

- **Bandeja/janela**: sliders de vibrância e canais RGB, presets
  (50/100/125/150/200%), editor de perfis por app, campo de hotkey e
  checkboxes de comportamento com seletor de monitor.
- **CLI**: `wayvibrance --help` mostra tudo
  (`wayvibrance 1.5`, `toggle`, `rgb …`, `primary …`, `app list|set|del|name|reset|focused`).

## Notas que importam

- O cursor do mouse não é afetado (hardware plane) e ganhos RGB ≠ 100% usam
  o caminho offscreen (um pouco mais pesado, mas ainda barato). A lista
  completa de [limitações](docs/FEATURES.md#known-limitations) e os
  [detalhes de compatibilidade de versão](docs/FEATURES.md#version-compatibility)
  estão na documentação.
- Tudo roda em espaço do usuário: sem daemons, sem rede, sem telemetria;
  escreve apenas em `~/.config/{kwinrc,wayvibrancerc}` —
  [privacidade & pegada](docs/FEATURES.md#privacy--footprint).

## Roadmap

Vibrância "de verdade", valores por saída, HDR, backend Vulkan, pacotes
prebuilt/CI, OSD — veja **[docs/ROADMAP.md](docs/ROADMAP.md)** (em inglês).

## Estado

Funcional e em uso diário; release atual **v0.5.0** (seletor do monitor
primário). Perfis por aplicação vieram na v0.4.0; suporte multi-distro,
empacotamento e a reorganização da doc estão na fila da próxima release.

## Desenvolvimento

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
scripts/dev.sh {load|unload|toggle|status}   # hot-load na sessão
scripts/package.sh [fedora|debian|ubuntu|arch]  # prebuilts → dist/
```

Docs: [features & referência](docs/FEATURES.md) ·
[roadmap](docs/ROADMAP.md) ·
[RESEARCH.md](docs/RESEARCH.md) (as descobertas da API do KWin por trás de
cada decisão de projeto).

## Assistência de IA

Este projeto foi **vibe-coded com assistência de IA**: código, pesquisa e
documentação foram gerados e refinados junto com um agente de codificação
(OpenCode), sob direção e revisão do mantenedor. O histórico completo da
colaboração está preservado no git log.

## Licença

GPL-2.0-or-later (compatível com o KWin). Veja [LICENSE](LICENSE).
