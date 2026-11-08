/*
 * Parser JSON em árvore. Nós e strings decodificadas ficam na arena; os
 * filhos de objetos e arrays formam uma lista encadeada.
 */
#ifndef CB_JSON_H
#define CB_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

typedef enum {
    CB_JSON_NULL,
    CB_JSON_BOOL,
    CB_JSON_NUMBER,
    CB_JSON_STRING,
    CB_JSON_ARRAY,
    CB_JSON_OBJECT
} cb_json_type;

typedef struct cb_json {
    cb_json_type type;
    int boolean;
    double number;
    char *string;            /* decodificada, terminada em NUL */
    const char *key;         /* chave quando é membro de objeto */
    struct cb_json *first;   /* primeiro filho (arrays e objetos) */
    struct cb_json *last;
    struct cb_json *next;    /* irmão seguinte */
    size_t count;
} cb_json;

/* Retorna NULL em erro e preenche err (se não for NULL). */
cb_json *cb_json_parse(cb_arena *a, const char *src, size_t len,
                       char *err, size_t errlen);

/* Busca membro de objeto pela chave; NULL se ausente ou não for objeto. */
const cb_json *cb_json_get(const cb_json *obj, const char *key);

/* Valor string ou NULL se o tipo não for string. */
const char *cb_json_str(const cb_json *v);

/* Aceita número ou string numérica (YouTube envia contentLength como string). */
int64_t cb_json_i64(const cb_json *v, int64_t fallback);

/* Número como double, ou fallback. */
double cb_json_num(const cb_json *v, double fallback);

#endif
