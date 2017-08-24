/**
 * @file common.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief common libyang routines implementations
 *
 * Copyright (c) 2015 - 2017 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "tree_internal.h"
#include "xpath.h"
#include "context.h"
#include "libyang.h"

THREAD_LOCAL struct ly_err ly_err_main;

API LY_ERR *
ly_errno_address(void)
{
    return &ly_err_main.no;
}

API LY_VECODE *
ly_vecode_address(void)
{
    return &ly_err_main.code;
}

API const char *
ly_errmsg(void)
{
    return ly_err_main.msg;
}

API const char *
ly_errpath(void)
{
    return &ly_err_main.path[ly_err_main.path_index];
}

API const char *
ly_errapptag(void)
{
    return ly_err_main.apptag;
}

void
ly_err_free(void *ptr)
{
    struct ly_err_item *i, *next;

    /* clean the error list */
    for (i = (struct ly_err_item *)ptr; i; i = next) {
        next = i->next;
        free(i->msg);
        free(i->path);
        free(i);
    }
}

void
ly_err_clean(struct ly_ctx *ctx, int with_errno)
{
    struct ly_err_item *i;

    i = pthread_getspecific(ctx->errlist_key);
    pthread_setspecific(ctx->errlist_key, NULL);
    ly_err_free(i);

    if (i && with_errno) {
        i->no = LY_SUCCESS;
        i->code = LYVE_SUCCESS;
    }
}

char *
ly_buf(void)
{
    return ly_err_main.buf;
}

#ifndef  __USE_GNU

char *
get_current_dir_name(void)
{
    char tmp[PATH_MAX];
    char *retval;

    if (getcwd(tmp, sizeof(tmp))) {
        retval = strdup(tmp);
        LY_CHECK_ERR_RETURN(!retval, LOGMEM, NULL);
        return retval;
    }
    return NULL;
}

#endif

const char *
strpbrk_backwards(const char *s, const char *accept, unsigned int s_len)
{
    const char *sc;

    for (; *s != '\0' && s_len; --s, --s_len) {
        for (sc = accept; *sc != '\0'; ++sc) {
            if (*s == *sc) {
                return s;
            }
        }
    }
    return s;
}

char *
strnchr(const char *s, int c, unsigned int len)
{
    for (; *s != (char)c; ++s, --len) {
        if ((*s == '\0') || (!len)) {
            return NULL;
        }
    }
    return (char *)s;
}

const char *
strnodetype(LYS_NODE type)
{
    switch (type) {
    case LYS_UNKNOWN:
        return NULL;
    case LYS_AUGMENT:
        return "augment";
    case LYS_CONTAINER:
        return "container";
    case LYS_CHOICE:
        return "choice";
    case LYS_LEAF:
        return "leaf";
    case LYS_LEAFLIST:
        return "leaf-list";
    case LYS_LIST:
        return "list";
    case LYS_ANYXML:
        return "anyxml";
    case LYS_GROUPING:
        return "grouping";
    case LYS_CASE:
        return "case";
    case LYS_INPUT:
        return "input";
    case LYS_OUTPUT:
        return "output";
    case LYS_NOTIF:
        return "notification";
    case LYS_RPC:
        return "rpc";
    case LYS_USES:
        return "uses";
    case LYS_ACTION:
        return "action";
    case LYS_ANYDATA:
        return "anydata";
    case LYS_EXT:
        return "extension instance";
    }

    return NULL;
}

const char *
transform_module_name2import_prefix(const struct lys_module *module, const char *module_name)
{
    uint16_t i;

    if (!module_name) {
        return NULL;
    }

    if (!strcmp(lys_main_module(module)->name, module_name)) {
        /* the same for module and submodule */
        return module->prefix;
    }

    for (i = 0; i < module->imp_size; ++i) {
        if (!strcmp(module->imp[i].module->name, module_name)) {
            return module->imp[i].prefix;
        }
    }

    return NULL;
}

