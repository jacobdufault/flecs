#include "private_api.h"

/* This is the implementation of the rule solver, which for a given rule 
 * expression returns all combinations of variable values that satisfy the
 * constraints of the rule.
 *
 * An expression is a list of terms. Each term describes a predicate with 0..N
 * arguments. Both the predicate and arguments can be variables. If a term does
 * not contain any variables it is a fact. Evaluating a fact will always return
 * either true or false.
 *
 * Terms with variables are conceptually evaluated against every possible value 
 * for those variables, and only sets of variable values that meet all 
 * constraints are yielded by the rule solver.
 */

#define ECS_RULE_MAX_VARIABLE_COUNT (256)

#define RULE_PAIR_PREDICATE (1)
#define RULE_PAIR_OBJECT (2)

/* A rule pair contains a predicate and object that can be stored in a register. */
typedef struct ecs_rule_pair_t {
    uint32_t pred;
    uint32_t obj;
    int8_t reg_mask; /* bit 1 = predicate, bit 2 = object, bit 4 = wildcard */
} ecs_rule_pair_t;

/* A rule register stores temporary values for rule variables */
typedef enum ecs_rule_var_kind_t {
    EcsRuleVarKindTable, /* Used for sorting, must be smallest */
    EcsRuleVarKindEntity,
    EcsRuleVarKindUnknown
} ecs_rule_var_kind_t;

typedef struct ecs_rule_reg_t {
    uint8_t var_id;
    union {
        ecs_entity_t entity;
        ecs_vector_t *type;
        ecs_table_t *table;
    } is;
} ecs_rule_reg_t;

/* Operations describe how the rule should be evaluated */
typedef enum ecs_rule_op_kind_t {
    EcsRuleInput,       /* Input placeholder, first instruction in every rule */
    EcsRuleFollow,      /* Follows a relationship depth-first */
    EcsRuleSelect,      /* Selects all ables for a given predicate */
    EcsRuleWith,        /* Applies a filter to a table or entity */
    EcsRuleEach,        /* Forwards each entity in a table */
    EcsRuleYield        /* Yield result */
} ecs_rule_op_kind_t;

/* Single operation */
typedef struct ecs_rule_op_t {
    ecs_rule_op_kind_t kind;    /* What kind of operation is it */
    ecs_rule_pair_t param;      /* Parameter that contains optional filter */
    ecs_entity_t subject;       /* If set, operation has a constant subject */

    int16_t on_ok;              /* Jump location when match succeeds */
    int16_t on_fail;            /* Jump location when match fails */

    int8_t column;              /* Corresponding column index in signature */
    uint8_t r_in;               /* Optional In/Out registers */
    uint8_t r_out;

    bool has_in, has_out;       /* Keep track of whether operation uses input
                                 * and/or output registers. This helps with
                                 * debugging rule programs. */
} ecs_rule_op_t;

/* With context. Shared with select. */
typedef struct ecs_rule_with_ctx_t {
    ecs_sparse_t *table_set;    /* Currently evaluated table set */
    int32_t table_index;        /* Currently evaluated index in table set */
} ecs_rule_with_ctx_t;

typedef struct ecs_rule_follow_frame_t {
    ecs_rule_with_ctx_t with_ctx;
    ecs_table_t *table;
    int32_t row;
} ecs_rule_follow_frame_t;

/* Follow context */
typedef struct ecs_rule_follow_ctx_t {
    ecs_rule_follow_frame_t storage[16]; /* Alloc-free array for small trees */
    ecs_rule_follow_frame_t *stack;
    int32_t sp;
} ecs_rule_follow_ctx_t;

/* Each context */
typedef struct ecs_rule_each_ctx_t {
    int32_t row;                /* Currently evaluated row in evaluated table */
} ecs_rule_each_ctx_t;

/* From operation context */
typedef struct ecs_rule_from_ctx_t {
    ecs_type_t type;            /* Currently evaluated type */
    int32_t column;             /* Currently evaluated column in type */
} ecs_rule_from_ctx_t;

/* Operation context. This is a per-operation, per-iterator structure that
 * stores information for stateful operations. */
typedef struct ecs_rule_op_ctx_t {
    union {
        ecs_rule_follow_ctx_t follow;
        ecs_rule_with_ctx_t with;
        ecs_rule_each_ctx_t each;
        ecs_rule_from_ctx_t from;
    } is;
} ecs_rule_op_ctx_t;

/* Rule variables allow for the rule to be parameterized */
typedef struct ecs_rule_var_t {
    ecs_rule_var_kind_t kind;
    char *name;       /* Variable name */
    int32_t id;       /* Unique variable id */
    int32_t occurs;   /* Number of occurrences (used for operation ordering) */
    int32_t depth;  /* Depth in dependency tree (used for operation ordering) */
    bool marked;      /* Used for cycle detection */
} ecs_rule_var_t;

/* Top-level rule datastructure */
struct ecs_rule_t {
    ecs_world_t *world;         /* Ref to world so rule can be used by itself */
    ecs_rule_op_t *operations;  /* Operations array */
    ecs_rule_var_t *variables;  /* Variable array */
    ecs_sig_t sig;              /* Parsed signature expression */

    int32_t variable_count;     /* Number of variables in signature */
    int32_t subject_variable_count;
    int32_t register_count;    /* Number of registers in rule */
    int32_t column_count;       /* Number of columns in signature */
    int32_t operation_count;    /* Number of operations in rule */
};

static
void rule_error(
    ecs_rule_t *rule,
    const char *fmt,
    ...)
{
    va_list valist;
    va_start(valist, fmt);
    char *msg = ecs_vasprintf(fmt, valist);
    ecs_os_err("error: %s: %s", rule->sig.expr, msg);
    ecs_os_free(msg);
}

static
ecs_rule_op_t* create_operation(
    ecs_rule_t *rule)
{
    int8_t cur = rule->operation_count ++;
    rule->operations = ecs_os_realloc(
        rule->operations, (cur + 1) * ECS_SIZEOF(ecs_rule_op_t));

    ecs_rule_op_t *result = &rule->operations[cur];
    memset(result, 0, sizeof(ecs_rule_op_t));
    return result;
}

static
ecs_rule_var_t* create_variable(
    ecs_rule_t *rule,
    ecs_rule_var_kind_t kind,
    const char *name)
{
    uint8_t cur = ++ rule->variable_count;
    rule->variables = ecs_os_realloc(
        rule->variables, cur * ECS_SIZEOF(ecs_rule_var_t));

    ecs_rule_var_t *var = &rule->variables[cur - 1];
    if (name) {
        var->name = ecs_os_strdup(name);
    } else {
        /* Anonymous register */
        char name_buff[32];
        sprintf(name_buff, "_%u", cur - 1);
        var->name = ecs_os_strdup(name_buff);
    }

    var->kind = kind;

    /* The variable id is the location in the variable array and also points to
     * the register element that corresponds with the variable. */
    var->id = cur - 1;

    /* Depth is used to calculate how far the variable is from the root, where
     * the root is the variable with 0 dependencies. */
    var->depth = UINT8_MAX;
    var->marked = false;
    var->occurs = 0;

    if (rule->register_count < rule->variable_count) {
        rule->register_count ++;
    }

    return var;
}

/* Find variable with specified name and type. If Unknown is provided as type,
 * the function will return any variable with the provided name. The root 
 * variable can occur both as a table and entity variable, as some rules
 * require that each entity in a table is iterated. In this case, there are two
 * variables, one for the table and one for the entities in the table, that both
 * have the same name. */
