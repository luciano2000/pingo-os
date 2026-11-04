# Fase 1: sistema utilizável (RG35XX-H)

## Decisões (2026-11-04)

| Tema | Decisão | Motivo |
|---|---|---|
| Frontend | Escrito do zero, a partir do `pingo-retro` da Fase 0 | O MinUI não tem licença (todos os direitos reservados) e está arquivado; só aproveitamos ideias de UX |
| Arquitetura | Dois programas: launcher e `pingo-retro`; o launcher abre o jogo como outro processo | Core que trava ou vaza memória não derruba o launcher |
| Suspensão prolongada | Fica suspenso até a bateria chegar a 10 % (desligamento de emergência) ou por no máximo 24 h; aí salva e desliga | O deep suspend gasta ~2 %/h (Fase 0); 10-15 min jogaria fora essa vantagem |
| Pastas | Códigos curtos no cartão (`GB`, `SNES`, `MD`...); o launcher mostra o nome completo | Fácil de digitar no PC, sem acentos nem espaços |
| Saves | Nomes do RetroArch (`Jogo.srm`); save states em formato próprio | Saves do ROCKNIX/RetroArch funcionam sem renomear |
| Idioma | Português, com os textos num arquivo separado | O inglês entra depois sem retrabalho |

## Fatias

1. **Cartão:** partição `ROMS` (exFAT) no primeiro boot, `fsck` em todo boot, pastas, dados do sistema persistentes, imagem sem o Clang.
2. **Launcher:** Início, lista de jogos como navegador de pastas (subpastas em qualquer nível, como o MinUI), cache por pasta, fonte Inter, barra com relógio, Wi-Fi e bateria. Boot direto no launcher.
3. **Jogo completo:** menu dentro do jogo, 4 slots com prévia, SRAM, escala Ajustada/Inteira, sincronização pelo áudio.
4. **Energia:** Power suspende/retoma, auto-resume, segurar 2 s salva e desliga.
5. **Cores:** todos da seção 4 da especificação, BIOS, PS1 (`.chd`, `.m3u`), filtros de tela.
6. **Arcade:** FBNeo com nomes vindos do DAT.
7. **SD1/SD2:** origem por console.

## Fatia 1: cartão

`/etc/init.d/S06pingo-storage`:

- Acha o disco do sistema pelo `root=PARTUUID=` do cmdline.
- Sem partição 3 e com pelo menos 1 GiB livre: mostra "Preparando cartão..." na tela, cria a partição 3 (tipo `07`) no espaço restante com `sfdisk --append`, registra no kernel com `partx -a` (o disco está em uso, não dá para reler a tabela inteira) e formata em exFAT com o rótulo `ROMS`.
- **Regravar a `.img` não apaga os jogos:** o `dd` troca a tabela de partições, mas a partição 3 é recriada no mesmo lugar; se já existir um exFAT `ROMS` ali, ele é reaproveitado sem formatar.
- Em todo boot: `fsck.exfat -p` e montagem em `/mnt/sd1` (`noatime,errors=remount-ro`).
- Uma partição 3 que não seja exFAT nunca é tocada.

Estrutura criada:

```
Roms/<GB GBC GBA NES SNES VB POKEMINI MS GG MD SEGACD 32X PCE PCECD NGP WS MSX A2600 A7800 LYNX PS ARCADE>
Bios/  Saves/  States/  Ports/  Config/
.system/   dropbear (chaves SSH), cache (shaders do Mesa), logs
```

Outros ajustes:

- Chaves SSH persistentes: `/var/run/dropbear` aponta para `.system/dropbear`, então o PC não reclama mais de chave trocada a cada boot.
- `XDG_CACHE_HOME=/mnt/sd1/.system/cache` (`/etc/profile.d/pingo.sh`): o Mesa guarda os shaders compilados.
- O `post-build.sh` remove o Clang, o `libLLVMSPIRVLib` e os shaders de OpenCL (~145 MB): o Mesa só precisa deles no build.

### Checklist

- [x] Imagem sem o Clang (131 MB, 57 MB em `.xz`); `kmscube` e `pingo-retro` a 60 fps; cache de shaders na `ROMS`
- [x] Primeiro boot cria e formata a `ROMS` (118,5 GB no cartão de 119 GB). Primeira versão criava a partição no buraco antes do boot; corrigido com início explícito
- [ ] A `ROMS` aparece no PC (Linux, e se possível Windows) com as pastas
- [ ] Regravar a imagem preserva a `ROMS` e o conteúdo
- [x] Chaves SSH iguais entre boots
- [ ] Desligamento sujo: `fsck` corrige no boot seguinte

## Formatos de jogos

| Sistema | Formatos | Observação |
|---|---|---|
| Cartuchos (GB, GBA, NES, SNES, MD...) | ROM solta ou `.zip` | O `pingo-retro` descompacta o `.zip` na memória (fatia 3) |
| Arcade (FBNeo) | `.zip` (romset) | Nunca extrair: o romset é o próprio `.zip` |
| PlayStation (PCSX ReARMed) | `.chd` (recomendado), `.cue`+`.bin`, `.m3u`, `.pbp` | Vários discos: um item na lista via `.m3u` |
| PC Engine CD (Beetle PCE Fast) | `.chd` (recomendado), `.cue`+`.bin`/`.wav`/`.ogg`, `.ccd` | |
| `.zip` de CD, `.7z` | Não suportados por enquanto | Para CD, o compactado é o `.chd` (`chdman` no PC) |

## Tela de boot

- `board/h700/splash/pingo.png` (640x480) vira `/usr/share/pingo/splash.raw` no build (`scripts/png2fb.py`, XRGB8888) e o `S00pingo-splash` copia para o `/dev/fb0` no começo do boot.
- Kernel silencioso (`loglevel=3 logo.nologo vt.global_cursor_default=0`); `/dev/console` passa a ser a serial (`ttyS0`), então as mensagens do init e o login não escrevem por cima do logo. Diagnóstico: `pingo-log.txt`, `dmesg`, SSH e serial USB.
