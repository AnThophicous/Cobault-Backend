# Créditos e referências

O código do Cobault foi escrito do zero, em C.

## yt-dlp

Durante o desenvolvimento, o yt-dlp (licença The Unlicense, domínio público)
foi consultado como referência de fatos técnicos observáveis:

- valores de contexto dos clientes da API interna do YouTube (nomes de
  cliente, versões e identificadores de cabeçalho), usados em
  `src/extract/youtube.c`;
- a possibilidade de pedir a faixa de bytes pelo parâmetro da query em alguns
  servidores, e o tamanho de bloco de 10 MiB usado pelo yt-dlp como referência.

Esses dados são fatos de protocolo. O yt-dlp também é a ferramenta de
comparação nas medições do README.

## Dependências em tempo de execução

- libcurl (licença curl, de estilo MIT)
- OpenSSL/libcrypto (Apache-2.0)
- ffmpeg, opcional, chamado como programa externo para juntar vídeo e áudio
  (LGPL ou GPL, conforme a compilação instalada)

Não há dependência de Python nem de yt-dlp em tempo de execução.

## Ferramentas usadas só nas medições

aria2c (GPL-2.0), yt-dlp e ffmpeg. Elas não fazem parte do repositório.