static
ecs_rule_var_t* find_variable(
    const ecs_rule_t *rule,
    ecs_rule_var_kind_t kind,
    const char *name)
{
    ecs_assert(rule != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(name != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_rule_var_t *variables = rule->variables;
    int32_t i, count = rule->variable_count;
    
    for (i = 0; i < count; i ++) {
        ecs_rule_var_t *variable = &variables[i];
        if (!strcmp(name, variable->name)) {
            if (kind == EcsRuleVarKindUnknown || kind == variable->kind) {
                return variable;
            }
        }
    }

    return NULL;
}

/* Ensure variable with specified name and type exists. If an existing variable
 * is found with an unknown type, its type will be overwritten with the 
 * specified type. During the variable ordering phase it is not yet clear which
 * variable is the root. Which variable is the root determines its type, which
 * is why during this phase variables are still untyped. */
static
ecs_rule_var_t* ensure_variable(
    ecs_rule_t *rule,
    ecs_rule_var_kind_t kind,
    const char *name)
{
    ecs_rule_var_t *var = find_variable(rule, kind, name);
    if (!var) {
        var = create_variable(rule, kind, name);
    } else {
        if (var->kind == EcsRuleVarKindUnknown) {
            var->kind = kind;
        }
    }

    return var;
}

/* Get variable from a term identifier */
ecs_rule_var_t* column_id_to_var(
    ecs_rule_t *rule,
    ecs_sig_identifier_t *sid)
{
    if (!sid->entity) {
        return find_variable(rule, EcsRuleVarKindUnknown, sid->name);
    } else if (sid->entity == EcsThis) {
        return find_variable(rule, EcsRuleVarKindUnknown, ".");
    } else {
        return NULL;
    }
}

/* Get variable from a term predicate */
ecs_rule_var_t* column_pred(
    ecs_rule_t *rule,
    ecs_sig_column_t *column)
{
    return column_id_to_var(rule, &column->pred);
}

/* Get variable from a term subject */
ecs_rule_var_t* column_subj(
    ecs_rule_t *rule,
    ecs_sig_column_t *column)
{
    return column_id_to_var(rule, &column->argv[0]);
}

/* Get variable from a term object */
ecs_rule_var_t* column_obj(
    ecs_rule_t *rule,
    ecs_sig_column_t *column)
{
    if (column->argc > 1) {
        return column_id_to_var(rule, &column->argv[1]);
    } else {
        return NULL;
    }
}

/* Get register array for current stack frame. The stack frame is determined by
 * the current operation that is evaluated. The register array contains the
 * values for the reified variables. If a variable hasn't been reified yet, its
 * register will store a wildcard. */
static
ecs_rule_reg_t* get_registers(
    ecs_rule_iter_t *it,
    int32_t op)    
{
    return &it->registers[op * it->rule->variable_count];
}

/* Get columns array. Columns store, for each matched column in a table, the 
 * index at which it occurs. This reduces the amount of searching that
 * operations need to do in a type, since select/with already provide it. */
static
int32_t* get_columns(
    ecs_rule_iter_t *it,
    int32_t op)    
{
    return &it->columns[op * it->rule->column_count];
}

/* This encodes a column expression into a pair. A pair stores information about
 * the variable(s) associated with the column. Pairs are used by operations to
 * apply filters, and when there is a match, to reify variables. */
static
ecs_rule_pair_t column_to_pair(
    ecs_rule_t *rule,
    ecs_sig_column_t *column)
{
    ecs_rule_pair_t result = {0};

    /* Columns must always have at least one argument (the subject) */
    ecs_assert(column->argc >= 1, ECS_INTERNAL_ERROR, NULL);

    ecs_entity_t pred_id = column->pred.entity;

    /* If the predicate id is a variable, find the variable and encode its id
     * in the pair so the operation can find it later. */
    if (!pred_id || pred_id == EcsThis) {
        /* Always lookup the as an entity, as pairs never refer to tables */
        const ecs_rule_var_t *var = find_variable(
            rule, EcsRuleVarKindEntity, column->pred.name);

        /* Variables should have been declared */
        ecs_assert(var != NULL, ECS_INTERNAL_ERROR, NULL);
        ecs_assert(var->kind == EcsRuleVarKindEntity, ECS_INTERNAL_ERROR, NULL);
        result.pred = var->id;

        /* Set flag so the operation can see that the predicate is a variable */
        result.reg_mask |= RULE_PAIR_PREDICATE;
    } else {
        /* If the predicate is not a variable, simply store its id. */
        result.pred = pred_id;
    }

    /* The pair doesn't do anything with the subject (subjects are the things that
     * are matched against pairs) so if the column does not have a object, 
     * there is nothing left to do. */
    if (column->argc == 1) {
        return result;
    }

    /* If arguments is higher than 2 this is not a pair but a nested rule */
    ecs_assert(column->argc == 2, ECS_INTERNAL_ERROR, NULL);

    ecs_entity_t obj_id = column->argv[1].entity;

    /* Same as above, if the object is a variable, store it and flag it */
    if (!obj_id || obj_id == EcsThis) {
        const ecs_rule_var_t *var = find_variable(
            rule, EcsRuleVarKindEntity, column->argv[1].name);

        /* Variables should have been declared */
        ecs_assert(var != NULL, ECS_INTERNAL_ERROR, NULL);
        ecs_assert(var->kind == EcsRuleVarKindEntity, ECS_INTERNAL_ERROR, NULL);

        result.obj = var->id;
        result.reg_mask |= RULE_PAIR_OBJECT;
    } else {
        /* If the object is not a variable, simply store its id */
        result.obj = obj_id;
    }

    return result;
}

/* When an operation has a pair, it is used to filter its input. This function
 * translates a pair back into an entity id, and in the process substitutes the
 * variables that have already been filled out. It's one of the most important
 * functions, as a lot of the filtering logic depends on having an entity that
 * has all of the reified variables correctly filled out. 
 * This function is in essence the decoder for column_to_pair.*/
static
ecs_entity_t pair_to_entity(
    ecs_rule_iter_t *it,
    ecs_rule_pair_t pair)
{
    ecs_entity_t pred = pair.pred;
    ecs_entity_t obj = pair.obj;

    /* Get registers in case we need to resolve ids from registers. Get them
     * from the previous, not the current stack frame as the current operation
     * hasn't reified its variables yet. */
    ecs_rule_reg_t *regs = get_registers(it, it->op - 1);

    if (pair.reg_mask & RULE_PAIR_PREDICATE) {
        pred = regs[pred].is.entity;
    }
    if (pair.reg_mask & RULE_PAIR_OBJECT) {
        obj = regs[obj].is.entity;
    }

    if (!obj) {
        return pred;
    } else {
        return ecs_trait(obj, pred);
    }
}

static
bool pair_has_var(
    const ecs_rule_t *rule,
    ecs_rule_pair_t pair,
    int32_t var_id)
{
    if (var_id == UINT8_MAX) {
        return false;
    }

    int8_t pred = (int8_t)pair.pred;
    int8_t obj = (int8_t)pair.obj;

    if (pair.reg_mask & RULE_PAIR_PREDICATE) {
        if (var_id == pred) {
            return true;
        } else {
            if (!strcmp(rule->variables[pred].name, 
                rule->variables[var_id].name))
            {
                return true;
            }
        }
    }

    if (pair.reg_mask & RULE_PAIR_OBJECT) {
        if (var_id == obj) {
            return true;
        } else {
            if (!strcmp(rule->variables[obj].name, 
                rule->variables[var_id].name))
            {
                return true;
            }
        }
    }

    return false;
}

/* This function is used to test whether an entity id contains wildcards. If
 * the encoded pair contains wildcards, variables may need to be reified. */
static
bool entity_is_wildcard(
    ecs_entity_t e)
{
    if (e == EcsWildcard) {
        return true;
    } else if (ECS_HAS_ROLE(e, TRAIT)) {
        if (ecs_entity_t_lo(e) == EcsWildcard) {
            return true;
        } else if (ecs_entity_t_hi(e & ECS_COMPONENT_MASK) == EcsWildcard) {
            return true;
        }
    }
    return false;
}

/* This function iterates a type with a provided pair expression, as is returned
 * by pair_to_entity. It starts looking in the type at an offset ('column') and
 * returns the first matching element. */
static
int32_t find_next_match(
    ecs_type_t type, 
    int32_t column,
    ecs_entity_t look_for)
{
    /* Scan the type for the next match */
    int32_t i, count = ecs_vector_count(type);
    ecs_entity_t *entities = ecs_vector_first(type, ecs_entity_t);

    /* If this is a trait, the wildcard can be either the type or object */
    if (ECS_HAS_ROLE(look_for, TRAIT)) {
        /* If the type is not a wildcard, the next element must match the 
         * queried for entity, or the type won't contain any more matches. */
        ecs_entity_t type_id = ecs_entity_t_hi(look_for);
        if (type_id != EcsWildcard) {
            /* Evaluate at most one element if column is not 0. If column is 0,
             * evaluate entire type. */
            if (column && column < count) {
                count = column + 1;
            }
        }
    }

    /* Mask the parts of the id that are not wildcards */
    ecs_entity_t lo = ecs_entity_t_lo(look_for);
    ecs_entity_t hi = ecs_entity_t_hi(look_for & ECS_COMPONENT_MASK);
    ecs_entity_t expr_mask = ECS_ROLE_MASK & look_for;
    ecs_entity_t eq_mask = ECS_ROLE_MASK & look_for;

    expr_mask |= 0xFFFFFFFF * (lo != EcsWildcard);
    expr_mask |= ((uint64_t)0xFFFFFFFF << 32) * (hi != EcsWildcard);

    eq_mask |= lo * (lo != EcsWildcard);
    eq_mask |= (hi << 32) * (hi != EcsWildcard);

    /* Find next column that equals look_for after masking out the wildcards */
    for (i = column; i < count; i ++) {
        if ((entities[i] & expr_mask) == eq_mask) {
            return i;
        }
    }

    /* No matching columns were found in remainder of type */
    return -1;
}

/* This function is responsible for reifying the variables (filling them out 
 * with their actual values as soon as they are known). It uses the pair 
 * expression returned by pair_to_entity, and attempts to fill out each of the
 * wildcards in the pair. If a variable isn't reified yet, the pair expression
 * will still contain one or more wildcards, which is harmless as the respective
 * registers will also point to a wildcard. */
static
void reify_variables(
    ecs_rule_iter_t *it, 
    ecs_rule_pair_t pair,
    ecs_type_t type,
    int32_t column,
    ecs_entity_t look_for)
{
    /* If look_for does not contain wildcards, there is nothing to resolve */
    ecs_assert(entity_is_wildcard(look_for), ECS_INTERNAL_ERROR, NULL);

    const ecs_rule_t *rule = it->rule;
    const ecs_rule_var_t *vars = rule->variables;

    /* If the pair contains references to registers, check if any of them were
     * wildcards while the operation was being evaluated. */
    if (pair.reg_mask) {
        ecs_rule_reg_t *regs = get_registers(it, it->op);
        ecs_entity_t *elem = ecs_vector_get(type, ecs_entity_t, column);
        ecs_assert(elem != NULL, ECS_INTERNAL_ERROR, NULL);

        /* If the type part of a pair is a register, depending on whether we're
         * looking for a trait or not we must get the lo or hi part */
        if (pair.reg_mask & RULE_PAIR_PREDICATE) {
            /* Check if type is a wildcard. If it's not a wildcard it's possible
             * that a previous instruction filled out the register or that the
             * variable was provided as input. */
            if (ECS_HAS_ROLE(look_for, TRAIT)) {
                if (ecs_entity_t_hi(look_for & ECS_COMPONENT_MASK) == EcsWildcard) {
                    ecs_assert(vars[pair.pred].kind == EcsRuleVarKindEntity, 
                        ECS_INTERNAL_ERROR, NULL);
                    regs[pair.pred].is.entity = 
                        ecs_entity_t_hi(*elem & ECS_COMPONENT_MASK);
                }
            } else if (look_for == EcsWildcard) {
                ecs_assert(vars[pair.pred].kind == EcsRuleVarKindEntity, 
                    ECS_INTERNAL_ERROR, NULL);
                regs[pair.pred].is.entity = *elem;
            }
        }

        /* If object is a wildcard, this is guaranteed to be a trait */
        if (pair.reg_mask & RULE_PAIR_OBJECT) {
            ecs_assert(ECS_HAS_ROLE(look_for, TRAIT), ECS_INTERNAL_ERROR, NULL);

            /* Same as above, if object is not a wildcard it could already have
             * been resolved by either input or a previous operation. */
            if (ecs_entity_t_lo(look_for) == EcsWildcard) {
                ecs_assert(vars[pair.obj].kind == EcsRuleVarKindEntity, ECS_INTERNAL_ERROR, NULL);
                regs[pair.obj].is.entity = ecs_entity_t_lo(*elem);
            }
        }
    }
}

/* Returns whether variable is a subject */
static
bool is_subject(
    ecs_rule_t *rule,
    ecs_rule_var_t *var)
{
    ecs_assert(rule != NULL, ECS_INTERNAL_ERROR, NULL);

    if (!var) {
        return false;
    }

    if (var->id < rule->subject_variable_count) {
        return true;
    }

    return false;
}

static
uint8_t get_variable_depth(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    ecs_rule_var_t *root,
    int recur);

static
uint8_t trace_object(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    ecs_rule_var_t *root,
    int recur)    
{
    ecs_sig_column_t *columns = ecs_vector_first(
        rule->sig.columns, ecs_sig_column_t);

    int32_t i, count = rule->column_count;

    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        ecs_rule_var_t 
        *pred = column_pred(rule, column),
        *subj = column_subj(rule, column),
        *obj = column_obj(rule, column); 

        if (obj != var) {
            continue;
        }

        if (pred && !pred->marked) {
            get_variable_depth(rule, pred, root, recur + 1);
        }

        if (subj && !subj->marked) {
            get_variable_depth(rule, subj, root, recur + 1);
        }
    }

    return 0;
}

static
uint8_t get_depth_from_var(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    ecs_rule_var_t *root,
    int recur)
{
    /* If variable is the root, return its depth */
    if (var == root || var->depth != UINT8_MAX) {
        return var->depth + 1;
    }

    if (var->marked) {
        return 0;
    }
    
    uint8_t depth = get_variable_depth(rule, var, root, recur + 1);
    if (depth == UINT8_MAX) {
        return depth;
    } else {
        return depth + 1;
    }
}

static
uint8_t get_depth_from_term(
    ecs_rule_t *rule,
    ecs_rule_var_t *cur,
    ecs_rule_var_t *pred,
    ecs_rule_var_t *obj,
    ecs_rule_var_t *root,
    int recur)
{
    uint8_t result = UINT8_MAX;

    ecs_assert(cur != pred || cur != obj, ECS_INTERNAL_ERROR, NULL);

    /* If neither of the other parts of the terms are variables, this
     * variable is guaranteed to have no dependencies. */
    if (!pred && !obj) {
        result = 0;
    } else {
        /* If this is a variable that is not the same as the current, 
         * we can use it to determine dependency depth. */
        if (pred && cur != pred) {
            uint8_t depth = get_depth_from_var(rule, pred, root, recur);
            if (depth == UINT8_MAX) {
                return UINT8_MAX;
            }

            /* If the found depth is lower than the depth found, overwrite it */
            if (depth < result) {
                result = depth;
            }
        }

        /* Same for obj */
        if (obj && cur != obj) {
            uint8_t depth = get_depth_from_var(rule, obj, root, recur);
            if (depth == UINT8_MAX) {
                return UINT8_MAX;
            }

            if (depth < result) {
                result = depth;
            }
        }
    }

    return result;
}

/* Find the depth of the dependency tree from the variable to the root */
static
uint8_t get_variable_depth(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    ecs_rule_var_t *root,
    int recur)
{
    var->marked = true;

    /* Iterate columns, find all instances where 'var' is not used as subject.
     * If the subject of that column is either the root or a variable for which
     * the depth is known, the depth for this variable can be determined. */
    ecs_sig_column_t *columns = ecs_vector_first(
        rule->sig.columns, ecs_sig_column_t);

    int32_t i, count = rule->column_count;
    uint8_t result = UINT8_MAX;

    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        ecs_rule_var_t 
        *pred = column_pred(rule, column),
        *subj = column_subj(rule, column),
        *obj = column_obj(rule, column);

        if (subj != var) {
            continue;
        }

        if (!is_subject(rule, pred)) {
            pred = NULL;
        }

        if (!is_subject(rule, obj)) {
            obj = NULL;
        }

        uint8_t depth = get_depth_from_term(rule, var, pred, obj, root, recur);
        if (depth < result) {
            result = depth;
        }
    }

    if (result == UINT8_MAX) {
        result = 0;
    }

    var->depth = result;    

    /* Dependencies are calculated from subject to (pred, obj). If there were
     * subjects that are only related by object (like (X, Y), (Z, Y)) it is
     * possible that those have not yet been found yet. To make sure those 
     * variables are found, loop again & follow object links */
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        ecs_rule_var_t 
        *subj = column_subj(rule, column),
        *obj = column_obj(rule, column);

        if (subj != var) {
            continue;
        }

        trace_object(rule, subj, root, recur);

        if (obj && obj != var) {
            trace_object(rule, obj, root, recur);
        }
    }

    return var->depth;
}

