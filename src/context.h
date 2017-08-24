/**
 * @file context.h
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief internal context structures and functions
 *
 * Copyright (c) 2015 - 2017 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LY_CONTEXT_H_
#define LY_CONTEXT_H_

#include <pthread.h>

#include "common.h"
#include "dict_private.h"
#include "tree_schema.h"
#include "libyang.h"

struct ly_modules_list {
    char **search_paths;
    int size;
    int used;
    struct lys_module **list;
    /* all (sub)modules that are currently being parsed */
    struct lys_module **parsing_sub_modules;
    /* all already parsed submodules of a module, which is before all its submodules (to mark submodule imports) */
    struct lys_module **parsed_submodules;
    uint8_t parsing_sub_modules_count;
    uint8_t parsed_submodules_count;
    uint16_t module_set_id;
    uint32_t flags;
};

#define LY_CTX_ALLIMPLEMENTED 0x01 /**< all modules are implemented despite they were loaded explicitly or implicitly
                                        via import statement */

struct ly_err_item {
    LY_ERR no;
    LY_VECODE code;
    char *msg;
    char *path;
    struct ly_err_item *next;
};

struct ly_ctx {
    struct dict_table dict;
    struct ly_modules_list models;
    ly_module_imp_clb imp_clb;
    void *imp_clb_data;
    ly_module_data_clb data_clb;
    void *data_clb_data;
    pthread_key_t errlist_key;
};

#endif /* LY_CONTEXT_H_ */