static const char *
_transform_json2xml(const struct lys_module *module, const char *expr, int schema, int inst_id, const char ***prefixes,
                    const char ***namespaces, uint32_t *ns_count)
{
    const char *cur_expr, *end, *prefix, *ptr;
    char *out, *name;
    size_t out_size, out_used, name_len;
    const struct lys_module *mod = NULL, *prev_mod = NULL;
    uint32_t i, j;
    struct lyxp_expr *exp;

    assert(module && expr && ((!prefixes && !namespaces && !ns_count) || (prefixes && namespaces && ns_count)));

    if (ns_count) {
        *ns_count = 0;
        *prefixes = NULL;
        *namespaces = NULL;
    }

    if (!expr[0]) {
        /* empty value */
        return lydict_insert(module->ctx, expr, 0);
    }

    out_size = strlen(expr) + 1;
    out = malloc(out_size);
    LY_CHECK_ERR_RETURN(!out, LOGMEM(module->ctx), NULL);
    out_used = 0;

    exp = lyxp_parse_expr(expr);
    LY_CHECK_ERR_RETURN(!exp, free(out), NULL);

    for (i = 0; i < exp->used; ++i) {
        cur_expr = &exp->expr[exp->expr_pos[i]];

        /* copy WS */
        if (i && ((end = exp->expr + exp->expr_pos[i - 1] + exp->tok_len[i - 1]) != cur_expr)) {
            strncpy(&out[out_used], end, cur_expr - end);
            out_used += cur_expr - end;
        }

        if ((exp->tokens[i] == LYXP_TOKEN_NAMETEST) && ((end = strnchr(cur_expr, ':', exp->tok_len[i])) || inst_id)) {
            /* get the module */
            if (!schema) {
                if (end) {
                    name_len = end - cur_expr;
                    name = strndup(cur_expr, name_len);
                    mod = ly_ctx_get_module(module->ctx, name, NULL);
                    free(name);
                    if (!mod) {
                        LOGVAL(module->ctx, LYE_INMOD_LEN, LY_VLOG_NONE, NULL, name_len, cur_expr);
                        goto error;
                    }
                    prev_mod = mod;
                } else {
                    mod = prev_mod;
                    if (!mod) {
                        LOGINT(module->ctx);
                        goto error;
                    }
                    name_len = 0;
                    end = cur_expr;
                }
                prefix = mod->prefix;
            } else {
                if (end) {
                    name_len = end - cur_expr;
                } else {
                    name_len = strlen(cur_expr);
                    end = cur_expr;
                }
                name = strndup(cur_expr, name_len);
                prefix = transform_module_name2import_prefix(module, name);
                free(name);
                if (!prefix) {
                    LOGVAL(module->ctx, LYE_INMOD_LEN, LY_VLOG_NONE, NULL, name_len, cur_expr);
                    goto error;
                }
            }

            /* remember the namespace definition (only if it's new) */
            if (!schema && ns_count) {
                for (j = 0; j < *ns_count; ++j) {
                    if (ly_strequal((*namespaces)[j], mod->ns, 1)) {
                        break;
                    }
                }
                if (j == *ns_count) {
                    ++(*ns_count);
                    *prefixes = ly_realloc(*prefixes, *ns_count * sizeof **prefixes);
                    LY_CHECK_ERR_GOTO(!(*prefixes), LOGMEM(module->ctx), error);
                    *namespaces = ly_realloc(*namespaces, *ns_count * sizeof **namespaces);
                    LY_CHECK_ERR_GOTO(!(*namespaces), LOGMEM(module->ctx), error);
                    (*prefixes)[*ns_count - 1] = mod->prefix;
                    (*namespaces)[*ns_count - 1] = mod->ns;
                }
            }

            /* adjust out size (it can even decrease in some strange cases) */
            out_size += strlen(prefix) + 1 - name_len;
            out = ly_realloc(out, out_size);
            LY_CHECK_ERR_GOTO(!out, LOGMEM(module->ctx), error);

            /* copy the model name */
            strcpy(&out[out_used], prefix);
            out_used += strlen(prefix);

            if (!name_len) {
                /* we are adding the prefix, so also ':' */
                out[out_used] = ':';
                ++out_used;
            }

            /* copy the rest */
            strncpy(&out[out_used], end, exp->tok_len[i] - name_len);
            out_used += exp->tok_len[i] - name_len;
        } else if ((exp->tokens[i] == LYXP_TOKEN_LITERAL) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            ptr = end;
            while (isalnum(ptr[-1]) || (ptr[-1] == '_') || (ptr[-1] == '-') || (ptr[-1] == '.')) {
                --ptr;
            }

            /* get the module */
            name_len = end - ptr;
            if (!schema) {
                prefix = NULL;
                name = strndup(ptr, name_len);
                mod = ly_ctx_get_module(module->ctx, name, NULL);
                free(name);
                if (mod) {
                    prefix = mod->prefix;
                }
            } else {
                name = strndup(ptr, name_len);
                prefix = transform_module_name2import_prefix(module, name);
                free(name);
            }

            if (prefix) {
                /* remember the namespace definition (only if it's new) */
                if (!schema && ns_count) {
                    for (j = 0; j < *ns_count; ++j) {
                        if (ly_strequal((*namespaces)[j], mod->ns, 1)) {
                            break;
                        }
                    }
                    if (j == *ns_count) {
                        ++(*ns_count);
                        *prefixes = ly_realloc(*prefixes, *ns_count * sizeof **prefixes);
                        LY_CHECK_ERR_GOTO(!(*prefixes), LOGMEM(module->ctx), error);
                        *namespaces = ly_realloc(*namespaces, *ns_count * sizeof **namespaces);
                        LY_CHECK_ERR_GOTO(!(*namespaces), LOGMEM(module->ctx), error);
                        (*prefixes)[*ns_count - 1] = mod->prefix;
                        (*namespaces)[*ns_count - 1] = mod->ns;
                    }
                }

                /* adjust out size (it can even decrease in some strange cases) */
                out_size += strlen(prefix) - name_len;
                out = ly_realloc(out, out_size);
                LY_CHECK_ERR_GOTO(!out, LOGMEM(module->ctx), error);

                /* copy any beginning */
                strncpy(&out[out_used], cur_expr, ptr - cur_expr);
                out_used += ptr - cur_expr;

                /* copy the model name */
                strcpy(&out[out_used], prefix);
                out_used += strlen(prefix);

                /* copy the rest */
                strncpy(&out[out_used], end, (exp->tok_len[i] - name_len) - (ptr - cur_expr));
                out_used += (exp->tok_len[i] - name_len) - (ptr - cur_expr);
            } else {
                strncpy(&out[out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
                out_used += exp->tok_len[i];
            }
        } else {
            strncpy(&out[out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
            out_used += exp->tok_len[i];
        }
    }
    out[out_used] = '\0';

    lyxp_expr_free(exp);
    return lydict_insert_zc(module->ctx, out);

error:
    if (!schema && ns_count) {
        free(*prefixes);
        free(*namespaces);
    }
    free(out);
    lyxp_expr_free(exp);
    return NULL;
}

const char *
transform_json2xml(const struct lys_module *module, const char *expr, int inst_id, const char ***prefixes,
                   const char ***namespaces, uint32_t *ns_count)
{
    return _transform_json2xml(module, expr, 0, inst_id, prefixes, namespaces, ns_count);
}

const char *
transform_json2schema(const struct lys_module *module, const char *expr)
{
    return _transform_json2xml(module, expr, 1, 0, NULL, NULL, NULL);
}

const char *
transform_xml2json(struct ly_ctx *ctx, const char *expr, struct lyxml_elem *xml, int inst_id, int use_ctx_data_clb, int log)
{
    const char *end, *cur_expr, *ptr;
    char *out, *prefix;
    uint16_t i;
    size_t out_size, out_used, pref_len;
    const struct lys_module *mod, *prev_mod = NULL;
    const struct lyxml_ns *ns;
    struct lyxp_expr *exp;

    out_size = strlen(expr) + 1;
    out = malloc(out_size);
    if (!out) {
        if (log) {
            LOGMEM(ctx);
        }
        return NULL;
    }
    out_used = 0;

    exp = lyxp_parse_expr(expr);
    if (!exp) {
        free(out);
        return NULL;
    }

    for (i = 0; i < exp->used; ++i) {
        cur_expr = &exp->expr[exp->expr_pos[i]];

        /* copy WS */
        if (i && ((end = exp->expr + exp->expr_pos[i - 1] + exp->tok_len[i - 1]) != cur_expr)) {
            strncpy(&out[out_used], end, cur_expr - end);
            out_used += cur_expr - end;
        }

        if ((exp->tokens[i] == LYXP_TOKEN_NAMETEST) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            /* get the module */
            pref_len = end - cur_expr;
            prefix = strndup(cur_expr, pref_len);
            if (!prefix) {
                if (log) {
                    LOGMEM(ctx);
                }
                goto error;
            }
            ns = lyxml_get_ns(xml, prefix);
            free(prefix);
            if (!ns) {
                if (log) {
                    LOGVAL(ctx, LYE_XML_INVAL, LY_VLOG_XML, xml, "namespace prefix");
                    LOGVAL(ctx, LYE_SPEC, LY_VLOG_PREV, NULL,
                        "XML namespace with prefix \"%.*s\" not defined.", pref_len, cur_expr);
                }
                goto error;
            }
            mod = ly_ctx_get_module_by_ns(ctx, ns->value, NULL);
            if (use_ctx_data_clb && ctx->data_clb) {
                if (!mod) {
                    mod = ctx->data_clb(ctx, NULL, ns->value, 0, ctx->data_clb_data);
                } else if (!mod->implemented) {
                    mod = ctx->data_clb(ctx, mod->name, mod->ns, LY_MODCLB_NOT_IMPLEMENTED, ctx->data_clb_data);
                }
            }
            if (!mod) {
                if (log) {
                    LOGVAL(ctx, LYE_XML_INVAL, LY_VLOG_XML, xml, "module namespace");
                    LOGVAL(ctx, LYE_SPEC, LY_VLOG_PREV, NULL,
                        "Module with the namespace \"%s\" could not be found.", ns->value);
                }
                goto error;
            }

            if (!inst_id || (mod != prev_mod)) {
                /* adjust out size (it can even decrease in some strange cases) */
                out_size += strlen(mod->name) - pref_len;
                out = ly_realloc(out, out_size);
                if (!out) {
                    if (log) {
                        LOGMEM(ctx);
                    }
                    goto error;
                }

                /* copy the model name */
                strcpy(&out[out_used], mod->name);
                out_used += strlen(mod->name);
            } else {
                /* skip ':' */
                ++end;
                ++pref_len;
            }

            /* remember previous model name */
            prev_mod = mod;

            /* copy the rest */
            strncpy(&out[out_used], end, exp->tok_len[i] - pref_len);
            out_used += exp->tok_len[i] - pref_len;
        } else if ((exp->tokens[i] == LYXP_TOKEN_NAMETEST) && inst_id) {
            if (log) {
                LOGVAL(ctx, LYE_XML_INVAL, LY_VLOG_XML, xml, "namespace prefix");
                LOGVAL(ctx, LYE_SPEC, LY_VLOG_PREV, NULL, "Node name is missing module prefix.");
            }
            goto error;
        } else if ((exp->tokens[i] == LYXP_TOKEN_LITERAL) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            ptr = end;
            while (isalnum(ptr[-1]) || (ptr[-1] == '_') || (ptr[-1] == '-') || (ptr[-1] == '.')) {
                --ptr;
            }

            /* get the module */
            pref_len = end - cur_expr;
            prefix = strndup(cur_expr, pref_len);
            if (!prefix) {
                if (log) {
                    LOGMEM(ctx);
                }
                goto error;
            }
            ns = lyxml_get_ns(xml, prefix);
            free(prefix);
            if (!ns) {
                if (log) {
                    LOGVAL(ctx, LYE_XML_INVAL, LY_VLOG_XML, xml, "namespace prefix");
                    LOGVAL(ctx, LYE_SPEC, LY_VLOG_PREV, NULL,
                        "XML namespace with prefix \"%.*s\" not defined.", pref_len, cur_expr);
                }
                goto error;
            }
            mod = ly_ctx_get_module_by_ns(ctx, ns->value, NULL);
            if (mod) {
                /* adjust out size (it can even decrease in some strange cases) */
                out_size += strlen(mod->name) - pref_len;
                out = ly_realloc(out, out_size);
                if (!out) {
                    if (log) {
                        LOGMEM(ctx);
                    }
                    goto error;
                }

                /* copy any beginning */
                strncpy(&out[out_used], cur_expr, ptr - cur_expr);
                out_used += ptr - cur_expr;

                /* copy the model name */
                strcpy(&out[out_used], mod->name);
                out_used += strlen(mod->name);

                /* copy the rest */
                strncpy(&out[out_used], end, (exp->tok_len[i] - pref_len) - (ptr - cur_expr));
                out_used += (exp->tok_len[i] - pref_len) - (ptr - cur_expr);
            } else {
                strncpy(&out[out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
                out_used += exp->tok_len[i];
            }
        } else {
            strncpy(&out[out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
            out_used += exp->tok_len[i];
        }
    }
    out[out_used] = '\0';

    lyxp_expr_free(exp);
    return lydict_insert_zc(ctx, out);

error:
    free(out);
    lyxp_expr_free(exp);
    return NULL;
}

const char *
transform_schema2json(const struct lys_module *module, const char *expr)
{
    const char *end, *cur_expr, *ptr;
    char *out;
    uint16_t i;
    size_t out_size, out_used, pref_len;
    const struct lys_module *mod;
    struct lyxp_expr *exp = NULL;

    out_size = strlen(expr) + 1;
    out = malloc(out_size);
    LY_CHECK_ERR_RETURN(!out, LOGMEM(module->ctx), NULL);
    out_used = 0;

    exp = lyxp_parse_expr(expr);
    LY_CHECK_ERR_GOTO(!exp, , error);

    for (i = 0; i < exp->used; ++i) {
        cur_expr = &exp->expr[exp->expr_pos[i]];

        /* copy WS */
        if (i && ((end = exp->expr + exp->expr_pos[i - 1] + exp->tok_len[i - 1]) != cur_expr)) {
            strncpy(&out[out_used], end, cur_expr - end);
            out_used += cur_expr - end;
        }

        if ((exp->tokens[i] == LYXP_TOKEN_NAMETEST) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            /* get the module */
            pref_len = end - cur_expr;
            mod = lys_get_import_module(module, cur_expr, pref_len, NULL, 0);
            if (!mod) {
                LOGVAL(module->ctx, LYE_INMOD_LEN, LY_VLOG_NONE, NULL, pref_len, cur_expr);
                goto error;
            }

            /* adjust out size (it can even decrease in some strange cases) */
            out_size += strlen(mod->name) - pref_len;
            out = ly_realloc(out, out_size);
            LY_CHECK_ERR_GOTO(!out, LOGMEM(module->ctx), error);

            /* copy the model name */
            strcpy(&out[out_used], mod->name);
            out_used += strlen(mod->name);

            /* copy the rest */
            strncpy(&out[out_used], end, exp->tok_len[i] - pref_len);
            out_used += exp->tok_len[i] - pref_len;
        } else if ((exp->tokens[i] == LYXP_TOKEN_LITERAL) && (end = strnchr(cur_expr, ':', exp->tok_len[i]))) {
            ptr = end;
            while (isalnum(ptr[-1]) || (ptr[-1] == '_') || (ptr[-1] == '-') || (ptr[-1] == '.')) {
                --ptr;
            }

            /* get the module */
            pref_len = end - ptr;
            mod = lys_get_import_module(module, ptr, pref_len, NULL, 0);
            if (mod) {
                /* adjust out size (it can even decrease in some strange cases) */
                out_size += strlen(mod->name) - pref_len;
                out = ly_realloc(out, out_size);
                LY_CHECK_ERR_GOTO(!out, LOGMEM(module->ctx), error);

                /* copy any beginning */
                strncpy(&out[out_used], cur_expr, ptr - cur_expr);
                out_used += ptr - cur_expr;

                /* copy the model name */
                strcpy(&out[out_used], mod->name);
                out_used += strlen(mod->name);

                /* copy the rest */
                strncpy(&out[out_used], end, (exp->tok_len[i] - pref_len) - (ptr - cur_expr));
                out_used += (exp->tok_len[i] - pref_len) - (ptr - cur_expr);
            } else {
                strncpy(&out[out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
                out_used += exp->tok_len[i];
            }
        } else {
            strncpy(&out[out_used], &exp->expr[exp->expr_pos[i]], exp->tok_len[i]);
            out_used += exp->tok_len[i];
        }
    }
    out[out_used] = '\0';

    lyxp_expr_free(exp);
    return lydict_insert_zc(module->ctx, out);

error:
    free(out);
    lyxp_expr_free(exp);
    return NULL;
}

const char *
transform_iffeat_schema2json(const struct lys_module *module, const char *expr)
{
    const char *in, *id;
    char *out, *col;
    size_t out_size, out_used, id_len, rc;
    const struct lys_module *mod;

    in = expr;
    out_size = strlen(in) + 1;
    out = malloc(out_size);
    LY_CHECK_ERR_RETURN(!out, LOGMEM(module->ctx), NULL);
    out_used = 0;

    while (1) {
        col = strchr(in, ':');
        /* we're finished, copy the remaining part */
        if (!col) {
            strcpy(&out[out_used], in);
            out_used += strlen(in) + 1;
            assert(out_size == out_used);
            return lydict_insert_zc(module->ctx, out);
        }
        id = strpbrk_backwards(col - 1, "/ [\'\"", (col - in) - 1);
        if ((id[0] == '/') || (id[0] == ' ') || (id[0] == '[') || (id[0] == '\'') || (id[0] == '\"')) {
            ++id;
        }
        id_len = col - id;
        rc = parse_identifier(id);
        if (rc < id_len) {
            LOGVAL(module->ctx, LYE_INCHAR, LY_VLOG_NONE, NULL, id[rc], &id[rc]);
            free(out);
            return NULL;
        }

        /* get the module */
        mod = lys_get_import_module(module, id, id_len, NULL, 0);
        if (!mod) {
            LOGVAL(module->ctx, LYE_INMOD_LEN, LY_VLOG_NONE, NULL, id_len, id);
            free(out);
            return NULL;
        }

        /* adjust out size (it can even decrease in some strange cases) */
        out_size += strlen(mod->name) - id_len;
        out = ly_realloc(out, out_size);
        LY_CHECK_ERR_RETURN(!out, LOGMEM(module->ctx), NULL);

        /* copy the data before prefix */
        strncpy(&out[out_used], in, id - in);
        out_used += id - in;

        /* copy the model name */
        strcpy(&out[out_used], mod->name);
        out_used += strlen(mod->name);

        /* copy ':' */
        out[out_used] = ':';
        ++out_used;

        /* finally adjust in pointer for next round */
        in = col + 1;
    }

    /* unreachable */
    LOGINT(module->ctx);
    return NULL;
}

static int
transform_json2xpath_subexpr(const struct lys_module *cur_module, const struct lys_module *prev_mod, struct lyxp_expr *exp,
                             uint32_t *i, enum lyxp_token end_token, char **out, size_t *out_used, size_t *out_size)
{
    const char *cur_expr, *end, *ptr;
    size_t name_len;
    char *name;
    const struct lys_module *mod;
    struct ly_ctx *ctx = cur_module->ctx;

    while (*i < exp->used) {
        if (exp->tokens[*i] == end_token) {
            return 0;
        }

        cur_expr = &exp->expr[exp->expr_pos[*i]];

        /* copy WS */
        if (*i && ((end = exp->expr + exp->expr_pos[*i - 1] + exp->tok_len[*i - 1]) != cur_expr)) {
            strncpy(*out + *out_used, end, cur_expr - end);
            *out_used += cur_expr - end;
        }

        if (exp->tokens[*i] == LYXP_TOKEN_BRACK1) {
            /* copy "[" */
            strncpy(*out + *out_used, &exp->expr[exp->expr_pos[*i]], exp->tok_len[*i]);
            *out_used += exp->tok_len[*i];
            ++(*i);

            /* call recursively because we need to remember current prev_mod for after the predicate */
            if (transform_json2xpath_subexpr(cur_module, prev_mod, exp, i, LYXP_TOKEN_BRACK2, out, out_used, out_size)) {
                return -1;
            }

            /* copy "]" */
            strncpy(*out + *out_used, &exp->expr[exp->expr_pos[*i]], exp->tok_len[*i]);
            *out_used += exp->tok_len[*i];
        } else if (exp->tokens[*i] == LYXP_TOKEN_NAMETEST) {
            if ((end = strnchr(cur_expr, ':', exp->tok_len[*i]))) {
                /* there is a prefix, get the module */
                name_len = end - cur_expr;
                name = strndup(cur_expr, name_len);
                prev_mod = ly_ctx_get_module(cur_module->ctx, name, NULL);
                free(name);
                if (!prev_mod) {
                    LOGVAL(ctx, LYE_INMOD_LEN, LY_VLOG_NONE, NULL, name_len ? name_len : exp->tok_len[*i], cur_expr);
                    return -1;
                }
                /* skip ":" */
                ++end;
                ++name_len;
            } else {
                end = cur_expr;
                name_len = 0;
            }

            /* do we print the module name? */
            if (prev_mod != cur_module) {
                /* adjust out size (it can even decrease in some strange cases) */
                *out_size += (strlen(prev_mod->name) - name_len) + 1;
                *out = ly_realloc(*out, *out_size);
                LY_CHECK_ERR_RETURN(!*out, LOGMEM(ctx), -1);

                /* copy the model name */
                strcpy(*out + *out_used, prev_mod->name);
                *out_used += strlen(prev_mod->name);

                /* print ":" */
                (*out)[*out_used] = ':';
                ++(*out_used);
            }

            /* copy the rest */
            strncpy(*out + *out_used, end, exp->tok_len[*i] - name_len);
            *out_used += exp->tok_len[*i] - name_len;
        } else if ((exp->tokens[*i] == LYXP_TOKEN_LITERAL) && (end = strnchr(cur_expr, ':', exp->tok_len[*i]))) {
            ptr = end;
            while (isalnum(ptr[-1]) || (ptr[-1] == '_') || (ptr[-1] == '-') || (ptr[-1] == '.')) {
                --ptr;
            }

            /* get the module, but it may actually not be a module name */
            name_len = end - ptr;
            name = strndup(ptr, name_len);
            mod = ly_ctx_get_module(cur_module->ctx, name, NULL);
            free(name);

            if (mod && (mod != cur_module)) {
                /* adjust out size (it can even decrease in some strange cases) */
                *out_size += strlen(mod->name) - name_len;
                *out = ly_realloc(*out, *out_size);
                LY_CHECK_ERR_RETURN(!*out, LOGMEM(ctx), -1);

                /* copy any beginning */
                strncpy(*out + *out_used, cur_expr, ptr - cur_expr);
                *out_used += ptr - cur_expr;

                /* copy the model name */
                strcpy(*out + *out_used, mod->name);
                *out_used += strlen(mod->name);

                /* copy the rest */
                strncpy(*out + *out_used, end, (exp->tok_len[*i] - name_len) - (ptr - cur_expr));
                *out_used += (exp->tok_len[*i] - name_len) - (ptr - cur_expr);
            } else {
                strncpy(*out + *out_used, &exp->expr[exp->expr_pos[*i]], exp->tok_len[*i]);
                *out_used += exp->tok_len[*i];
            }
        } else {
            strncpy(*out + *out_used, &exp->expr[exp->expr_pos[*i]], exp->tok_len[*i]);
            *out_used += exp->tok_len[*i];
        }

        ++(*i);
    }

    return 0;
}

char *
transform_json2xpath(const struct lys_module *cur_module, const char *expr)
{
    char *out;
    size_t out_size, out_used;
    uint32_t i;
    struct lyxp_expr *exp;

    assert(cur_module && expr);

    out_size = strlen(expr) + 1;
    out = malloc(out_size);
    LY_CHECK_ERR_RETURN(!out, LOGMEM(cur_module->ctx), NULL);
    out_used = 0;

    exp = lyxp_parse_expr(expr);
    LY_CHECK_ERR_RETURN(!exp, free(out), NULL);

    i = 0;
    if (transform_json2xpath_subexpr(cur_module, cur_module, exp, &i, LYXP_TOKEN_NONE, &out, &out_used, &out_size)) {
        goto error;
    }
    out[out_used] = '\0';

    lyxp_expr_free(exp);
    return out;

error:
    free(out);
    lyxp_expr_free(exp);
    return NULL;
}

int
ly_new_node_validity(const struct lys_node *schema)
{
    int validity;

    validity = LYD_VAL_OK;
    switch (schema->nodetype) {
    case LYS_LEAF:
    case LYS_LEAFLIST:
        if (((struct lys_node_leaf *)schema)->type.base == LY_TYPE_LEAFREF) {
            validity |= LYD_VAL_LEAFREF;
        }
        validity |= LYD_VAL_MAND;
        break;
    case LYS_LIST:
        validity |= LYD_VAL_UNIQUE;
        /* fallthrough */
    case LYS_CONTAINER:
    case LYS_NOTIF:
    case LYS_RPC:
    case LYS_ACTION:
    case LYS_ANYXML:
    case LYS_ANYDATA:
        validity |= LYD_VAL_MAND;
        break;
    default:
        break;
    }

    return validity;
}

void *
ly_realloc(void *ptr, size_t size)
{
    void *new_mem;

    new_mem = realloc(ptr, size);
    if (!new_mem) {
        free(ptr);
    }

    return new_mem;
}

int
ly_strequal_(const char *s1, const char *s2)
{
    if (s1 == s2) {
        return 1;
    } else if (!s1 || !s2) {
        return 0;
    } else {
        for ( ; *s1 == *s2; s1++, s2++) {
            if (*s1 == '\0') {
                return 1;
            }
        }
        return 0;
    }
}

int64_t
dec_pow(uint8_t exp)
{
    int64_t ret = 1;
    uint8_t i;

    for (i = 0; i < exp; ++i) {
        ret *= 10;
    }

    return ret;
}

int
dec64cmp(int64_t num1, uint8_t dig1, int64_t num2, uint8_t dig2)
{
    if (dig1 < dig2) {
        num2 /= dec_pow(dig2 - dig1);
    } else if (dig1 > dig2) {
        num1 /= dec_pow(dig1 - dig2);
    }

    if (num1 == num2) {
        return 0;
    }
    return (num1 > num2 ? 1 : -1);
}