/* Compare function used for qsort. It ensures that variables are first ordered
 * by depth, followed by how often they occur. */
static
int compare_variable(
    const void* ptr1, 
    const void *ptr2)
{
    const ecs_rule_var_t *v1 = ptr1;
    const ecs_rule_var_t *v2 = ptr2;

    if (v1->kind < v2->kind) {
        return -1;
    } else if (v1->kind > v2->kind) {
        return 1;
    }

    if (v1->depth < v2->depth) {
        return -1;
    } else if (v1->depth > v2->depth) {
        return 1;
    }

    if (v1->occurs < v2->occurs) {
        return 1;
    } else {
        return -1;
    }

    return (v1->id < v2->id) - (v1->id > v2->id);
}

/* After all subject variables have been found, inserted and sorted, the 
 * remaining variables (predicate & object) still need to be inserted. This
 * function serves two purposes. The first purpose is to ensure that all 
 * variables are known before operations are emitted. This ensures that the
 * variables array won't be reallocated while emitting, which simplifies code.
 * The second purpose of the function is to ensure that if the root variable
 * (which, if it exists has now been created with a table type) is also inserted
 * with an entity type if required. This is used later to decide whether the
 * rule needs to insert an each instruction. */
static
void ensure_all_variables(
    ecs_rule_t *rule)
{
    ecs_sig_column_t *columns = ecs_vector_first(
        rule->sig.columns, ecs_sig_column_t);

    int32_t i, count = rule->column_count;
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        /* If predicate is a variable, make sure it has been registered */
        if (!column->pred.entity || (column->pred.entity == EcsThis)) {
            ensure_variable(rule, EcsRuleVarKindEntity, column->pred.name);
        }

        /* If subject is a variable and it is not This, make sure it is 
         * registered as an entity variable. This ensures that the program will
         * correctly return all permutations */
        if (!column->argv[0].entity) {
            ensure_variable(rule, EcsRuleVarKindEntity, column->argv[0].name);
        }

        /* If object is a variable, make sure it has been registered */
        if (column->argc > 1 && (!column->argv[1].entity || 
            column->argv[1].entity == EcsThis)) 
        {
            ensure_variable(rule, EcsRuleVarKindEntity, column->argv[1].name);
        }        
    }    
}

