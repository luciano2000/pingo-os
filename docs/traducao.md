# Traduzir o Pingo OS

A interface vem em português e troca de idioma em **MENU no Início → Idioma**, sem reiniciar. Cada idioma é um arquivo de texto simples.

## Fazer uma tradução

1. Copie o [`frontend/idiomas/en.txt`](../frontend/idiomas/en.txt) (inglês, completo) com o código do idioma no nome: `es.txt`, `fr.txt`, `it.txt`, `de.txt`...
2. Troque a linha `NOME=` pelo nome do idioma como ele aparece no menu (`NOME=Español`).
3. Traduza o texto depois do `=` em cada linha. **Não mude a chave** (a parte antes do `=`).
4. Teste no aparelho: copie o arquivo para a pasta `Idiomas/` do cartão (SD1, partição ROMS; crie a pasta se não existir) e escolha o idioma no menu. Não precisa gravar a imagem de novo.
5. Mande o arquivo pronto como pull request (em `frontend/idiomas/`) ou no [Discord do Retrogamer Brasil](https://discord.gg/S3SBnaKh58): ele entra na próxima versão.

## Regras

- **`%d`, `%s`, `%u`**: são os números e nomes que o sistema coloca no texto. Mantenha todos, **na mesma ordem**. Uma linha com eles trocados é ignorada (vale o português) e aparece no log (`.system/logs/ui.log`).
  - `NO_GAMES_ON=Sem jogos de %s no %s` → `NO_GAMES_ON=No %s games on %s` (primeiro o console, depois o cartão).
- Formatos de data (`SAVED_TODAY`, `SAVED_DATE`...) usam os códigos do `strftime`: `%H` hora, `%M` minuto, `%d` dia, `%m` mês, `%b` mês abreviado. Aqui a ordem pode mudar.
- Textos curtos (dicas do rodapé como `BACK=voltar`) devem continuar curtos: o rodapé tem pouco espaço.
- Linha que falta no arquivo fica em português. Linhas começando com `#` são comentários.
- `\n` no texto vira quebra de linha.
- Codificação UTF-8 (acentos funcionam).

## O que ainda não é traduzido

Nomes dos consoles, dos fusos horários e das opções dos emuladores (`frontend/cores/*.opts`). Os nomes dos dip switches do arcade só são traduzidos para o português; nos outros idiomas aparecem como vêm do FBNeo (em inglês).

## Onde ficam

- Imagem: `/usr/share/pingo/idiomas/<código>.txt`.
- Cartão: `Idiomas/<código>.txt` no SD1 (vale mais que o da imagem, para testar ou corrigir).
- Idioma escolhido: `Config/idioma.txt`.
