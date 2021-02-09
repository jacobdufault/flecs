/**
 * @file expr.h
 * @brief Structure that stores rule expression
 */

#ifndef FLECS_EXPR_H
#define FLECS_EXPR_H

#include "api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EcsIn,
    EcsOut,
    EcsInOut
} ecs_term_rw_t;

typedef enum {
    EcsAnd,
    EcsOr,
    EcsNot,
    EcsOptional
} ecs_term_op_t;

typedef struct {
    ecs_entity_t entity;
    char *name;
} ecs_atom_t;

typedef struct {
    ecs_term_rw_t rw;
    ecs_term_op_t op;
    ecs_atom_t pred;
    int32_t arg_count;
    ecs_atom_t *args;    
} ecs_term_t;

typedef struct {
    int32_t term_count;
    ecs_term_t *terms;
} ecs_expr_t;

#ifdef __cplusplus
}
#endif

#endif