/* Scan for variables, put them in optimal dependency order. */
static
int scan_variables(
    ecs_rule_t *rule)
{
    /* Objects found in rule. One will be elected root */
    uint16_t subject_count = 0;

    /* If this (.) is found, it always takes precedence in root election */
    uint8_t this_var = UINT8_MAX;

    /* Keep track of the subject variable that occurs the most. In the absence of
     * this (.) the variable with the most occurrences will be elected root. */
    uint8_t max_occur = 0;
    uint8_t max_occur_var = UINT8_MAX;

    /* Step 1: find all possible roots */
    ecs_sig_column_t *columns = ecs_vector_first(rule->sig.columns, ecs_sig_column_t);
    int32_t i, count = rule->column_count;
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        /* Evaluate the subject. The predicate and object are not evaluated, 
         * since they never can be elected as root. */
        if (!column->argv[0].entity || column->argv[0].entity == EcsThis) {
            const char *subj_name = column->argv[0].name;

            ecs_rule_var_t *subj = find_variable(
                rule, EcsRuleVarKindTable, subj_name);
            if (!subj) {
                subj = create_variable(rule, EcsRuleVarKindTable, subj_name);
                if (subject_count >= ECS_RULE_MAX_VARIABLE_COUNT) {
                    rule_error(rule, "too many variables in rule");
                    goto error;
                }
            }

            if (++ subj->occurs > max_occur) {
                max_occur = subj->occurs;
                max_occur_var = subj->id;
            }
        }
    }

    rule->subject_variable_count = rule->variable_count;

    ensure_all_variables(rule);

    /* Step 2: elect a root. This is either this (.) or the variable with the
     * most occurrences. */
    uint8_t root_var = this_var;
    if (root_var == UINT8_MAX) {
        root_var = max_occur_var;
        if (root_var == UINT8_MAX) {
            /* If no subject variables have been found, the rule expression only
             * operates on a fixed set of entities, in which case no root 
             * election is required. */
            goto done;
        }
    }

    ecs_rule_var_t *root = &rule->variables[root_var];
    root->depth = get_variable_depth(rule, root, root, 0);

    /* Step 4: order variables by depth, followed by occurrence. The variable
     * array will later be used to lead the iteration over the columns, and
     * determine which operations get inserted first. */
    qsort(rule->variables, rule->variable_count, sizeof(ecs_rule_var_t), 
        compare_variable);

    /* Iterate variables to correct ids after sort */
    for (i = 0; i < rule->variable_count; i ++) {
        rule->variables[i].id = i;
    }

done:
    return 0;
error:
    return -1;
}

static
bool is_transitive(
    ecs_rule_t *rule,
    ecs_rule_pair_t pair)
{
    ecs_world_t *world = rule->world;

    bool transitive = false;

    /* Test if predicate is transitive */
    if (pair.pred && pair.obj) {
        /* If predicate is not variable, check if it's transitive */
        if (!(pair.reg_mask & RULE_PAIR_PREDICATE)) {
            ecs_entity_t pred_id = pair.pred;
            transitive = ecs_has_entity(world, pred_id, EcsTransitive);
        }
    }

    return transitive;
}

static
ecs_rule_op_t* insert_operation(
    ecs_rule_t *rule,
    int32_t column_index)
{
    ecs_rule_op_t *op = create_operation(rule);
    op->on_ok = rule->operation_count;
    op->on_fail = rule->operation_count - 2;

    /* Parse the column's type into a pair. A pair extracts the ids from
     * the column, and replaces variables with wildcards which can then
     * be matched against actual relationships. A pair retains the 
     * information about the variables, so that when a match happens,
     * the pair can be used to reify the variable. */
    if (column_index != -1) {
        ecs_sig_column_t *column = ecs_vector_get(
            rule->sig.columns, ecs_sig_column_t, column_index);
        ecs_assert(column != NULL, ECS_INTERNAL_ERROR, NULL);    

        ecs_rule_pair_t pair = column_to_pair(rule, column);
 
        op->param = pair;
    } else {
        /* Not all operations have a filter (like Each) */
    }

    /* Store corresponding signature column so we can correlate and
     * store the table columns with signature columns. */
    op->column = column_index;

    return op;
}

static
void write_variable(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    int32_t column,
    bool *written)
{
    ecs_rule_var_t 
    *tvar = find_variable(rule, EcsRuleVarKindTable, var->name),
    *evar = find_variable(rule, EcsRuleVarKindEntity, var->name);

    /* If variable is used as predicate or object, it should have been 
     * registered as an entity. */
    ecs_assert(evar != NULL, ECS_INTERNAL_ERROR, NULL);

    /* Usually table variables are resolved before they are used as a predicate
     * or object, but in the case of cyclic dependencies this is not guaranteed.
     * Only insert an each instruction of the table variable has been written */
    if (tvar && written[tvar->id]) {
        /* If the variable has been written as a table but not yet
         * as an entity, insert an each operation that yields each
         * entity in the table. */
        if (evar && !written[evar->id]) {
            ecs_rule_op_t *op = insert_operation(rule, column);
            op->kind = EcsRuleEach;
            op->has_in = true;
            op->has_out = true;
            op->r_in = tvar->id;
            op->r_out = evar->id;
        }
    }

    /* Entity will either be written or has been written */
    written[evar->id] = true;
}

