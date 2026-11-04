# BIOS

Alguns consoles precisam do arquivo de BIOS do aparelho original. O Pingo OS não traz nenhuma BIOS: use as dos seus próprios aparelhos.

Os arquivos vão **direto na pasta `Bios/`** do cartão (partição `ROMS`), com o nome **exatamente** como na tabela (maiúsculas e minúsculas contam). O MD5 serve para conferir se a cópia é a certa: no PC, `md5sum arquivo` (Linux/macOS) ou `certutil -hashfile arquivo MD5` (Windows).

## Obrigatórias

| Console | Arquivo | MD5 | Observação |
|---|---|---|---|
| PlayStation | `scph5501.bin` | `490f666e1afb15b7362b406ed1cea246` | América. Basta uma das três |
| PlayStation | `scph5500.bin` | `8dd7d5296a650fac7319bce665a6a53c` | Japão |
| PlayStation | `scph5502.bin` | `32736f17079d0b2b7024407c39bd3050` | Europa |
| Sega CD | `bios_CD_U.bin` | `2efd74e3232ff260e371b99f84024f7f` | América |
| Sega CD | `bios_CD_E.bin` | `e66fa1dc5820d254611fdcdba0662372` | Europa |
| Sega CD | `bios_CD_J.bin` | `278a9397d192149e84e820ac621a8edd` | Japão |
| PC Engine CD | `syscard3.pce` | `38179df8f4ac870017db21ebcbf53114` | System Card 3.0 |
| Atari Lynx | `lynxboot.img` | `fcd403db69f54290b51035d82f835e7b` | |
| MSX | pastas `Machines/` e `Databases/` | — | Do pacote de BIOS do blueMSX; as duas pastas dentro de `Bios/` |
| Famicom Disk System | `disksys.rom` | `ca30b50f880eb660a320674ed365ef7a` | Só para jogos `.fds` |

No PlayStation, sem BIOS o core usa uma BIOS simulada: a maioria dos jogos roda, mas alguns travam ou não salvam no memory card. Com a BIOS real tudo funciona.

No Sega CD, cada região de jogo precisa da BIOS da mesma região.

## Opcionais

| Console | Arquivo | MD5 | O que muda |
|---|---|---|---|
| Game Boy Advance | `gba_bios.bin` | `a860e8c0b6d573d191e4ec7db1b1e4f6` | Mostra o logo de abertura; alguns jogos ficam mais fiéis |
| Pokémon mini | `bios.min` | `1e4fb124a3a886865acb574f388c803d` | Abertura original |
| Atari 7800 | `7800 BIOS (U).rom` | `0763f1ffb006ddbe32e52d497ee848ae` | Abertura original |

## Não precisam de BIOS

Game Boy, Game Boy Color, NES, Super Nintendo, Virtual Boy, Master System, Game Gear, Mega Drive, 32X, PC Engine (HuCard), Neo Geo Pocket, WonderSwan e Atari 2600.

Arcade (FBNeo): os jogos de Neo Geo precisam do `neogeo.zip` (fica na pasta `Roms/ARCADE`, junto com os jogos). Detalhes na fatia do arcade.
