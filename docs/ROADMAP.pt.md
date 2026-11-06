[English](ROADMAP.md) | [Português](ROADMAP.pt.md)

# Way Vibrance — Roadmap

Extraído do README; o [README](../README.pt.md) mantém só um resumo.
Voltar ao [README](../README.pt.md) · [Recursos](FEATURES.pt.md).

1. **Vibrância "de verdade"** — protege tons de pele (algoritmo
   [libvibrant](https://github.com/libvibrant/vibrant-cli)) como modo
   opcional de shader.
2. **Valores por saída** — cada monitor com vibrância/ganhos próprios (o
   *seletor* de monitor já existe desde a v0.5.0; valores por saída pedem um
   uniform/passe de cena por saída).
3. **HDR / fidelidade em wide gamut** — correção consciente de BT.2020.
4. **Composição Vulkan** — aplicar saturação/ganhos no backend Vulkan do
   KWin (hoje o efeito recusa carregar lá em vez de virar no-op silencioso).
5. **Pacotes prebuilt** — entregues na próxima release via
   `scripts/package.sh`, **um artefato por release de distro** (o efeito é
   compilado contra o KWin daquele release; a API de efeitos não tem
   garantia binária entre versões):
   - RPM → Fedora 44, 45; no **Fedora Atomic/Kinoite** instale com
     `rpm-ostree install` — verificado: KWin 6.3–6.8 só carrega efeitos de
     diretórios de sistema, então escopo de usuário não é opção para o
     efeito;
   - DEB → Debian 13 + 14 (testing) e Ubuntu 26.04/26.10 — só releases
     suportadas; releases em EOL saem da matriz (25.04/25.10 removidos com o
     fim do suporte da Canonical);
   - PKGBUILD → Arch (rebuild automático estilo AUR a cada update do KWin).
   CI: a matriz podman roda cada build; um GitHub Action poderia publicar
   `dist/` automaticamente nas tags de release.
6. **Som/OSD ao trocar perfil** (paridade com vibrance-gui) — OSD
   layer-shell.

### Fora de escopo, de propósito
- **Sessão de jogos do SteamOS**: o gamescope não roda KWin, então um
  efeito de KWin não pode colorir os jogos lá; o efeito vale só para a
  sessão desktop do SteamOS.
- **Flatpak**: apps em sandbox não podem carregar plugins no KWin do host.