ecs_rule_t* ecs_rule_new(
    ecs_world_t *world,
    const char *expr)
{
    ecs_rule_t *result = ecs_os_calloc(ECS_SIZEOF(ecs_rule_t));

    /* Parse the signature expression. This initializes the columns array which
     * contains the information about which components/pairs are requested. */
    if (ecs_sig_init(world, NULL, expr, &result->sig)) {
        ecs_os_free(result);
        return NULL;
    }

    ecs_sig_t *sig = &result->sig;
    ecs_sig_column_t *columns = ecs_vector_first(sig->columns, ecs_sig_column_t);
    int32_t v, c, column_count = ecs_vector_count(sig->columns);

    result->world = world;
    result->column_count = column_count;

    /* Create first operation, which is always Input. This creates an entry in
     * the register stack for the initial state. */
    ecs_rule_op_t *op = create_operation(result);
    op->kind = EcsRuleInput;

    /* The first time Input is evaluated it goes to the next/first operation */
    op->on_ok = 1;

    /* When Input is evaluated with redo = true it will return false, which will
     * finish the program as op becomes -1. */
    op->on_fail = -1;

    /* Find all variables & resolve dependencies */
    if (scan_variables(result) != 0) {
        goto error;
    }

    /* Trace which variables have been written while inserting instructions.
     * This determines which instruction needs to be inserted */
    bool written[ECS_RULE_MAX_VARIABLE_COUNT] = { false };

    /* First insert all instructions that do not have a variable subject. Such
     * instructions iterate the type of an entity literal and are usually good
     * candidates for quickly narrowing down the set of potential results. */
    for (c = 0; c < column_count; c ++) {
        ecs_sig_column_t *column = &columns[c];
        ecs_rule_var_t* subj = column_subj(result, column);
        if (subj) {
            continue;
        }

        /* If predicate and/or object are variables, mark them as written */
        ecs_rule_var_t 
        *pred = column_pred(result, column),
        *obj = column_obj(result, column);

        if (pred) {
            write_variable(result, pred, c, written);
        }
        if (obj) {
            write_variable(result, obj, c, written);
        } 

        op = insert_operation(result, c);
        op->kind = EcsRuleWith;
        op->r_in = UINT8_MAX; /* Indicate literal */
        op->has_in = true;
        op->subject = column->argv[0].entity;
        ecs_assert(op->subject != 0, ECS_INTERNAL_ERROR, NULL);
    }

    /* Insert variables based on dependency order */
    for (v = 0; v < result->subject_variable_count; v ++) {
        ecs_rule_var_t *var = &result->variables[v];

        ecs_assert(var->kind == EcsRuleVarKindTable, ECS_INTERNAL_ERROR, NULL);

        for (c = 0; c < column_count; c ++) {
            ecs_sig_column_t *column = &columns[c];

            /* Only process columns for which variable is subject */
            ecs_rule_var_t* subj = column_subj(result, column);
            if (subj != var) {
                continue;
            }

            bool entity_written = false, table_written = written[var->id];
            ecs_rule_var_t *entity_var = find_variable(result, 
                EcsRuleVarKindEntity, var->name);
            if (entity_var) {
                entity_written = written[entity_var->id];
            }

            /* Mark predicate & object variables as entities, as they will be 
             * written by the operation */
            ecs_rule_var_t 
            *pred = column_pred(result, column),
            *obj = column_obj(result, column);

            if (pred) {
                write_variable(result, pred, c, written);
            }
            if (obj) {
                write_variable(result, obj, c, written);
            } 

            op = insert_operation(result, c);

            /* If the variable is already written as an entity, use From so the
             * filter is applied to the type of the entity. */
            if (entity_written) {
                op->kind = EcsRuleWith;
                op->has_in = true;
                op->r_in = entity_var->id;
            
            /* If variable is written as a table, use With so the filter is
             * applied to the table */
            } else if (table_written) {
                op->kind = EcsRuleWith;
                op->has_in = true;
                op->r_in = var->id;
           
            /* If the variable was not written yet, insert a select */
            } else {
                if (is_transitive(result, op->param)) {
                    op->kind = EcsRuleFollow;
                } else {
                    op->kind = EcsRuleSelect;
                }
                op->has_out = true;
                op->r_out = var->id;

                /* A select reifies the table variable */
                written[var->id] = true;
            }      
        }
    }

    /* Verify all subject variables have been written. Subject variables are of
     * the table type, and a select/follow should have been inserted for each */
    for (v = 0; v < result->subject_variable_count; v ++) {
        if (!written[v]) {
            /* If the table variable hasn't been written, this can only happen
             * if an instruction wrote the variable before a select/follow could
             * have been inserted for it. Make sure that this is the case by
             * testing if an entity variable exists and whether it has been
             * written. */
            ecs_rule_var_t *var = find_variable(
                result, EcsRuleVarKindEntity, result->variables[v].name);
            ecs_assert(written[var->id], ECS_INTERNAL_ERROR, NULL);
        }
    }

    /* Make sure that all entity variables are written. With the exception of
     * the this variable, which can be returned as a table, other variables need
     * to be available as entities. This ensures that all permutations for all
     * variables are correctly returned by the iterator. When an entity variable
     * hasn't been written yet at this point, it is because it only constrained
     * through a common predicate or object. */
    for (; v < result->variable_count; v ++) {
        if (!written[v]) {
            ecs_rule_var_t *var = &result->variables[v];
            ecs_assert(var->kind == EcsRuleVarKindEntity, 
                ECS_INTERNAL_ERROR, NULL);

            ecs_rule_var_t *table_var = find_variable(
                result, EcsRuleVarKindTable, var->name);
            
            /* A table variable must exist if the variable hasn't been resolved
             * yet. If there doesn't exist one, this could indicate an 
             * unconstrained variable which should have been caught earlier */
            ecs_assert(table_var != NULL, ECS_INTERNAL_ERROR, NULL);

            /* Insert each operation that takes the table variable as input, and
             * yields each entity in the table */
            op = insert_operation(result, -1);
            op->kind = EcsRuleEach;
            op->r_in = table_var->id;
            op->r_out = var->id;
            op->has_in = true;
            op->has_out = true;
            written[var->id] = true;
        }
    }     

    /* Insert yield instruction */
    op = create_operation(result);
    op->kind = EcsRuleYield;
    op->has_in = true;
    op->on_fail = result->operation_count - 2;
    /* Yield can only fail since it is the end of the program */

    /* Find variable associated with this. It is possible that the variable
     * exists both as a table and as an entity. This can happen when a rule
     * first selects a table for this, but then subsequently needs to evaluate
     * each entity in that table. In that case the yield instruction should
     * return the entity, so look for that first. */
    ecs_rule_var_t *var = find_variable(result, EcsRuleVarKindEntity, ".");
    if (!var) {
        var = find_variable(result, EcsRuleVarKindTable, ".");
    }

    /* If there is no this, there is nothing to yield. In that case the rule
     * simply returns true or false. */
    if (!var) {
        op->r_in = UINT8_MAX;
    } else {
        op->r_in = var->id;
    }

    return result;
error:
    /* TODO: proper cleanup */
    ecs_os_free(result);
    return NULL;
}

void ecs_rule_free(
    ecs_rule_t *rule)
{
    int32_t i;
    for (i = 0; i < rule->variable_count; i ++) {
        ecs_os_free(rule->variables[i].name);
    }
    ecs_os_free(rule->variables);
    ecs_os_free(rule->operations);

    ecs_sig_deinit(&rule->sig);

    ecs_os_free(rule);
}

/* Quick convenience function to get a variable from an id */
ecs_rule_var_t* get_variable(
    const ecs_rule_t *rule,
    uint8_t var_id)
{
    if (var_id == UINT8_MAX) {
        return NULL;
    }

    return &rule->variables[var_id];
}

/* Convert the program to a string. This can be useful to analyze how a rule is
 * being evaluated. */
char* ecs_rule_str(
    ecs_rule_t *rule)
{
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    char filter_expr[256];

    int32_t i, count = rule->operation_count;
    for (i = 1; i < count; i ++) {
        ecs_rule_op_t *op = &rule->operations[i];
        ecs_rule_pair_t pair = op->param;
        ecs_entity_t type = pair.pred;
        ecs_entity_t object = pair.obj;
        const char *type_name, *object_name;

        if (pair.reg_mask & RULE_PAIR_PREDICATE) {
            ecs_rule_var_t *type_var = &rule->variables[type];
            type_name = type_var->name;
        } else {
            type_name = ecs_get_name(rule->world, type);
        }

        if (object) {
            if (pair.reg_mask & RULE_PAIR_OBJECT) {
                ecs_rule_var_t *obj_var = &rule->variables[object];;
                object_name = obj_var->name;
            } else {
                object_name = ecs_get_name(rule->world, object);
            }
        }

        ecs_strbuf_append(&buf, "%d: [Pass:%d, Fail:%d] ", i, 
            op->on_ok, op->on_fail);

        bool has_filter = false;

        switch(op->kind) {
        case EcsRuleFollow:
            ecs_strbuf_append(&buf, "follow");
            has_filter = true;
            break;
        case EcsRuleSelect:
            ecs_strbuf_append(&buf, "select");
            has_filter = true;
            break;
        case EcsRuleWith:
            ecs_strbuf_append(&buf, "with  ");
            has_filter = true;
            break;
        case EcsRuleEach:
            ecs_strbuf_append(&buf, "each  ");
            break;
        case EcsRuleYield:
            ecs_strbuf_append(&buf, "yield ");
            break;
        default:
            continue;
        }

        if (op->has_in) {
            ecs_rule_var_t *r_in = get_variable(rule, op->r_in);
            if (r_in) {
                ecs_strbuf_append(&buf, " %s%s", 
                    r_in->kind == EcsRuleVarKindTable ? "t" : "",
                    r_in->name);
            } else if (op->subject) {
                ecs_strbuf_append(&buf, " %s", 
                    ecs_get_name(rule->world, op->subject));
            }
        }

        if (op->has_out) {
            ecs_rule_var_t *r_out = get_variable(rule, op->r_out);
            if (r_out) {
                ecs_strbuf_append(&buf, " > %s%s", 
                    r_out->kind == EcsRuleVarKindTable ? "t" : "",
                    r_out->name);
            } else if (op->subject) {
                ecs_strbuf_append(&buf, " > %s <- ", 
                    ecs_get_name(rule->world, op->subject));
            }
        }

        if (has_filter) {
            if (!object) {
                sprintf(filter_expr, "(%s)", type_name);
            } else {
                sprintf(filter_expr, "(%s, %s)", type_name, object_name);
            }
            ecs_strbuf_append(&buf, " %s", filter_expr);
        }

        ecs_strbuf_appendstr(&buf, "\n");
    }

    return ecs_strbuf_get(&buf);
}

