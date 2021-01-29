#include "private_api.h"

#define ECS_RULE_MAX_VARIABLE_COUNT (256)

/* A rule pair contains a type and subject that can be stored in a register. */
typedef struct ecs_rule_pair_t {
    uint32_t pred;
    uint32_t subj;
    int8_t reg_mask; /* bit 1 = role, bit 2 = subject */
} ecs_rule_pair_t;

/* A rule param specifies what With/Match operations should filter on */
typedef enum ecs_rule_param_kind_t {
    EcsRuleParamPair,    /* Single pair */
    EcsRuleParamType     /* Type that contains N pairs */
} ecs_rule_param_kind_t;

typedef struct ecs_rule_param_t {
    ecs_rule_param_kind_t kind;
    union {
        ecs_rule_pair_t entity;
        ecs_vector_t *type;
    } is;
} ecs_rule_param_t;

/* A rule register stores temporary values for rule variables */
typedef enum ecs_rule_var_kind_t {
    EcsRuleVarKindUnknown,
    EcsRuleVarKindEntity,
    EcsRuleVarKindType,
    EcsRuleVarKindTable,
    EcsRuleVarKindLiteral
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
    EcsRuleInput,
    EcsRuleSelect,
    EcsRuleWith,
    EcsRuleEach,
    EcsRuleFrom,
    EcsRuleYield
} ecs_rule_op_kind_t;

/* Single operation */
typedef struct ecs_rule_op_t {
    ecs_rule_op_kind_t kind;    /* What kind of operation is it */
    ecs_rule_param_t param;     /* Parameter that contains optional filter */
    ecs_entity_t object;        /* If set, operation has a constant object */

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
        ecs_rule_with_ctx_t with;
        ecs_rule_each_ctx_t each;
        ecs_rule_from_ctx_t from;
    } is;
} ecs_rule_op_ctx_t;

/* Rule variables allow for the rule to be parameterized */
typedef struct ecs_rule_var_t {
    ecs_rule_var_kind_t kind;
    const char *name; /* Variable name */
    uint8_t id;       /* Unique variable id */
    uint8_t occurs;   /* Number of occurrences (used for operation ordering) */
    uint8_t depth;  /* Depth in dependency tree (used for operation ordering) */
} ecs_rule_var_t;

/* Top-level rule datastructure */
struct ecs_rule_t {
    ecs_world_t *world;         /* Ref to world so rule can be used by itself */
    ecs_rule_op_t *operations;  /* Operations array */
    ecs_rule_var_t *variables;  /* Variable array */
    ecs_sig_t sig;              /* Parsed signature expression */

    uint8_t variable_count;     /* Number of variables in signature */
    uint8_t column_count;       /* Number of columns in signature */
    int16_t operation_count;    /* Number of operations in rule */
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
    var->name = ecs_os_strdup(name);
    var->kind = kind;

    /* The variable id is the location in the variable array and also points to
     * the register element that corresponds with the variable. */
    var->id = cur - 1;

    /* Depth is used to calculate how far the variable is from the root, where
     * the root is the variable with 0 dependencies. */
    var->depth = UINT8_MAX;
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
    ecs_rule_t *rule,
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

