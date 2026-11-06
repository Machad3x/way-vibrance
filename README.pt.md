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
- Guia completo, API D-Bus e todos os detalhes: **[docs/FEATURES.pt.md](docs/FEATURES.pt.md)**

## Requisitos

- KDE Plasma 6 (KWin ≥ 6.0), sessão Wayland, composição OpenGL.
- Validado na matriz de contêineres: Arch (KWin 6.8), Fedora 44/45, Debian
  13/14 (testing), Ubuntu 26.04/26.10 — o mesmo fonte, sem pin de versão.

## Instalação

**Pacotes prebuilt**: baixe o artefato da sua distro **e release exatos** na
[última release](https://github.com/Machad3x/way-vibrance/releases) — o
efeito é compilado contra o KWin daquele release (a API de efeitos não tem
promessa binária entre versões):

- RPM `.fc44/.fc45` — Fedora; no Atomic (Silverblue/Kinoite):
  `rpm-ostree install ./way-vibrance-*.rpm`
- `.deb` — Debian 13/14-testing (`~debian13`/`~debian14`) e Ubuntu
  26.04/26.10 (`~ubuntuXX.YY`); instale com `sudo apt install ./…deb`
- `.pkg.tar.zst` — Arch & derivadas (`sudo pacman -U …`)
- Pacotes são fornecidos **apenas para releases ainda suportadas upstream**
  (ex.: Ubuntu 25.04/25.10 saíram da matriz ao chegarem ao EOL).
- Para regenerar tudo localmente: `scripts/package.sh` (matriz podman)

**Ou compile (qualquer distro suportada):**

```
./scripts/build.sh   # checa dependências (instala só o que falta, nunca
                     # atualiza o sistema) → configure → build → install
```

Controle manual: `cmake --build build && sudo cmake --install build`.

Depois faça **logout + login** para o KWin carregar o plugin (binários de
plugin não podem ser trocados a quente). Ative em *Configurações do Sistema
→ Efeitos da Área de Trabalho → Aparência → Way Vibrance* ou apenas rode
`wayvibrance-tray`.

## Desinstalação

`./scripts/uninstall.sh` — faz o hot-unload do efeito na sessão em execução
e remove exatamente o que o `scripts/build.sh` instalou. Usa o manifesto do
build, aceita `--prefix ~/.local` (instalações sem root), `--dry-run` para
prévia; só pede sudo quando é necessário.

A configuração é **mantida** por padrão e seus caminhos são impressos —
adicione `--purge-config` para também remover a seção `[Effect-wayvibrance]`
e seu subgrupo de perfis do `kwinrc` e apagar `wayvibrancerc` + a entrada de
autostart.

Instalou via **pacote**? Use o gerenciador da sua distro:
`pacman -Rns way-vibrance` · `dnf remove way-vibrance` ·
`apt remove way-vibrance` · `rpm-ostree uninstall way-vibrance`.

## Uso

- **Bandeja/janela**: sliders de vibrância e canais RGB — cada porcentagem
  tem um pequeno botão circular **↺** que restaura só aquele slider ao padrão
  (100%) — presets (0/25/50/75/100/125/150/175/200%), editor de perfis por app, campo de
  hotkey e chaves liga/desliga para *Enabled* e *Comportamento* com seletor
  de monitor. Cartões e chaves seguem o tema claro/escuro do sistema; o painel
  de perfis se explica quando está vazio; os sliders R/G/B têm cores de canal
  acessíveis e os presets formam um único controle segmentado. As chaves
  alternam com Espaço ou Enter, mantêm anel de foco visível e continuam
  mostrando a posição quando desabilitadas; não têm animação.
- **CLI**: `wayvibrance --help` mostra tudo
  (`wayvibrance 1.5`, `toggle`, `rgb …`, `primary …`, `app list|set|del|name|reset|focused`).

## Notas que importam

- O cursor do mouse não é afetado (hardware plane) e ganhos RGB ≠ 100% usam
  o caminho offscreen (um pouco mais pesado, mas ainda barato). A lista
  completa de [limitações](docs/FEATURES.pt.md#limitações-conhecidas) e os
  [detalhes de compatibilidade de versão](docs/FEATURES.pt.md#compatibilidade-de-versão)
  estão na documentação.
- Tudo roda em espaço do usuário: sem daemons, sem rede, sem telemetria;
  escreve apenas em `~/.config/{kwinrc,wayvibrancerc}` —
  [privacidade & pegada](docs/FEATURES.pt.md#privacidade--pegada).
- Segurança frente a anticheats: a correção de cor ocorre no compositor do
  desktop, *depois* de o jogo apresentar seus frames — nenhuma interação
  com o processo do jogo, a mesma categoria da vibrância de driver
  ([detalhes](docs/FEATURES.pt.md#segurança-frente-a-anticheats)).

## Roadmap

Vibrância "de verdade", valores por saída, HDR, backend Vulkan, pacotes
prebuilt/CI, OSD — veja **[docs/ROADMAP.pt.md](docs/ROADMAP.pt.md)**.

## Estado

Funcional e em uso diário; release atual **v0.6.2** — a linha de hardening concluída (rodadas 2–10 da
auditoria estática, nenhuma mudança de comportamento): confiança D-Bus
ancorada no PID do compositor e revalidada a cada troca de dono, tetos de
string em todos os pontos de entrada incluindo as identidades de janela, um
tray totalmente assíncrono e à prova de congelamento, matching de glob
orçado com teste fuzz versionado no CI e desinstalador à prova de manifesto
adulterado e de symlinks. Suporte
multi-distro de build (Arch/Fedora/Debian/Ubuntu, sem nunca atualizar o
sistema),
**pacotes prebuilt** por release compilados e verificados por instalação em
matrizes de contêineres (anexados às releases automaticamente pela CI),
seleção real da saída do monitor primário e documentação totalmente
bilíngue. A base da v0.4.0/v0.5.0 (perfis por app, seletor de monitor)
permanece.

## Desenvolvimento

O teste versionado do matcher (`tests/glob_match_test.cpp`, roda com
`ctest --test-dir build -R glob_match` e no CI) verifica por fuzz o glob matcher
contra a referência congelada da v0.6.1 e os tetos de orçamento de passos.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
scripts/dev.sh {load|unload|toggle|status}   # hot-load na sessão
scripts/package.sh [fedora|debian|ubuntu|arch]  # prebuilts → dist/
```

Docs (todas bilíngues — EN é o padrão, PT no seletor de cada página):
[recursos & referência](docs/FEATURES.pt.md) ·
[roadmap](docs/ROADMAP.pt.md) ·
[RESEARCH.pt.md](docs/RESEARCH.pt.md) (as descobertas da API do KWin por
trás de cada decisão de projeto).

## Assistência de IA

Este projeto foi **vibe-coded com assistência de IA**: código, pesquisa e
documentação foram gerados e refinados junto com um agente de codificação
(OpenCode), sob direção e revisão do mantenedor. O histórico completo da
colaboração está preservado no git log.

## Licença

GPL-2.0-or-later (compatível com o KWin). Veja [LICENSE](LICENSE).