/* Public function that returns number of variables. This enables an application
 * to iterate the variables and obtain their values. */
int32_t ecs_rule_variable_count(
    const ecs_rule_t *rule)
{
    ecs_assert(rule != NULL, ECS_INTERNAL_ERROR, NULL);
    return rule->variable_count;
}

/* Public function to find a variable by name */
int32_t ecs_rule_find_variable(
    const ecs_rule_t *rule,
    const char *name)
{
    ecs_rule_var_t *v = find_variable(rule, EcsRuleVarKindEntity, name);
    if (v) {
        return v->id;
    } else {
        return -1;
    }
}

/* Public function to get the name of a variable. */
const char* ecs_rule_variable_name(
    const ecs_rule_t *rule,
    int32_t var_id)
{
    return rule->variables[var_id].name;
}

/* Public function to get the type of a variable. */
bool ecs_rule_variable_is_entity(
    const ecs_rule_t *rule,
    int32_t var_id)
{
    return rule->variables[var_id].kind == EcsRuleVarKindEntity;
}

/* Public function to get the value of a variable. */
ecs_entity_t ecs_rule_variable(
    ecs_iter_t *iter,
    int32_t var_id)
{
    ecs_rule_iter_t *it = &iter->iter.rule;

    /* We can only return entity variables */
    if (it->rule->variables[var_id].kind == EcsRuleVarKindEntity) {
        ecs_rule_reg_t *regs = get_registers(it, it->op);
        return regs[var_id].is.entity;
    } else {
        return 0;
    }
}

/* Create rule iterator */
ecs_iter_t ecs_rule_iter(
    const ecs_rule_t *rule)
{
    ecs_iter_t result = {0};

    result.world = rule->world;

    ecs_rule_iter_t *it = &result.iter.rule;
    it->rule = rule;

    if (rule->operation_count) {
        if (rule->variable_count) {
            it->registers = ecs_os_malloc(rule->operation_count * 
                rule->variable_count * ECS_SIZEOF(ecs_rule_reg_t));
        }
        
        it->op_ctx = ecs_os_malloc(rule->operation_count * 
            ECS_SIZEOF(ecs_rule_op_ctx_t));

        if (rule->column_count) {
            it->columns = ecs_os_malloc(rule->operation_count * 
                rule->column_count * ECS_SIZEOF(int32_t));
        }
    }

    it->op = 0;

    int i;
    for (i = 0; i < rule->variable_count; i ++) {
        it->registers[i].is.entity = EcsWildcard;
    }
    
    result.column_count = rule->column_count;
    if (result.column_count) {
        it->table.components = ecs_os_malloc(
            result.column_count * ECS_SIZEOF(ecs_entity_t));
    }

    return result;
}

void ecs_rule_iter_free(
    ecs_iter_t *iter)
{
    ecs_rule_iter_t *it = &iter->iter.rule;
    ecs_os_free(it->registers);
    ecs_os_free(it->columns);
    ecs_os_free(it->op_ctx);
    ecs_os_free(it->table.components);
    it->registers = NULL;
    it->columns = NULL;
    it->op_ctx = NULL;
}

/* Input operation. The input operation acts as a placeholder for the start of
 * the program, and creates an entry in the register array that can serve to
 * store variables passed to an iterator. */
static
bool eval_input(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    if (!redo) {
        /* First operation executed by the iterator. Always return true. */
        return true;
    } else {
        /* When Input is asked to redo, it means that all other operations have
         * exhausted their results. Input itself does not yield anything, so
         * return false. This will terminate rule execution. */
        return false;
    }
}

/* This function finds the next table in a table set, and is used by the select
 * operation. The function automatically skips empty tables, so that subsequent
 * operations don't waste a lot of processing for nothing. */
static
ecs_table_record_t* find_next_table(
    ecs_sparse_t *table_set,
    ecs_rule_with_ctx_t *op_ctx)
{
    ecs_table_record_t *table_record;
    int32_t count;

    /* If the current index is higher than the number of tables in the table
     * set, we've exhausted all matching tables. */
    if (op_ctx->table_index >= ecs_sparse_count(table_set)) {
        return NULL;
    }

    /* Find the next non-empty table */
    do {
        op_ctx->table_index ++;

        table_record = ecs_sparse_get(
            table_set, ecs_table_record_t, op_ctx->table_index);
        if (!table_record) {
            return NULL;
        }

        count = ecs_table_count(table_record->table);
    } while (!count);

    /* Paranoia check */
    ecs_assert(ecs_table_count(table_record->table) != 0, 
        ECS_INTERNAL_ERROR, NULL);

    return table_record;
}

static
bool eval_follow(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    const ecs_rule_t  *rule = it->rule;
    ecs_world_t *world = rule->world;
    ecs_rule_follow_ctx_t *op_ctx = &it->op_ctx[op_index].is.follow;
    ecs_rule_follow_frame_t *frame = NULL;
    ecs_table_record_t *table_record = NULL;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for output */
    int32_t sp, row;
    uint8_t r = op->r_out;
    ecs_assert(r != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param;
    ecs_entity_t look_for = pair_to_entity(it, pair);
    ecs_sparse_t *table_set;
    ecs_table_t *table = NULL;

    if (!redo) {
        op_ctx->stack = op_ctx->storage;
        sp = op_ctx->sp = 0;
        frame = &op_ctx->stack[sp];
        table_set = frame->with_ctx.table_set = ecs_map_get_ptr(
            world->store.table_index, ecs_sparse_t*, look_for);
        
        /* If no table set could be found for expression, yield nothing */
        if (!table_set) {
            return false;
        }

        frame->with_ctx.table_index = -1;
        table_record = find_next_table(table_set, &frame->with_ctx);
        
        /* If first table set does has no non-empty table, yield nothing */
        if (!table_record) {
            return false;
        }

        regs[r].is.table = frame->table = table_record->table;
        frame->row = 0;

        return true;
    }

    sp = op_ctx->sp;
    frame = &op_ctx->stack[sp];
    table = frame->table;
    table_set = frame->with_ctx.table_set;
    row = frame->row;

    do {
        if (!table) {
            sp = -- op_ctx->sp;
            if (sp <= 0) {
                return false;
            }

            frame = &op_ctx->stack[sp];
            table = frame->table;
            table_set = frame->with_ctx.table_set;
            row = frame->row;
        }        

        /* Must have a table at this point, either the first table or from the
        * previous frame. */
        ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

        /* Table must be non-empty, or it wouldn't have been returned */
        ecs_assert(ecs_table_count(table) > 0, ECS_INTERNAL_ERROR, NULL);        

        /* If row exceeds number of elements in table, find next table in frame that
         * still has entities */
        while ((sp >= 0) && (row >= ecs_table_count(table))) {
            table_record = find_next_table(table_set, &frame->with_ctx);

            if (table_record) {
                table = frame->table = table_record->table;
                ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
                row = frame->row = 0;
            } else {
                sp = -- op_ctx->sp;
                if (sp < 0) {
                    /* If none of the frames yielded anything, no more data */
                    return false;
                }
                frame = &op_ctx->stack[sp];
                table = frame->table;
                table_set = frame->with_ctx.table_set;
                row = ++ frame->row;
            }
        }

        int32_t row_count = ecs_table_count(table);

        /* Table must have at least row elements */
        ecs_assert(row_count > row, ECS_INTERNAL_ERROR, NULL);

        ecs_data_t *data = ecs_table_get_data(table);
        ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

        ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);
        ecs_assert(entities != NULL, ECS_INTERNAL_ERROR, NULL);

        /* The entity used to find the next table set */
        do {
            ecs_entity_t e = entities[row];

            /* Create look_for expression with the resolved entity as object */
            pair.reg_mask &= ~RULE_PAIR_OBJECT; /* turn of bit because it's not a reg */
            pair.obj = e;
            look_for = pair_to_entity(it, pair);

            /* Find table set for expression */
            table = NULL;
            table_set = frame->with_ctx.table_set = ecs_map_get_ptr(
                world->store.table_index, ecs_sparse_t*, look_for);

            /* If table set is found, find first non-empty table */
            if (table_set) {
                ecs_rule_follow_frame_t *new_frame = &op_ctx->stack[sp + 1];
                new_frame->with_ctx.table_set = NULL;
                new_frame->with_ctx.table_index = -1;
                table_record = find_next_table(table_set, &new_frame->with_ctx);

                /* If set contains non-empty table, push it to stack */
                if (table_record) {
                    table = table_record->table;
                    op_ctx->sp ++;
                    new_frame->table = table;
                    new_frame->row = 0;
                }
            }

            /* If no table was found for the current entity, advance row */
            if (!table) {
                row = frame->row ++;
            }
        } while (!table && row < row_count);

    } while (!table);

    regs[r].is.table = table;  

    return true;
}

