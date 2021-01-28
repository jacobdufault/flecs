#include "private_api.h"

/* var: Variable types:
 * --------------------
 * .                             : self
 * X                             : variable
 * _                             : anonymous variable
 * Identifier                    : entity literal
 * (Role, Subject)               : pair literal 
 */ 

/* expr: Expression types:
 * -----------------
 * var:0                         : true if self has var:0
 * var:0(var:1)                  : true if var:1 has var:0
 * var:0(var:1, var:2)           : true if var:1 has (var:0, var:2)
 * [expr:0, expr:1, ..., expr:n] : Type expression
 */

/* Register types:
 * ---------------
 * Entity
 * Type
 * Table
 */

/* Operations:
 * -----------
 * With(expr)                    : push each Table with provided Entity, Type or Table's Type
 * From(expr)                    : push each Entity from an Entity's Type, Type or Table's Type
 * Match(expr)                   : test expression, push if true, pop if false
 * Yield                         : yield current state
 *
 * Expanded:
 * ---------
 * With(Entity)
 *
 * From(Entity)
 * From(Type)
 * From(Table)
 * 
 * Match(reg, Entity)
 * Match(reg, Type)
 * MatchSubject(reg, Entity)
 * MatchRole(reg, Entity)
 * MatchPair(reg, Entity)
 */

/* Examples
 * ----------------
 * 
 * Position, Velocity
 *   With [Position, Velocity] -> . // for each table with Type
 *   Yield .                        // yield table
 *
 * Likes(., X)
 *   With (Likes, *) -> .           // for each table with (Likes, *) pair
 *   From . -> r0                   // for each (r0 in typeof(.))
 *   Match r0 <> (Likes, *) > r1    // match r0 with (Likes, *), store * in r1
 *   Yield ., X = r1
 *   
 * Likes(., X), Likes(X, .)
 *   With (Likes, *) -> .           // for each table with (Likes, *) pair
 *   From . -> r0                   // for each (r0 in typeof(.))
 *   Match r0 == (Likes, *) -> r1   // match r0 with (Likes, *), store * in r1
 *   From X -> r2                   // for each (r2 in typeof(.))
 *   Match r2 == (Likes, .)         // match r2 with (Likes, .)
 *   Yield ., X = r1
 *
 * X(., Bob)
 *   With (*, Bob) -> .             // for each table with (*, *) pair
 *   From . -> r0                   // for each (r0 in typeof(.))
 *   Match r0 == (*, Bob) -> r1     // match r0 with (*, Bob), store * in r1
 *   Yield ., X = r1
 *
 * X(., Y)
 *   With (*, *) -> .               // for each table with (*, *) pair
 *   From . -> r0                   // for each (r0 in typeof(.))
 *   Match r0 == (*, *) -> r1, r2   // match r0 with (*, *), store (*,*) in (r1,r2)
 *   Yield ., X = r1, Y = r2
 *
 * X(., Y), Y(., X)
 *   With (*, *) -> .               // for each table with (*, *) pair
 *   From . -> r0                   // for each (r0 in typeof(.))
 *   Match r0 == (*, *) -> r1, r2   // match r0 with (*, *), store (*,*) in (r1,r2)
 *   From . -> r0                   // for each (r0 in typeof(.))
 *   Match r0 == (r2, r1)           // match r0 with (r2, r1)
 *
 * .(Bob)
 *   From Bob -> r0                 // for each (r0 in typeof(Bob))
 *   Yield . = r0
 *
 * .(X)
 *   With [] -> r0
 *   From r0 -> .
 *   Yield ., X = r0
 *
 * .(X, Bob)
 *   With (*, Bob) -> r0
 *   From r0 -> .
 *   Match . == (r0, Bob)
 *   Yield ., X = r0
 * 
 * Likes(Bob, .)
 *   From Bob -> r0                 // for each (r0 in typeof(Bob))
 *   Match r0 == (Likes, *) -> r1   // match r0 with (Likes, *), store * in r1
 *   Yield . = r1
 *
 * Likes(., X), Position(X), Velocity(X), Likes(X, Bob)
 *   With (Likes, *) -> .
 *   From . -> r0
 *   Match r0 == (Likes, *) -> r1
 *   From r1 -> r2
 *   Match r2 == [Position, Velocity, (Likes, Bob)]
 *   Yield ., r1
 */

