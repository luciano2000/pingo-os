# Fase 2: polimento

## Wi-Fi

- **Tela:** MENU no Início → **Wi-Fi**. Liga/desliga (lembrado entre boots), estado ("Conectado a REDE · IP"), lista das redes por perto com o sinal em 4 barras e as marcas "conectada", "salva" e "aberta". A conecta, X procura de novo, Y esquece uma rede salva, B volta.
- **Senha:** teclado na tela (minúsculas, MAIÚSC com Y, símbolos com SELECT, X espaço, B apaga, START confirma, MENU cancela). Mínimo de 8 caracteres. Redes WEP são recusadas.
- **Backend:** `/usr/sbin/pingo-wifi` (`start|stop|on|off|status|scan|connect|forget`) sobre o `wpa_supplicant` pelo socket de controle (`wpa_cli`), sem NetworkManager. O launcher passa a senha pela entrada padrão do script, nunca pela linha de comando.
- **Nada da senha em texto:** o script converte com `wpa_passphrase` e grava só a chave (psk de 64 dígitos) em `/mnt/sd1/.system/wifi/wpa_supplicant.conf`; o nome da rede vai em hexadecimal. A partição de boot (que qualquer PC abre) não guarda nada de Wi-Fi.
- **Conexão que falha** (senha errada, rede fora do alcance): depois de ~20 s a rede nova é descartada e as salvas voltam a valer, sem perder a conexão anterior.
- **`pingo-wifi.txt`** (Fase 0): saiu da imagem. Se alguém puser um na `PINGOBOOT` pelo PC, o boot importa a rede (já como chave) e apaga o arquivo.
- `S41pingo-wifi` só chama `pingo-wifi start` em segundo plano (inclui o religamento do driver do RTL8821CS e o acerto do relógio por NTP).

### Checklist

- [x] Backend no aparelho: importa o `pingo-wifi.txt` e apaga, conecta, lista redes, desliga/liga, senha errada volta para a rede salva
- [x] Tela: ligar/desligar (corrigido o travamento ao religar), estado e lista de redes
- [ ] Conectar numa rede nova com o teclado na tela; esquecer uma rede salva