/* Select operation. The select operation finds and iterates a table set that
 * corresponds to its pair expression. A select is often followed up by one or
 * more With operations, which apply more filters to the table. Select 
 * operations are always the 'real' first operations (excluding Input) in 
 * programs that have a root (subject) variable. */
static
bool eval_select(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    const ecs_rule_t  *rule = it->rule;
    ecs_world_t *world = rule->world;
    ecs_rule_with_ctx_t *op_ctx = &it->op_ctx[op_index].is.with;
    ecs_table_record_t *table_record = NULL;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for output */
    uint8_t r = op->r_out;
    ecs_assert(r != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param;
    ecs_entity_t look_for = pair_to_entity(it, pair);
    bool wildcard = entity_is_wildcard(look_for);

    /* If pair refers to the subject being resolved, do not treat this as a
     * wilcard expression, as it requires per-entity evaluation. Subsequent
     * from operations will take care of this. */
    if (pair_has_var(rule, pair, r)) {
        wildcard = false;
    }

    int32_t column = -1;
    ecs_table_t *table = NULL;
    ecs_sparse_t *table_set;

    /* If this is a redo, we already looked up the table set */
    if (redo) {
        table_set = op_ctx->table_set;
    
    /* If this is not a redo lookup the table set. Even though this may not be
     * the first time the operation is evaluated, variables may have changed
     * since last time, which could change the table set to lookup. */
    } else {
        /* A table set is a set of tables that all contain at least the 
         * requested look_for expression. What is returned is a table record, 
         * which in addition to the table also stores the first occurrance at
         * which the requested expression occurs in the table. This reduces (and
         * in most cases eliminates) any searching that needs to occur in a
         * table type. Tables are also registered under wildcards, which is why
         * this operation can simply use the look_for variable directly */
        table_set = op_ctx->table_set = ecs_map_get_ptr(
            world->store.table_index, ecs_sparse_t*, look_for);
    }

    /* If no table set was found for queried for entity, there are no results */
    if (!table_set) {
        return false;
    }

    int32_t *columns = get_columns(it, op_index);

    /* If this is not a redo, start at the beginning */
    if (!redo) {
        op_ctx->table_index = -1;

        /* Return the first table_record in the table set. */
        table_record = find_next_table(table_set, op_ctx);
    
        /* If no table record was found, there are no results. */
        if (!table_record) {
            return false;
        }

        table = table_record->table;

        /* Set current column to first occurrence of queried for entity */
        column = columns[op->column] = table_record->column;

        /* Store table in register */
        regs[r].is.table = table_record->table;
    
    /* If this is a redo, progress to the next match */
    } else {
        /* First test if there are any more matches for the current table, in 
         * case we're looking for a wildcard. */
        if (wildcard) {
            table = regs[r].is.table;

            ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

            column = columns[op->column];
            column = find_next_match(table->type, column + 1, look_for);

            columns[op->column] = column;
        }

        /* If no next match was found for this table, move to next table */
        if (column == -1) {
            table_record = find_next_table(table_set, op_ctx);
            if (!table_record) {
                return false;
            }

            ecs_assert(table_record != NULL, ECS_INTERNAL_ERROR, NULL);

            /* Assign new table to table register */
            table = regs[r].is.table = table_record->table;

            /* Assign first matching column */
            column = columns[op->column] = table_record->column;
        }
    }

    /* If we got here, we found a match. Table and column must be set */
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(column != -1, ECS_INTERNAL_ERROR, NULL);

    /* If this is a wildcard query, fill out the variable registers */
    if (wildcard) {
        reify_variables(it, pair, table->type, column, look_for);
    }

    ecs_entity_t *comp = ecs_vector_get(table->type, ecs_entity_t, column);
    ecs_assert(comp != NULL, ECS_INTERNAL_ERROR, NULL);
    it->table.components[op->column] = *comp;

    return true;
}

static
ecs_table_t* table_from_entity(
    ecs_world_t *world,
    ecs_entity_t e)
{
    ecs_record_t *record = ecs_eis_get(world, e);
    if (record) {
        return record->table;
    } else {
        return NULL;
    }
}

static
ecs_table_t* table_from_reg(
    const ecs_rule_t *rule,
    ecs_rule_op_t *op,
    ecs_rule_reg_t *regs,
    uint8_t r)
{
    if (r == UINT8_MAX) {
        ecs_assert(op->subject != 0, ECS_INTERNAL_ERROR, NULL);
        return table_from_entity(rule->world, op->subject);
    }
    if (rule->variables[r].kind == EcsRuleVarKindTable) {
        return regs[r].is.table;
    }
    if (rule->variables[r].kind == EcsRuleVarKindEntity) {
        return table_from_entity(rule->world, regs[r].is.entity);
    } 
    return NULL;
}

/* With operation. The With operation always comes after either the Select or
 * another With operation, and applies additional filters to the table. */
static
bool eval_with(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    const ecs_rule_t *rule = it->rule;
    ecs_world_t *world = rule->world;
    ecs_rule_with_ctx_t *op_ctx = &it->op_ctx[op_index].is.with;
    ecs_table_record_t *table_record = NULL;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for input */
    uint8_t r = op->r_in;

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param;
    ecs_entity_t look_for = pair_to_entity(it, pair);
    bool wildcard = entity_is_wildcard(look_for);

    /* If pair refers to the subject being resolved, do not treat this as a
     * wilcard expression, as it requires per-entity evaluation. Subsequent
     * from operations will take care of this. */
    if (pair_has_var(rule, pair, r)) {
        wildcard = false;
    }    

    /* If looked for entity is not a wildcard (meaning there are no unknown/
     * unconstrained variables) and this is a redo, nothing more to yield. */
    if (redo && !wildcard) {
        return false;
    }

    int32_t column = -1;
    ecs_table_t *table = NULL;
    ecs_sparse_t *table_set;    

    /* If this is a redo, we already looked up the table set */
    if (redo) {
        table_set = op_ctx->table_set;
    
    /* If this is not a redo lookup the table set. Even though this may not be
     * the first time the operation is evaluated, variables may have changed
     * since last time, which could change the table set to lookup. */
    } else {
        /* The With operation finds the table set that belongs to its pair
         * filter. The table set is a sparse set that provides an O(1) operation
         * to check whether the current table has the required expression. */
        table_set = op_ctx->table_set = ecs_map_get_ptr(
            world->store.table_index, ecs_sparse_t*, look_for);
    }

    /* If no table set was found for queried for entity, there are no results */
    if (!table_set) {
        return false;
    }

    int32_t *columns = get_columns(it, op_index);

    /* If this is not a redo, start at the beginning */
    if (!redo) {
        table = table_from_reg(rule, op, regs, r);
        if (!table) {
            return false;
        }

        /* Try to find the table in the table set by the table id. If the table
         * cannot be found in the table set, the table does not have the
         * required expression. This is a much faster way to do this check than
         * iterating the table type, and makes rules that request lots of
         * components feasible to execute in realtime. */
        table_record = ecs_sparse_get_sparse(
            table_set, ecs_table_record_t, table->id);

        /* If no table record was found, there are no results. */
        if (!table_record) {
            return false;
        }
        
        ecs_assert(table == table_record->table, ECS_INTERNAL_ERROR, NULL);

        /* Set current column to first occurrence of queried for entity */
        column = columns[op->column] = table_record->column;
    
    /* If this is a redo, progress to the next match */
    } else {
        /* First test if there are any more matches for the current table, in 
         * case we're looking for a wildcard. */
        if (wildcard) {
            table = table_from_reg(rule, op, regs, r);
            if (!table) {
                return NULL;
            }

            /* Find the next match for the expression in the column. The columns
             * array keeps track of the state for each With operation, so that
             * even after redoing a With, the search doesn't have to start from
             * the beginning. */
            column = columns[op->column];
            column = find_next_match(table->type, column + 1, look_for);
            columns[op->column] = column;
        }

        /* If no next match was found for this table, no more data */
        if (column == -1) {
            return false;
        }
    }

    /* If we got here, we found a match. Table and column must be set */
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(column != -1, ECS_INTERNAL_ERROR, NULL);

    /* If this is a wildcard query, fill out the variable registers */
    if (wildcard) {
        reify_variables(it, pair, table->type, column, look_for);
    }

    ecs_entity_t *comp = ecs_vector_get(table->type, ecs_entity_t, column);
    ecs_assert(comp != NULL, ECS_INTERNAL_ERROR, NULL);
    it->table.components[op->column] = *comp;    

    return true;
}

/* Each operation. The each operation is a simple operation that takes a table
 * as input, and outputs each of the entities in a table. This operation is
 * useful for rules that match a table, and where the entities of the table are
 * used as predicate or object. If a rule contains an each operation, an
 * iterator is guaranteed to yield an entity instead of a table. The input for
 * an each operation can only be the root variable. */
static
bool eval_each(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    ecs_rule_each_ctx_t *op_ctx = &it->op_ctx[op_index].is.each;
    ecs_rule_reg_t *regs = get_registers(it, op_index);
    uint8_t r_in = op->r_in;
    uint8_t r_out = op->r_out;
    int32_t row;

    /* Make sure in/out registers are of the correct kind */
    ecs_assert(it->rule->variables[r_in].kind == EcsRuleVarKindTable, 
        ECS_INTERNAL_ERROR, NULL);
    ecs_assert(it->rule->variables[r_out].kind == EcsRuleVarKindEntity, 
        ECS_INTERNAL_ERROR, NULL);

    /* Get table, make sure that it contains data. The select operation should
     * ensure that empty tables are never forwarded. */
    ecs_table_t *table = regs[r_in].is.table;
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_data_t *data = ecs_table_get_data(table);
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

    int32_t count = ecs_table_data_count(data);
    ecs_assert(count != 0, ECS_INTERNAL_ERROR, NULL);

    ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);
    ecs_assert(entities != NULL, ECS_INTERNAL_ERROR, NULL);

    /* If this is is not a redo, start from row 0, otherwise go to the
     * next entity. */
    if (!redo) {
        row = op_ctx->row = 0;
    } else {
        row = ++ op_ctx->row;
    }

    /* If row exceeds number of entities in table, return false */
    if (row >= count) {
        return false;
    }

    /* Skip builtin entities that could confuse operations */
    ecs_entity_t e = entities[row];
    while (e == EcsWildcard || e == EcsThis) {
        row ++;
        if (row == count) {
            return false;
        }
        e = entities[row];      
    }

    /* Assign entity */
    regs[r_out].is.entity = e;

    return true;
}