#define ECS_RULE_MAX_VARIABLE_COUNT (256)

/* A rule pair contains a type and subject that can be stored in a register. */
typedef struct ecs_rule_pair_t {
    uint32_t type;
    uint32_t subject;
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
typedef enum ecs_rule_register_kind_t {
    EcsRuleRegisterUnknown,
    EcsRuleRegisterEntity,
    EcsRuleRegisterType,
    EcsRuleRegisterTable
} ecs_rule_register_kind_t;

typedef struct ecs_rule_register_t {
    ecs_rule_register_kind_t kind;
    union {
        ecs_entity_t entity;
        ecs_vector_t *type;
        ecs_table_t *table;
    } is;
    uint8_t var_id;
} ecs_rule_register_t;

/* Operations describe how the rule should be evaluated */
typedef enum ecs_operation_kind_t {
    EcsRuleInput,
    EcsRuleSelect,
    EcsRuleWith,
    EcsRuleFrom,
    EcsRuleEach,
    EcsRuleYield
} ecs_operation_kind_t;

typedef struct ecs_rule_operation_t {
    ecs_operation_kind_t kind;  /* What kind of operation is it */
    ecs_rule_param_t param;     /* Parameter that contains optional filter */

    int16_t on_ok;              /* Jump location when match succeeds */
    int16_t on_fail;            /* Jump location when match fails */

    int8_t column;              /* Corresponding column index in signature */
    int8_t r_1;                 /* Optional In/Out registers */
    int8_t r_2;
    int8_t r_3;
} ecs_rule_operation_t;

typedef struct ecs_rule_with_ctx_t {
    ecs_sparse_t *table_set;
    int32_t table_index;
} ecs_rule_with_ctx_t;

typedef struct ecs_rule_from_ctx_t {
    ecs_type_t type;
    int32_t column;
} ecs_rule_from_ctx_t;

typedef struct ecs_rule_operation_ctx_t {
    union {
        ecs_rule_with_ctx_t with;
        ecs_rule_from_ctx_t from;
    } is;
} ecs_rule_operation_ctx_t;

/* Rule variables allow for the rule to be parameterized */
typedef struct ecs_rule_variable_t {
    ecs_rule_register_kind_t kind;
    const char *name;
    uint8_t id;     /* unique variable id */
    uint8_t reg;    /* register associated with variable */
    uint8_t occurs; /* number of occurrences (used for operation ordering) */
    uint8_t depth;  /* depth in dependency tree (used for operation ordering) */
} ecs_rule_variable_t;

/* Operation iteration state */
typedef struct ecs_operation_state_t {
    int32_t cur;    /* Marks location of last evaluated element */
} ecs_operation_state_t;

struct ecs_rule_t {
    ecs_world_t *world;
    ecs_rule_operation_t *operations;
    ecs_rule_variable_t *variables;
    ecs_sig_t sig;

    int8_t register_count;
    int8_t variable_count;
    int8_t column_count;
    uint8_t this_id;

    int16_t operation_count;
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
ecs_rule_operation_t* create_operation(
    ecs_rule_t *rule)
{
    int8_t cur = rule->operation_count ++;
    rule->operations = ecs_os_realloc(rule->operations, (cur + 1) * ECS_SIZEOF(ecs_rule_operation_t));
    ecs_rule_operation_t *result = &rule->operations[cur];
    memset(result, 0, sizeof(ecs_rule_operation_t));
    return result;
}

static
uint8_t create_register(
    ecs_rule_t *rule)
{
    return rule->register_count ++;
}

static
ecs_rule_variable_t* create_variable(
    ecs_rule_t *rule,
    ecs_rule_register_kind_t kind,
    const char *name)
{
    uint8_t cur = ++ rule->variable_count;
    rule->variables = ecs_os_realloc(
        rule->variables, cur * ECS_SIZEOF(ecs_rule_variable_t));
    
    uint8_t reg = create_register(rule);
  
    ecs_rule_variable_t *var = &rule->variables[cur - 1];
    var->name = ecs_os_strdup(name);
    var->kind = kind;
    var->id = cur - 1;
    var->reg = reg;
    var->depth = UINT8_MAX;

    return var;
}

static
ecs_rule_variable_t* find_variable(
    ecs_rule_t *rule,
    ecs_rule_register_kind_t kind,
    const char *name)
{
    ecs_assert(rule != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(name != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_rule_variable_t *variables = rule->variables;
    int32_t i, count = rule->variable_count;
    
    for (i = 0; i < count; i ++) {
        ecs_rule_variable_t *variable = &variables[i];
        if (!strcmp(name, variable->name)) {
            if (kind == EcsRuleRegisterUnknown || kind == variable->kind) {
                return variable;
            }
        }
    }

    return NULL;
}

static
ecs_rule_variable_t* ensure_variable(
    ecs_rule_t *rule,
    ecs_rule_register_kind_t kind,
    const char *name)
{
    ecs_rule_variable_t *var = find_variable(rule, kind, name);
    if (!var) {
        var = create_variable(rule, kind, name);
    } else {
        if (var->kind == EcsRuleRegisterUnknown) {
            var->kind = kind;
        }
    }

    return var;
}

static
ecs_rule_pair_t column_to_pair(
    ecs_rule_t *rule,
    ecs_sig_column_t *column)
{
    ecs_rule_pair_t result = {0};

    ecs_entity_t type_id = column->pred.entity;
    if (!type_id) {
        const ecs_rule_variable_t *var = ensure_variable(
            rule, EcsRuleRegisterEntity, column->pred.name);
        result.type = var->reg;
        result.reg_mask |= 1;
    } else {
        result.type = type_id;
    }

    if (!column->argc || column->argc == 1) {
        return result;
    }

    /* If arguments is higher than 2 this is not a pair but a nested rule */
    ecs_assert(column->argc == 2, ECS_INTERNAL_ERROR, NULL);

    ecs_entity_t subject_id = column->argv[1].entity;
    if (!column->argv[1].entity) {
        const ecs_rule_variable_t *var = ensure_variable(
            rule, EcsRuleRegisterEntity, column->argv[1].name);
        result.subject = var->reg;
        result.reg_mask |= 2;
    } else {
        result.subject = subject_id;
    }

    return result;
}

static
ecs_entity_t pair_to_entity(
    ecs_rule_iter_t *it,
    ecs_rule_pair_t pair)
{
    ecs_entity_t type = pair.type;
    ecs_entity_t subject = pair.subject;

    if (pair.reg_mask & 1) {
        type = it->registers[type].is.entity;
    }
    if (pair.reg_mask & 2) {
        subject = it->registers[subject].is.entity;
    }

    if (!subject) {
        return type;
    } else {
        return ecs_trait(subject, type);
    }
}

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

static
ecs_rule_register_t* get_registers(
    ecs_rule_iter_t *it,
    int32_t op)    
{
    return &it->registers[op * it->rule->variable_count];
}

static
int32_t* get_columns(
    ecs_rule_iter_t *it,
    int32_t op)    
{
    return &it->columns[op * it->rule->column_count];
}

/* Substitute wildcard expression with actual column type ids */
static
void resolve_variables(
    ecs_rule_iter_t *it, 
    ecs_rule_pair_t pair,
    ecs_type_t type,
    int32_t column,
    ecs_entity_t look_for)
{
    /* If look_for does not contain wildcards, there is nothing to resolve */
    ecs_assert(entity_is_wildcard(look_for), ECS_INTERNAL_ERROR, NULL);

    /* If the pair contains references to registers, check if any of them were
     * wildcards while the operation was being evaluated. */
    if (pair.reg_mask) {
        ecs_rule_register_t *regs = get_registers(it, it->op);
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
                    regs[pair.type].kind = EcsRuleRegisterEntity;
                    regs[pair.type].is.entity = 
                        ecs_entity_t_hi(*elem & ECS_COMPONENT_MASK);
                }
            } else if (look_for == EcsWildcard) {
                regs[pair.type].kind = EcsRuleRegisterEntity;
                regs[pair.type].is.entity = *elem;
            }
        }

        /* If subject is a wildcard, this is guaranteed to be a trait */
        if (pair.reg_mask & 2) {
            ecs_assert(ECS_HAS_ROLE(look_for, TRAIT), ECS_INTERNAL_ERROR, NULL);

            /* Same as above, if subject is not a wildcard it could already have
             * been resolved by either input or a previous operation. */
            if (ecs_entity_t_lo(look_for) == EcsWildcard) {
                regs[pair.subject].kind = EcsRuleRegisterEntity;
                regs[pair.subject].is.entity = ecs_entity_t_lo(*elem);
            }
        }
    }
}

/* Find the depth of the dependency tree from the variable to the root */
static
uint8_t get_variable_depth(
    ecs_rule_t *rule,
    ecs_rule_variable_t *var,
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
            if (find_variable(rule, EcsRuleRegisterUnknown, column->pred.name) == var) {
                var_found = true;
            }
        }

        /* Test if the subject is the variable */
        if (!var_found && column->argc > 1) {
            if (!column->argv[1].entity) {
                if (find_variable(rule, EcsRuleRegisterUnknown, column->argv[1].name) == var) {
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
        ecs_rule_variable_t *obj = find_variable(
            rule, EcsRuleRegisterUnknown, column->argv[0].name);

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

static
int compare_variable(
    const void* ptr1, 
    const void *ptr2)
{
    const ecs_rule_variable_t *v1 = ptr1;
    const ecs_rule_variable_t *v2 = ptr2;

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

            ecs_rule_variable_t *obj = find_variable(
                rule, EcsRuleRegisterUnknown, obj_name);
            if (!obj) {
                obj = create_variable(rule, EcsRuleRegisterUnknown, obj_name);
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

        ecs_rule_variable_t *var = &rule->variables[objects[i]];
        var->depth = get_variable_depth(rule, var, root_var, 0);
        if (var->depth == UINT8_MAX) {
            /* Found a disjoint set of variables, which is invalid */
            goto error;
        }
    }

    /* Step 4: order variables by depth, followed by occurrence */
    qsort(rule->variables, rule->variable_count, sizeof(ecs_rule_variable_t), 
        compare_variable);

done:
    return 0;
error:
    return -1;
}

static
void ensure_all_variables(
    ecs_rule_t *rule)
{
    ecs_sig_column_t *columns = ecs_vector_first(rule->sig.columns, ecs_sig_column_t);
    int32_t i, count = rule->column_count;
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        /* If type is a variable, make sure it has been registered */
        if (!column->pred.entity) {
            ensure_variable(rule, EcsRuleRegisterEntity, column->pred.name);
        }

        /* If subject is a variable, make sure it has been registered */
        if (column->argc > 1 && !column->argv[1].entity) {
            ensure_variable(rule, EcsRuleRegisterEntity, column->argv[1].name);
        }
    }    
}

ecs_rule_t* ecs_rule_new(
    ecs_world_t *world,
    const char *expr)
{
    ecs_rule_t *result = ecs_os_calloc(ECS_SIZEOF(ecs_rule_t));

    if (ecs_sig_init(world, NULL, expr, &result->sig)) {
        ecs_os_free(result);
        return NULL;
    }

    ecs_sig_t *sig = &result->sig;
    ecs_sig_column_t *columns = ecs_vector_first(sig->columns, ecs_sig_column_t);
    int32_t c, column_count = ecs_vector_count(sig->columns);

    result->world = world;
    result->column_count = column_count;
    result->this_id = UINT8_MAX;

    /* Create first operation, which is always Input. This creates an entry in
     * the register stack for the initial state. */
    ecs_rule_operation_t *op = create_operation(result);
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
    result->variables[0].kind = EcsRuleRegisterTable;

    /* The remainder of the variables are entities */
    for (v = 1; v < var_count; v ++) {
        result->variables[v].kind = EcsRuleRegisterEntity;
    }

    /* Scan expression for remainder of variables that are not used as objects.
     * This ensures that all variables are known, and that the array won't be
     * reallocated as operations are inserted, which simplifies the code. */
    ensure_all_variables(result);

    /* Iterate variables front to back, and insert operations that have the
     * iterated over variable as object. Variables are stored in dependency
     * order, and by storing operations in the same order it is guaranteed that
     * variables are resolved in optimal order. 
     * At this point the array with variables only contains the variables that
     * appear as objects in the expression. */
    for (v = 0; v < var_count; v ++) {
        ecs_rule_variable_t *var = &result->variables[v];

        for (c = 0; c < column_count; c ++) {
            ecs_sig_column_t *column = &columns[c];

            /* Skip operation if this is not the variable for which operations
             * are currently being inserted. */
            if (strcmp(column->argv[0].name, var->name)) {
                continue;
            }

            /* Insert operation */
            op = create_operation(result);

            /* If the variable is of type table, insert Select/With. The Select
             * operation yields an initial table that matches a provided filter.
             * Subsequent With expressions filter the tables by testing it 
             * against their expression. Select/With operations always appear at
             * the start of a program, and always in a single chain. */
            if (var->kind == EcsRuleRegisterTable) {
                /* If this is the first operation after Input, insert Select */
                if (result->operation_count == 2) {
                    op->kind = EcsRuleSelect;
                    op->r_1 = var->reg; /* Output register */

                /* If this is not the first operation, insert a With. The With
                 * will read the output of Select, and apply its filter. If the
                 * filter passes, the program will resume to the next operation.
                 * If the filter fails, the program will redo the previous 
                 * instruction, which if this were a Select would yield the next
                 * table. The With operation does not output anything as
                 * subsequent operations can reuse the same input register. */
                } else {
                    op->kind = EcsRuleWith;
                    op->r_1 = var->reg; /* Input register */
                }

            /* If this variable is not of type table, it operates on a single
             * entity. The From operation retrieves the type of the entity and
             * applies its filter to it. */
            } else {
                op->kind = EcsRuleFrom;
                op->r_1 = var->reg;

                /* Variable is an entity */
                var->kind = EcsRuleRegisterEntity;
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
    op->on_fail = result->operation_count - 2;
    /* Yield can only fail since it is the end of the program */

    return result;
error:
    /* TODO: proper cleanup */
    ecs_os_free(result);
    return NULL;
}

static
ecs_rule_variable_t* variable_from_reg(
    ecs_rule_t *rule,
    uint8_t reg)
{
    int32_t i, count = rule->variable_count;
    for (i = 0; i < count; i ++) {
        if (rule->variables[i].reg == reg) {
            return &rule->variables[i];
        }
    }

    return NULL;
}

char* ecs_rule_str(
    ecs_rule_t *rule)
{
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    char filter_expr[256];

    int32_t i, count = rule->operation_count;
    for (i = 1; i < count; i ++) {
        ecs_rule_operation_t *op = &rule->operations[i];
        ecs_rule_pair_t pair = op->param.is.entity;
        ecs_entity_t type = pair.type;
        ecs_entity_t subject = pair.subject;
        const char *type_name, *subject_name;

        if (pair.reg_mask & 1) {
            ecs_rule_variable_t *type_var = variable_from_reg(rule, type);
            type_name = type_var->name;
        } else {
            type_name = ecs_get_name(rule->world, type);
        }

        if (subject) {
            if (pair.reg_mask & 2) {
                ecs_rule_variable_t *subj_var = variable_from_reg(rule, subject);
                subject_name = subj_var->name;
            } else {
                subject_name = ecs_get_name(rule->world, subject);
            }
        }

        if (!subject) {
            sprintf(filter_expr, "(%s)", type_name);
        } else {
            sprintf(filter_expr, "(%s, %s)", type_name, subject_name);
        }

        ecs_strbuf_append(&buf, "%d: [Pass:%d, Fail:%d] ", i, op->on_ok, op->on_fail);

        ecs_rule_variable_t *var = variable_from_reg(rule, op->r_1);
        if (var) {
            ecs_strbuf_append(&buf, "%s(%s) <- ", 
                var->kind == EcsRuleRegisterTable ? " Table" : "Entity",
                var->name);
        }

        switch(op->kind) {
        case EcsRuleSelect:
            ecs_strbuf_append(&buf, "select %s", filter_expr);
            break;
        case EcsRuleWith:
            ecs_strbuf_append(&buf, "with %s", filter_expr);
            break;
        case EcsRuleFrom:
            ecs_strbuf_append(&buf, "from %s", filter_expr);
            break;
        case EcsRuleYield:
            ecs_strbuf_append(&buf, "yield");
            break;
        default:
            continue;
        }

        ecs_strbuf_appendstr(&buf, "\n");
    }

    return ecs_strbuf_get(&buf);
}

int32_t ecs_rule_variable_count(
    const ecs_rule_t *rule)
{
    ecs_assert(rule != NULL, ECS_INTERNAL_ERROR, NULL);
    return rule->variable_count;
}

const char* ecs_rule_variable_name(
    const ecs_rule_t *rule,
    int32_t var_id)
{
    return rule->variables[var_id].name;
}

ecs_entity_t ecs_rule_variable(
    ecs_iter_t *iter,
    int32_t var_id)
{
    ecs_rule_iter_t *it = &iter->iter.rule;
    ecs_rule_register_t *regs = get_registers(it, it->op);

    if (regs[var_id].kind == EcsRuleRegisterEntity) {
        return regs[var_id].is.entity;
    } else {
        return 0;
    }
}

ecs_iter_t ecs_rule_iter(
    const ecs_rule_t *rule)
{
    ecs_iter_t result;
    ecs_rule_iter_t *it = &result.iter.rule;
    it->rule = rule;
    
    it->registers = ecs_os_malloc(rule->operation_count * rule->variable_count * ECS_SIZEOF(ecs_rule_register_t));
    it->op_ctx = ecs_os_malloc(rule->operation_count * ECS_SIZEOF(ecs_rule_operation_ctx_t));
    it->columns = ecs_os_malloc(rule->operation_count * rule->column_count * ECS_SIZEOF(int32_t));
    it->op = 0;

    int i;
    for (i = 0; i < rule->variable_count; i ++) {
        it->registers[i].is.entity = EcsWildcard;
    }

    return result;
}

static
bool eval_input(
    ecs_rule_iter_t *it,
    ecs_rule_operation_t *op,
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

static
bool eval_yield(
    ecs_rule_iter_t *it,
    ecs_rule_operation_t *op,
    int16_t op_index,
    bool redo)
{
    /* Yield always returns false, because there are never any operations after
     * a yield. */
    return false;
}

static
bool eval_select(
    ecs_rule_iter_t *it,
    ecs_rule_operation_t *op,
    int16_t op_index,
    bool redo)
{
    ecs_world_t *world = it->rule->world;
    ecs_rule_with_ctx_t *op_ctx = &it->op_ctx[op_index].is.with;
    ecs_table_record_t *table_record = NULL;
    ecs_rule_register_t *regs = get_registers(it, op_index);

    /* Get register indices for output */
    int8_t r = op->r_1;

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
        /* Return the first table_record in the table set. */
        table_record = ecs_sparse_get(table_set, ecs_table_record_t, 0);

        /* If no table record was found, there are no results. */
        if (!table_record) {
            return false;
        }

        table = table_record->table;
        op_ctx->table_index = 0;

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
            int32_t table_index = ++ op_ctx->table_index;
            if (table_index >= ecs_sparse_count(table_set)) {
                /* If no more records were found, nothing more to be done */
                return false;
            }

            table_record = ecs_sparse_get(
                table_set, ecs_table_record_t, table_index);
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
        resolve_variables(it, pair, table->type, column, look_for);
    }

    return true;    
}

static
bool eval_with(
    ecs_rule_iter_t *it,
    ecs_rule_operation_t *op,
    int16_t op_index,
    bool redo)
{
    ecs_world_t *world = it->rule->world;
    ecs_rule_with_ctx_t *op_ctx = &it->op_ctx[op_index].is.with;
    ecs_table_record_t *table_record = NULL;
    ecs_rule_register_t *regs = get_registers(it, op_index);

    /* Get register indices for input */
    int8_t r = op->r_1;

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
        resolve_variables(it, pair, table->type, column, look_for);
    }

    return true;
}

static
bool eval_from(
    ecs_rule_iter_t *it,
    ecs_rule_operation_t *op,
    int16_t op_index,
    bool redo)
{
    ecs_world_t *world = it->rule->world;
    ecs_type_t type = NULL;
    int32_t column = -1;
    ecs_rule_from_ctx_t *op_ctx = &it->op_ctx[op_index].is.from;
    ecs_rule_register_t *regs = get_registers(it, op_index);

    /* If this is not a redo, get type from input */
    if (!redo) {
        int8_t r_in = op->r_1;
        ecs_rule_register_kind_t reg_kind = regs[r_in].kind;

        switch(reg_kind) {
        case EcsRuleRegisterEntity: {
            ecs_entity_t e = regs[r_in].is.entity;
            type = ecs_get_type(world, e);
            break;
        }
        case EcsRuleRegisterType:
            type = regs[r_in].is.type;
            break;
        case EcsRuleRegisterTable:
            type = regs[r_in].is.table->type;
            break;
        default:
            /* Should never get here */
            ecs_abort(ECS_INTERNAL_ERROR, NULL);
            break;
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
        resolve_variables(it, pair, type, column, look_for);
    }

    return true;
}

static
bool eval_op(
    ecs_rule_iter_t *it, 
    ecs_rule_operation_t *op,
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
    case EcsRuleFrom:
        return eval_from(it, op, op_index, redo);  
    case EcsRuleYield:
        return eval_yield(it, op, op_index, redo);            
    default:
        return false;
    }
}

static
void push_registers(
    ecs_rule_iter_t *it,
    int32_t cur,
    int32_t next)
{
    ecs_rule_register_t *src_regs = get_registers(it, cur);
    ecs_rule_register_t *dst_regs = get_registers(it, next);

    memcpy(dst_regs, src_regs, 
        ECS_SIZEOF(ecs_rule_register_t) * it->rule->variable_count);
}

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

bool ecs_rule_next(
    ecs_iter_t *iter)
{
    ecs_rule_iter_t *it = &iter->iter.rule;
    bool redo = it->op != 0;

    do {
        int16_t cur = it->op;
        ecs_rule_operation_t *op = &it->rule->operations[cur];

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
            ecs_rule_register_t *regs = get_registers(it, cur);
            ecs_table_t *table = regs[0].is.table;
            ecs_data_t *data = ecs_table_get_data(table);
            if (!data) {
                continue;
            }

            iter->count = ecs_table_count(table);
            if (!iter->count) {
                continue;
            }

            iter->entities = ecs_vector_first(data->entities, ecs_entity_t);
            ecs_assert(iter->entities != NULL, ECS_INTERNAL_ERROR, NULL);

            return true;
        }
    } while ((it->op != -1));

    return false;
}