    /* Columns must always have at least one argument (the object) */
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
        result.reg_mask |= 1;
    } else {
        /* If the predicate is not a variable, simply store its id. */
        result.pred = pred_id;
    }

    /* The pair doesn't do anything with the object (objects are the things that
     * are matched against pairs) so if the column does not have a subject, 
     * there is nothing left to do. */
    if (column->argc == 1) {
        return result;
    }

    /* If arguments is higher than 2 this is not a pair but a nested rule */
    ecs_assert(column->argc == 2, ECS_INTERNAL_ERROR, NULL);

    ecs_entity_t subj_id = column->argv[1].entity;

    /* Same as above, if the subject is a variable, store it and flag it */
    if (!subj_id || subj_id == EcsThis) {
        const ecs_rule_var_t *var = find_variable(
            rule, EcsRuleVarKindEntity, column->argv[1].name);

        /* Variables should have been declared */
        ecs_assert(var != NULL, ECS_INTERNAL_ERROR, NULL);
        ecs_assert(var->kind == EcsRuleVarKindEntity, ECS_INTERNAL_ERROR, NULL);

        result.subj = var->id;
        result.reg_mask |= 2;
    } else {
        /* If the subject is not a variable, simply store its id */
        result.subj = subj_id;
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
    ecs_entity_t subj = pair.subj;

    /* Get registers in case we need to resolve ids from registers. Get them
     * from the previous, not the current stack frame as the current operation
     * hasn't reified its variables yet. */
    ecs_rule_reg_t *regs = get_registers(it, it->op - 1);

    if (pair.reg_mask & 1) {
        pred = regs[pred].is.entity;
    }
    if (pair.reg_mask & 2) {
        subj = regs[subj].is.entity;
    }

    if (!subj) {
        return pred;
    } else {
        return ecs_trait(subj, pred);
    }
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

    /* If this is a trait, the wildcard can be either the type or subject */
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
        if (pair.reg_mask & 1) {
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

        /* If subject is a wildcard, this is guaranteed to be a trait */
        if (pair.reg_mask & 2) {
            ecs_assert(ECS_HAS_ROLE(look_for, TRAIT), ECS_INTERNAL_ERROR, NULL);

            /* Same as above, if subject is not a wildcard it could already have
             * been resolved by either input or a previous operation. */
            if (ecs_entity_t_lo(look_for) == EcsWildcard) {
                ecs_assert(vars[pair.subj].kind == EcsRuleVarKindEntity, ECS_INTERNAL_ERROR, NULL);
                regs[pair.subj].is.entity = ecs_entity_t_lo(*elem);
            }
        }
    }
}

/* Find the depth of the dependency tree from the variable to the root */
static
uint8_t get_variable_depth(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    uint8_t root,
    int recur)
{
    bool is_this = !strcmp(var->name, ".");

    /* If we hit the variable limit while recursing, that means that there is a
     * cycle in the variable dependencies that does not include the root. This
     * indicates that there is a disjoint set of variables in the rule which
     * is not valid. */
    if(recur >= ECS_RULE_MAX_VARIABLE_COUNT) {
        rule_error(rule, "invalid isolated variable '%s'", var->name);
        return UINT8_MAX;
    }

    /* Iterate columns, find all instances where 'var' is not used as object.
     * If the object of that column is either the root or a variable for which
     * the depth is known, the depth for this variable can be determined. */
    ecs_sig_column_t *columns = ecs_vector_first(rule->sig.columns, ecs_sig_column_t);
    int32_t i, count = rule->column_count;
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];
        bool var_found = false;

        /* Test if type is variable */
        if (!column->pred.entity) {
            if (find_variable(rule, EcsRuleVarKindUnknown, column->pred.name) == var) {
                var_found = true;
            }
        }

        /* Test if the subject is the variable */
        if (!var_found && column->argc > 1) {
            if (!column->argv[1].entity) {
                if (find_variable(rule, EcsRuleVarKindUnknown, column->argv[1].name) == var) {
                    var_found = true;
                }
            } else if (is_this && column->argv[i].entity == EcsThis) {
                var_found = true;
            }
        }

        /* If the variable was not found in either type or subject, there is no
         * (direct) dependency relationship in this column for the variable. */
        if (!var_found) {
            continue;
        }

        /* If a variable was found, resolve the object of the expression */
        ecs_rule_var_t *obj = find_variable(
            rule, EcsRuleVarKindUnknown, column->argv[0].name);

        /* All variables that appear as objects must be defined at this time */
        ecs_assert(obj != NULL, ECS_INTERNAL_ERROR, NULL);

        /* If obj is the root, this is the lowest depth that we'll get for this
         * variable, so stop searching. */
        if (obj->id == root) {
            return var->depth = 1;
        }

        /* If the object depth has not yet been set, resolve it recursively */
        uint8_t depth = obj->depth;
        if (depth == UINT8_MAX) {
            depth = get_variable_depth(rule, obj, root, recur + 1);
            if (depth == UINT8_MAX) {
                /* Infinite recursion detected */
                return UINT8_MAX;
            }
        }

        if (depth < var->depth) {
            var->depth = depth + 1;
        }
    }

    /* The depth should have been resolved when we get here. If it hasn't been
     * resolved the variable has no relationship with the root. */
    if (var->depth == UINT8_MAX) {
        rule_error(rule, "invalid isolated variable '%s'", var->name);
        return UINT8_MAX;
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

    if (v1->depth < v2->depth) {
        return -1;
    } else if (v1->depth > v2->depth) {
        return 1;
    }

    return (v1->occurs < v2->occurs) - (v1->occurs > v2->occurs);
}