/* Yield operation. This is the simplest operation, as all it does is return
 * false. This will move the solver back to the previous instruction which
 * forces redo's on previous operations, for as long as there are matching
 * results. */
static
bool eval_yield(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    /* Yield always returns false, because there are never any operations after
     * a yield. */
    return false;
}

/* Dispatcher for operations */
static
bool eval_op(
    ecs_rule_iter_t *it, 
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    switch(op->kind) {
    case EcsRuleInput:
        return eval_input(it, op, op_index, redo);
    case EcsRuleFollow:
        return eval_follow(it, op, op_index, redo);
    case EcsRuleSelect:
        return eval_select(it, op, op_index, redo);
    case EcsRuleWith:
        return eval_with(it, op, op_index, redo);                
    case EcsRuleEach:
        return eval_each(it, op, op_index, redo);
    case EcsRuleYield:
        return eval_yield(it, op, op_index, redo);            
    default:
        return false;
    }
}

/* Utility to copy all registers to the next frame. Keeping track of register
 * values for each operation is necessary, because if an operation is asked to
 * redo matching, it must to be able to pick up from where it left of */
static
void push_registers(
    ecs_rule_iter_t *it,
    int32_t cur,
    int32_t next)
{
    ecs_rule_reg_t *src_regs = get_registers(it, cur);
    ecs_rule_reg_t *dst_regs = get_registers(it, next);

    memcpy(dst_regs, src_regs, 
        ECS_SIZEOF(ecs_rule_reg_t) * it->rule->variable_count);
}

/* Utility to copy all columns to the next frame. Columns keep track of which
 * columns are currently being evaluated for a table, and are populated by the
 * Select and With operations. The columns array is important, as it is used
 * to tell the application where to find component data. */
static
void push_columns(
    ecs_rule_iter_t *it,
    int32_t cur,
    int32_t next)
{
    int32_t *src_cols = get_columns(it, cur);
    int32_t *dst_cols = get_columns(it, next);

    memcpy(dst_cols, src_cols, ECS_SIZEOF(int32_t) * it->rule->column_count);
}

static
void set_iter_table(
    ecs_iter_t *iter,
    ecs_table_t *table,
    int32_t cur)
{
    ecs_rule_iter_t *it = &iter->iter.rule;

    ecs_data_t *data = ecs_table_get_data(table);

    /* Table must have data, or otherwise it wouldn't yield */
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

    /* Tell the iterator how many entities there are */
    iter->count = ecs_table_data_count(data);
    ecs_assert(iter->count != 0, ECS_INTERNAL_ERROR, NULL);

    /* Set the entities array */
    iter->entities = ecs_vector_first(data->entities, ecs_entity_t);
    ecs_assert(iter->entities != NULL, ECS_INTERNAL_ERROR, NULL);

    /* Set table parameters */
    it->table.columns = get_columns(it, cur);
    it->table.data = data;
    iter->table = &it->table;
    iter->table_columns = data->columns;

    ecs_assert(it->table.components != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(it->table.columns != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(table->type != NULL, ECS_INTERNAL_ERROR, NULL);

    /* Iterator expects column indices to start at 1. Can safely
     * modify the column ids, since the array is private to the
     * yield operation. */
    for (int i = 0; i < iter->column_count; i ++) {
        it->table.columns[i] ++;
    }    
}

/* Iterator next function. This evaluates the program until it reaches a Yield
 * operation, and returns the intermediate result(s) to the application. An
 * iterator can, depending on the program, either return a table, entity, or
 * just true/false, in case a rule doesn't contain the this variable. */
bool ecs_rule_next(
    ecs_iter_t *iter)
{
    ecs_rule_iter_t *it = &iter->iter.rule;
    const ecs_rule_t *rule = it->rule;
    bool redo = it->redo;

    do {
        int16_t cur = it->op;
        ecs_rule_op_t *op = &rule->operations[cur];

        /* Evaluate an operation. The result of an operation determines the
         * flow of the program. If an operation returns true, the program 
         * continues to the operation pointed to by 'on_ok'. If the operation
         * returns false, the program continues to the operation pointed to by
         * 'on_fail'.
         *
         * In most scenarios, on_ok points to the next operation, and on_fail
         * points to the previous operation.
         *
         * When an operation fails, the previous operation will be invoked with
         * redo=true. This will cause the operation to continue its search from
         * where it left off. When the operation succeeds, the next operation
         * will be invoked with redo=false. This causes the operation to start
         * from the beginning, which is necessary since it just received a new
         * input. */
        bool result = eval_op(it, op, cur, redo);

        /* Operation matched */
        if (result) {
            int16_t next = it->op = op->on_ok;

            /* Can never reach end of the sequence as result of a match */
            ecs_assert(next != -1, ECS_INTERNAL_ERROR, NULL);

            /* Push registers for next op */
            push_registers(it, cur, next);
            push_columns(it, cur, next);

            redo = false;

        /* Operation didn't match */
        } else {
            it->op = op->on_fail;
            redo = true;
        }

        /* If the current operation is yield, return results */
        if (op->kind == EcsRuleYield) {
            uint8_t r = op->r_in;

            /* If the input register for the yield does not point to a variable,
             * the rule doesn't contain a this (.) variable. In that case, the
             * iterator doesn't contain any data, and this function will simply
             * return true or false. An application will still be able to obtain
             * the variables that were resolved. */
            if (r == UINT8_MAX) {
                iter->count = 0;
            } else {
                ecs_rule_var_t *var = &rule->variables[r];
                ecs_rule_reg_t *regs = get_registers(it, cur);
                ecs_rule_reg_t *reg = &regs[r];

                if (var->kind == EcsRuleVarKindTable) {
                    ecs_table_t *table = reg->is.table;
                    set_iter_table(iter, table, cur);
                } else {
                    /* If a single entity is returned, simply return the
                     * iterator with count 1 and a pointer to the entity id */
                    ecs_assert(var->kind == EcsRuleVarKindEntity, 
                        ECS_INTERNAL_ERROR, NULL);
                    ecs_entity_t e = reg->is.entity;
                    ecs_record_t *record = ecs_eis_get(rule->world, e);

                    /* If an entity is not stored in a table, it could not have
                     * been matched by anything */
                    ecs_assert(record != NULL, ECS_INTERNAL_ERROR, NULL);
                    set_iter_table(iter, record->table, cur);
                    iter->count = 1;

                    bool is_monitored;
                    iter->offset = ecs_record_to_row(
                        record->row, &is_monitored);
                }
            }

            it->redo = redo;

            return true;
        }
    } while ((it->op != -1));

    ecs_rule_iter_free(iter);

    return false;
}
