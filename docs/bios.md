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

## Arcade (FBNeo)

- Romset: **FinalBurn Neo 1.0.0.03 (Arcade)**, de preferência **non-merged** (cada `.zip` completo e independente). Split/merged também funcionam, com o `.zip` pai junto.
- Romsets de **MAME** não servem: os nomes coincidem, mas o conteúdo dos `.zip` muitas vezes não.
- Para conferir ou reconstruir a coleção (RomVault, ClrMamePro), use o DAT exato desta versão: [FinalBurn Neo (ClrMame Pro XML, Arcade only).dat](https://github.com/libretro/FBNeo/blob/72af9e23b766b6ad6d2a30042fc8d83eadd25b61/dats/FinalBurn%20Neo%20(ClrMame%20Pro%20XML,%20Arcade%20only).dat).
- BIOS de arcade ficam **na mesma pasta dos jogos** e não aparecem na lista: `neogeo.zip` (Neo Geo), `pgm.zip` (PGM), `skns.zip`, `decocass.zip` etc.
- **Neo Geo separado:** a pasta `Roms/NEOGEO` aparece como "Neo Geo" no Início, com o mesmo core (FBNeo). O `neogeo.zip` vai dentro dela, junto com os jogos. No arcade a pasta só organiza: o FBNeo reconhece o jogo pelo nome do `.zip`.
- O launcher mostra o nome do jogo vindo do DAT ("Street Fighter II: The World Warrior" em vez de `sf2.zip`).
- **Samples** (sons gravados de jogos antigos, como Donkey Kong, Galaga, Space Invaders): em `Bios/fbneo/samples/`, um `.zip` por jogo, com o nome do jogo e sem descompactar (`Bios/fbneo/samples/dkong.zip`). Sem eles o jogo roda, só faltam alguns sons.
- **Recordes**: o `hiscore.dat` diz ao FBNeo onde cada jogo guarda a tabela de recordes na memória (não é um arquivo de recordes). O Pingo OS já traz o do FBNeo (5857 jogos) e copia para `Bios/fbneo/hiscore.dat` no boot, se não houver um; um `hiscore.dat` mais novo posto ali é mantido. Os recordes de cada jogo ficam em `Saves/<console>/fbneo/<jogo>.hi` (gravados ao sair do jogo). Os jogos de Neo Geo guardam os recordes na memória da própria placa (`<jogo>.fs`, na mesma pasta).
- As pastas `Bios/fbneo/` e `Bios/fbneo/samples/` são criadas no boot.
- Quando o FBNeo for atualizado no Pingo OS, a versão do romset muda junto; ela fica registrada aqui.