/* Scan for variables, put them in optimal dependency order. */
static
int scan_variables(
    ecs_rule_t *rule)
{
    /* Objects found in rule. One will be elected root */
    uint8_t objects[ECS_RULE_MAX_VARIABLE_COUNT] = {0};
    uint16_t object_count = 0;

    /* If this (.) is found, it always takes precedence in root election */
    uint8_t this_var = UINT8_MAX;

    /* Keep track of the object variable that occurs the most. In the absence of
     * this (.) the variable with the most occurrences will be elected root. */
    uint8_t max_occur = 0;
    uint8_t max_occur_var = UINT8_MAX;

    /* Step 1: find all possible roots */
    ecs_sig_column_t *columns = ecs_vector_first(rule->sig.columns, ecs_sig_column_t);
    int32_t i, count = rule->column_count;
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        /* Evaluate the object. The predicate and subject are not evaluated, 
         * since they never can be elected as root. */
        if (!column->argv[0].entity || column->argv[0].entity == EcsThis) {
            const char *obj_name = column->argv[0].name;

            ecs_rule_var_t *obj = find_variable(
                rule, EcsRuleVarKindUnknown, obj_name);
            if (!obj) {
                obj = create_variable(rule, EcsRuleVarKindUnknown, obj_name);
                objects[object_count ++] = obj->id;
                if (object_count >= ECS_RULE_MAX_VARIABLE_COUNT) {
                    rule_error(rule, "too many variables in rule");
                    goto error;
                }
            }

            if (++ obj->occurs > max_occur) {
                max_occur = obj->occurs;
                max_occur_var = obj->id;
            }

            /* If this (.) is used as an object, it always takes precedence
             * when electing a root. */
            if (!strcmp(obj_name, ".")) {
                this_var = obj->id;
            }
        }
    }

    /* Step 2: elect a root. This is either this (.) or the variable with the
     * most occurrences. */
    uint8_t root_var = this_var;
    if (root_var == UINT8_MAX) {
        root_var = max_occur_var;
        if (root_var == UINT8_MAX) {
            /* If no object variables have been found, the rule expression only
             * operates on a fixed set of entities, in which case no root 
             * election is required. */
            goto done;
        }
    }

    /* Assign the depth of the root variable to 0 */
    rule->variables[root_var].depth = 0;

    /* Step 3: now that we have a root, we can determine the depth for each
     * object to the root. This is used for ordering, as variables closer to the
     * root will be evaluated (and resolved) first. Additionally, this also 
     * serves as a validation check to ensure that the rule does not contain
     * disjoint sets of variables that are not related to the root. Such rules
     * are considered invalid as they would essentially represent an 
     * unconstrained join between rules, which would yield every possible
     * combination of results from each disjoint rule. This doesn't seem very
     * useful and is more likely the result of an error than anything else. */
    for (i = 0; i < object_count; i ++) {
        if (objects[i] == root_var) {
            /* We already know that the depth of the root is 0 */
            continue;
        }

        ecs_rule_var_t *var = &rule->variables[objects[i]];
        var->depth = get_variable_depth(rule, var, root_var, 0);
        if (var->depth == UINT8_MAX) {
            /* Found a disjoint set of variables, which is invalid */
            goto error;
        }
    }

    /* Step 4: order variables by depth, followed by occurrence. The variable
     * array will later be used to lead the iteration over the columns, and
     * determine which operations get inserted first. */
    qsort(rule->variables, rule->variable_count, sizeof(ecs_rule_var_t), 
        compare_variable);

done:
    return 0;
error:
    return -1;
}

/* After all object variables have been found, inserted and sorted, the 
 * remaining variables (predicate & subject) still need to be inserted. This
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

        /* If subject is a variable, make sure it has been registered */
        if (column->argc > 1 && (!column->argv[1].entity || 
            column->argv[1].entity == EcsThis)) 
        {
            ensure_variable(rule, EcsRuleVarKindEntity, column->argv[1].name);
        }        
    }    
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
    int32_t c, column_count = ecs_vector_count(sig->columns);

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

    /* Store the number of variables that have been inserted so far, as this
     * represents the list of object variables for which operations have to be
     * inserted first. */
    int32_t v, var_count = result->variable_count;

    /* If any variables have been registered so far, the rule expression has
     * variables as objects (which is the normal case), which means this 
     * expression can leverage table-based iteration. Ensure that the root 
     * variable (which is guaranteed to be the first one in the list) is 
     * registered with a table type. This will not create a new variable, as
     * up to this point variables have been registered with an unknown type
     * which will be overwritten as soon as an actual type is provided. */
    bool has_root;
    if (result->variables) {
        result->variables[0].kind = EcsRuleVarKindTable;
        has_root = true;
    }

    /* The remainder of the variables are entities */
    for (v = 1; v < var_count; v ++) {
        result->variables[v].kind = EcsRuleVarKindEntity;
    }

    /* Scan expression for remainder of variables that are not used as objects.
     * This ensures that all variables are known, and that the array won't be
     * reallocated as operations are inserted, which simplifies the code. */
    ensure_all_variables(result);

    /* Test if the root variable is registered as both table and entity. If this
     * is the case, the rule needs to insert an each expression before the first
     * non-select or with operation. */
    bool root_as_entity = false, root_entity_inserted = false;
    ecs_rule_var_t *root_var = NULL;

    if (has_root) {
        root_var = &result->variables[0];
        ecs_assert(root_var->kind == EcsRuleVarKindTable, ECS_INTERNAL_ERROR, NULL);
        if (find_variable(result, EcsRuleVarKindEntity, root_var->name)) {
            root_as_entity = true;
        }
    }

    /* Literal placeholder, this is used for operations that do not have a 
     * variable for an object. */
    ecs_rule_var_t var_literal = {
        .kind = EcsRuleVarKindEntity,
        .id = UINT8_MAX /* This acts as an indicator for a constant object */
    };

    /* Iterate variables front to back, and insert operations that have the
     * iterated over variable as object. Variables are stored in dependency
     * order, and by storing operations in the same order it is guaranteed that
     * variables are resolved in optimal order. 
     * At this point the array with variables only contains the variables that
     * appear as objects in the expression. */
    for (v = 0; v < var_count + 1; v ++) {
        for (c = 0; c < column_count; c ++) {
            ecs_rule_var_t *var = NULL;
            
            if (v < var_count) {
                var = &result->variables[v];
            }

            ecs_sig_column_t *column = &columns[c];

            /* Skip operation if this is not the variable for which operations
             * are currently being inserted. */
            if (var && strcmp(column->argv[0].name, var->name)) {
                continue;
            }

            /* Skip operation if this is not for a variable, and column has a
             * ariable as object. */
            if (!var && (!column->argv[0].entity || (column->argv[0].entity == EcsThis))) {
                continue;
            }

            /* If this is an operation for which the object is not a variable,
             * use the literal placeholder. This makes it easier to reuse the
             * same code generator. */
            if (!var) {
                var = &var_literal;
            }

            /* If the object variable is not a table, it means that all the
             * select and with operations have been inserted. If the rule
             * requires iterating over the entities in a table, insert an each
             * instruction so that each entity is iterated over. */
            if (var->kind != EcsRuleVarKindTable && root_as_entity && 
                !root_entity_inserted) 
            {
                /* Find the entity variable for the root */
                ecs_rule_var_t *root_entity_var = find_variable(result, 
                    EcsRuleVarKindEntity, root_var->name);

                op = create_operation(result);
                op->kind = EcsRuleEach;
                op->r_in = root_var->id; /* Input is the root table */
                op->r_out = root_entity_var->id; /* Output is entity var */
                op->on_ok = result->operation_count;
                op->on_fail = result->operation_count - 2;
                op->has_in = true;
                op->has_out = true;

                /* Once inserted we won't need to do it again as there can only
                 * be one table variable. */
                root_entity_inserted = true;
            }

            /* Insert operation */
            op = create_operation(result);            

            /* If the variable is of type table, insert Select/With. The Select
             * operation yields an initial table that matches a provided filter.
             * Subsequent With expressions filter the tables by testing it 
             * against their expression. Select/With operations always appear at
             * the start of a program, and always in a single chain. */
            if (var->kind == EcsRuleVarKindTable) {
                /* If this is the first operation after Input, insert Select */
                if (result->operation_count == 2) {
                    op->kind = EcsRuleSelect;
                    op->r_out = var->id; /* Write table to register */
                    op->has_out = true;

                    /* When selecting a table, the variable id should not be a 
                    * constant, as tables are not directly encoded in a rule. */
                    ecs_assert(op->r_out != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);

                /* If this is not the first operation, insert a With. The With
                 * will read the output of Select, and apply its filter. If the
                 * filter passes, the program will resume to the next operation.
                 * If the filter fails, the program will redo the previous 
                 * instruction, which if this were a Select would yield the next
                 * table. The With operation does not output anything as
                 * subsequent operations can reuse the same input register. */
                } else {
                    op->kind = EcsRuleWith;
                    op->r_in = var->id; /* Read table from register */

                    /* When selecting a table, the variable id should not be a 
                    * constant, as tables are not directly encoded in a rule. */
                    ecs_assert(op->r_in != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);                    
                }

            /* If this variable is not of type table, it operates on a single
             * entity. The From operation retrieves the type of the entity and
             * applies its filter to it. */
            } else {
                op->kind = EcsRuleFrom;
                op->r_in = var->id;
                op->has_in = true;

                /* If this is a constant object, copy entity identifier of 
                 * object to the object so the operation doesn't have to look 
                 * it up from the column. */
                if (op->r_in == UINT8_MAX) {
                    op->object = column->argv[0].entity;

                    /* Something's wrong if the column doesn't have an object */
                    ecs_assert(op->object != 0, ECS_INTERNAL_ERROR, NULL);
                }

                /* Variable is an entity */
                var->kind = EcsRuleVarKindEntity;
            }

            /* Parse the column's type into a pair. A pair extracts the ids from
             * the column, and replaces variables with wildcards which can then
             * be matched against actual relationships. A pair retains the 
             * information about the variables, so that when a match happens,
             * the pair can be used to reify the variable. */
            op->param.kind = EcsRuleParamPair;
            op->param.is.entity = column_to_pair(result, column);

            /* The on_ok and on_fail labels point to the operations that the
             * program should execute if the operation succeeds or fails. */
            op->on_ok = result->operation_count;
            op->on_fail = result->operation_count - 2;  

            /* Store corresponding signature column so we can correlate and
             * store the table columns with signature columns. */
            op->column = c;
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
        ecs_rule_pair_t pair = op->param.is.entity;
        ecs_entity_t type = pair.pred;
        ecs_entity_t subject = pair.subj;
        const char *type_name, *subject_name;

        if (pair.reg_mask & 1) {
            ecs_rule_var_t *type_var = &rule->variables[type];
            type_name = type_var->name;
        } else {
            type_name = ecs_get_name(rule->world, type);
        }

        if (subject) {
            if (pair.reg_mask & 2) {
                ecs_rule_var_t *subj_var = &rule->variables[subject];;
                subject_name = subj_var->name;
            } else {
                subject_name = ecs_get_name(rule->world, subject);
            }
        }

        ecs_strbuf_append(&buf, "%d: [Pass:%d, Fail:%d] ", i, 
            op->on_ok, op->on_fail);

        bool has_filter = false;

        switch(op->kind) {
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
        case EcsRuleFrom:
            ecs_strbuf_append(&buf, "from  ");
            has_filter = true;
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
                    r_in->kind == EcsRuleVarKindTable ? "t" : "e",
                    r_in->name);
            } else if (op->object) {
                ecs_strbuf_append(&buf, " %s", 
                    ecs_get_name(rule->world, op->object));
            }
        }

        if (op->has_out) {
            ecs_rule_var_t *r_out = get_variable(rule, op->r_out);
            if (r_out) {
                ecs_strbuf_append(&buf, " > %s%s", 
                    r_out->kind == EcsRuleVarKindTable ? "t" : "e",
                    r_out->name);
            } else if (op->object) {
                ecs_strbuf_append(&buf, " > %s <- ", 
                    ecs_get_name(rule->world, op->object));
            }
        }

        if (has_filter) {
            if (!subject) {
                sprintf(filter_expr, "(%s)", type_name);
            } else {
                sprintf(filter_expr, "(%s, %s)", type_name, subject_name);
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

/* Public function to get the name of a variable. */
const char* ecs_rule_variable_name(
    const ecs_rule_t *rule,
    int32_t var_id)
{
    return rule->variables[var_id].name;
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
    ecs_iter_t result;
    result.world = rule->world;

    ecs_rule_iter_t *it = &result.iter.rule;
    it->rule = rule;
    it->registers = ecs_os_malloc(rule->operation_count * rule->variable_count * ECS_SIZEOF(ecs_rule_reg_t));
    it->op_ctx = ecs_os_malloc(rule->operation_count * ECS_SIZEOF(ecs_rule_op_ctx_t));
    it->columns = ecs_os_malloc(rule->operation_count * rule->column_count * ECS_SIZEOF(int32_t));
    it->op = 0;

    int i;
    for (i = 0; i < rule->variable_count; i ++) {
        it->registers[i].is.entity = EcsWildcard;
    }

    return result;
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
        table_record = ecs_sparse_get(
            table_set, ecs_table_record_t, op_ctx->table_index);
        if (!table_record) {
            return NULL;
        }

        count = ecs_table_count(table_record->table);
        if (!count) {
            op_ctx->table_index ++;
        }
    } while (!count);

    /* Paranoia check */
    ecs_assert(ecs_table_count(table_record->table) != 0, 
        ECS_INTERNAL_ERROR, NULL);

    return table_record;
}

/* Select operation. The select operation finds and iterates a table set that
 * corresponds to its pair expression. A select is often followed up by one or
 * more With operations, which apply more filters to the table. Select 
 * operations are always the 'real' first operations (excluding Input) in 
 * programs that have a root (object) variable. */
static
bool eval_select(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    ecs_world_t *world = it->rule->world;
    ecs_rule_with_ctx_t *op_ctx = &it->op_ctx[op_index].is.with;
    ecs_table_record_t *table_record = NULL;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for output */
    uint8_t r = op->r_out;
    ecs_assert(r != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param.is.entity;
    ecs_entity_t look_for = pair_to_entity(it, pair);
    bool wildcard = entity_is_wildcard(look_for);

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
        op_ctx->table_index = 0;

        /* Return the first table_record in the table set. A table record stores*/
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
            op_ctx->table_index ++;

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

    return true;    
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
    ecs_world_t *world = it->rule->world;
    ecs_rule_with_ctx_t *op_ctx = &it->op_ctx[op_index].is.with;
    ecs_table_record_t *table_record = NULL;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for input */
    uint8_t r = op->r_in;
    ecs_assert(r != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param.is.entity;
    ecs_entity_t look_for = pair_to_entity(it, pair);
    bool wildcard = entity_is_wildcard(look_for);

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
        table = regs[r].is.table;
        ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

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
            table = regs[r].is.table;

            ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

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

    return true;
}

/* Each operation. The each operation is a simple operation that takes a table
 * as input, and outputs each of the entities in a table. This operation is
 * useful for rules that match a table, and where the entities of the table are
 * used as predicate or subject. If a rule contains an each operation, an
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

    /* Simple: if this is is not a redo, start from row 0, otherwise go to the
     * next entity. */
    if (!redo) {
        row = op_ctx->row = 0;
    } else {
        row = ++ op_ctx->row;
    }

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

    /* If row exceeds number of entities in table, return false */
    if (row >= count) {
        return false;
    }

    ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);
    ecs_assert(entities != NULL, ECS_INTERNAL_ERROR, NULL);

    /* Assign entity */
    regs[r_out].is.entity = entities[row];

    return true;
}

/* From operation. The from operation searches an entity type for a provided
 * filter. Currently this is an O(n) search as entity types usually contain a
 * small number of elements and iterating a regular array is fast, but this can
 * be easily optimized by using the same table index as Select and With use, by
 * looking up the table set for the filter, and testing whether the table of the
 * evaluated entity is part of the table set, which is guaranteed O(1). */
static
bool eval_from(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int16_t op_index,
    bool redo)
{
    const ecs_rule_t *rule = it->rule;
    ecs_world_t *world = it->rule->world;
    ecs_type_t type = NULL;
    int32_t column = -1;
    ecs_rule_from_ctx_t *op_ctx = &it->op_ctx[op_index].is.from;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* If this is not a redo, get type from input */
    if (!redo) {
        uint8_t r_in = op->r_in;
        
        /* If the operation has a constant object, get its type */
        if (r_in == UINT8_MAX) {
            ecs_assert(op->object != 0, ECS_INTERNAL_ERROR, NULL);
            type = ecs_get_type(world, op->object);

        /* If the operation does not have a constant object, get the type from
         * the input variable. */
        } else {
            ecs_rule_var_kind_t reg_kind = rule->variables[r_in].kind;
            switch(reg_kind) {
            case EcsRuleVarKindEntity: {
                ecs_entity_t e = regs[r_in].is.entity;
                type = ecs_get_type(world, e);
                break;
            }
            case EcsRuleVarKindType:
                type = regs[r_in].is.type;
                break;
            case EcsRuleVarKindTable:
                type = regs[r_in].is.table->type;
                break;
            default:
                /* Should never get here */
                ecs_abort(ECS_INTERNAL_ERROR, NULL);
                break;
            }
        }

        op_ctx->type = type;
        column = op_ctx->column = 0;

    /* If this is a redo, continue from previous type */        
    } else {
        type = op_ctx->type;
        column = op_ctx->column + 1;
    }

    /* If there is no type, there's nothing to yield */
    if (!type) {
        return false;
    }

    /* If column exceeds number of elements in type, nothing to yield */
    if (column >= ecs_vector_count(type)) {
        return false;
    }

    ecs_entity_t *elem = ecs_vector_get(type, ecs_entity_t, column);
    ecs_assert(elem != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_rule_pair_t pair = op->param.is.entity;
    ecs_entity_t look_for = pair_to_entity(it, pair);
    bool wildcard = entity_is_wildcard(look_for);
    
    if (redo && !wildcard) {
        /* If this is a redo and the queried for entity is not a wildcard,
         * there is nothing more to yield. */        
        return false;
    }
    
    column = op_ctx->column = find_next_match(type, column, look_for);
    if (column == -1) {
        /* No more matches */
        return false;
    }

    /* If this is a wildcard query, fill out the variable registers */
    if (wildcard) {
        reify_variables(it, pair, type, column, look_for);
    }

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
    case EcsRuleSelect:
        return eval_select(it, op, op_index, redo);
    case EcsRuleWith:
        return eval_with(it, op, op_index, redo);                
    case EcsRuleEach:
        return eval_each(it, op, op_index, redo);
    case EcsRuleFrom:
        return eval_from(it, op, op_index, redo);  
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

/* Iterator next function. This evaluates the program until it reaches a Yield
 * operation, and returns the intermediate result(s) to the application. An
 * iterator can, depending on the program, either return a table, entity, or
 * just true/false, in case a rule doesn't contain the this variable. */
bool ecs_rule_next(
    ecs_iter_t *iter)
{
    ecs_rule_iter_t *it = &iter->iter.rule;
    const ecs_rule_t *rule = it->rule;
    bool redo = it->op != 0;

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
                    ecs_data_t *data = ecs_table_get_data(table);

                    /* Table must have data, or otherwise it wouldn't yield */
                    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

                    /* Tell the iterator how many entities there are */
                    iter->count = ecs_table_data_count(data);
                    ecs_assert(iter->count != 0, ECS_INTERNAL_ERROR, NULL);

                    /* Set the entities array */
                    iter->entities = ecs_vector_first(
                        data->entities, ecs_entity_t);
                    ecs_assert(iter->entities != NULL, ECS_INTERNAL_ERROR, NULL);

                } else {
                    /* If a single entity is returned, simply return the
                     * iterator with count 1 and a pointer to the entity id */
                    ecs_assert(var->kind == EcsRuleVarKindEntity, 
                        ECS_INTERNAL_ERROR, NULL);
                    ecs_entity_t e = reg->is.entity;
                    it->entity = e;
                    iter->count = 1;
                    iter->entities = &it->entity;
                }
            }

            return true;
        }
    } while ((it->op != -1));

    return false;
}
