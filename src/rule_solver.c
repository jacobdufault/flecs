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
} ecs_rule_register_t;

/* Operations describe how the rule should be evaluated */
typedef enum ecs_operation_kind_t {
    EcsRuleInput,
    EcsRuleWith,
    EcsRuleFrom,
    EcsRuleMatch,
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
    const char *name;
    int8_t reg;
} ecs_rule_variable_t;

/* Operation iteration state */
typedef struct ecs_operation_state_t {
    int32_t cur;    /* Marks location of last evaluated element */
} ecs_operation_state_t;

struct ecs_rule_t {
    ecs_world_t *world;
    ecs_rule_operation_t *operations;
    ecs_rule_variable_t *variables;

    int16_t operation_count;
    int8_t register_count;
    int8_t variable_count;
    int8_t column_count;
};

static
ecs_rule_operation_t* create_operation(
    ecs_rule_t *rule)
{
    int8_t cur = rule->operation_count ++;
    rule->operations = ecs_os_realloc(rule->operations, (cur + 1) * ECS_SIZEOF(ecs_rule_operation_t));
    return &rule->operations[cur];
}

static
int32_t create_register(
    ecs_rule_t *rule)
{
    return rule->register_count ++;
}

static
const ecs_rule_variable_t* create_variable(
    ecs_rule_t *rule,
    const char *name)
{
    int8_t reg = create_register(rule);
    int8_t cur = rule->variable_count ++;
    rule->variables = ecs_os_realloc(rule->variables, (cur + 1) * ECS_SIZEOF(ecs_rule_variable_t));
    
    ecs_rule_variable_t *var = &rule->variables[cur];
    var->name = ecs_os_strdup(name);
    var->reg = reg;
    return var;
}

static
const ecs_rule_variable_t* find_variable(
    ecs_rule_t *rule,
    const char *name)
{
    ecs_assert(rule != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(name != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_rule_variable_t *variables = rule->variables;
    int32_t i, count = rule->variable_count;
    
    for (i = 0; i < count; i ++) {
        ecs_rule_variable_t *variable = &variables[i];
        if (!strcmp(name, variable->name)) {
            return variable;
        }
    }

    return NULL;
}

static
const ecs_rule_variable_t* ensure_variable(
    ecs_rule_t *rule,
    const char *name)
{
    const ecs_rule_variable_t *var = find_variable(rule, name);
    if (!var) {
        var = create_variable(rule, name);
    }
    return var;
}

static
ecs_rule_pair_t column_to_pair(
    ecs_rule_t *rule,
    ecs_sig_column_t *column)
{
    ecs_rule_pair_t result = {0};

    ecs_entity_t type_id = column->type.entity;
    if (!type_id) {
        const ecs_rule_variable_t *var = ensure_variable(rule, column->type.name);
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
        const ecs_rule_variable_t *var = ensure_variable(rule, column->argv[1].name);
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

ecs_rule_t* ecs_rule_new(
    ecs_world_t *world,
    const char *expr)
{
    ecs_sig_t sig;
    if (ecs_sig_init(world, NULL, expr, &sig)) {
        return NULL;
    }

    ecs_rule_t *result = ecs_os_calloc(ECS_SIZEOF(ecs_rule_t));
    ecs_sig_column_t *columns = ecs_vector_first(sig.columns, ecs_sig_column_t);
    int32_t i, count = ecs_vector_count(sig.columns);

    result->world = world;
    result->column_count = count;

    /* Create initial variable for This, which is always on index 0 */
    const ecs_rule_variable_t *v_this = create_variable(result, ".");
    int8_t r_this = v_this->reg;

    /* Create first operation, which is always Input. This creates an entry in
     * the register stack for the initial state. */
    ecs_rule_operation_t *op = create_operation(result);
    op->kind = EcsRuleInput;

    /* The first time Input is evaluated it goes to the next/first operation */
    op->on_ok = 1;

    /* When Input is evaluated with redo = true it will return false, which will
     * finish the program as op becomes -1. */
    op->on_fail = -1;

    /* Step 1: find all expressions that match the type of This */
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        /* Find columns where arg count is 0 or first argument is This  */
        if (!column->argc || column->argv[0].entity == EcsThis) {
            op = create_operation(result);
            op->kind = EcsRuleWith;
            op->param.kind = EcsRuleParamPair;
            op->param.is.entity = column_to_pair(result, column);
            op->on_ok = result->operation_count;
            op->on_fail = result->operation_count - 2;
            
            /* Store corresponding signature column so we can correlate and
             * store the table columns with signature columns. */
            op->column = i;

            /* Set register parameters. r_1 = in, r_2 = out */
            if (result->operation_count == 2) {
                /* The first With sets the register and has no input */
                op->r_1 = -1;
                op->r_2 = r_this;
            } else {
                /* Subsequent With's read the register, and write no output */
                op->r_1 = r_this;
                op->r_2 = -1;
            }
        }
    }

    /* Step 2: insert instructions for remaining expressions */
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        if (column->argc && column->argv[0].entity != EcsThis) {
            /* Select from other entities (not This) */
            op = create_operation(result);
            op->kind = EcsRuleFrom;
            op->param.kind = EcsRuleParamPair;
            op->param.is.entity = column_to_pair(result, column);            
            op->on_ok = result->operation_count;
            op->on_fail = result->operation_count - 2;
            op->column = i;

            /* Input entity */
            const ecs_rule_variable_t *v = ensure_variable(
                    result, column->argv[0].name);
            op->r_1 = v->reg;
        }
    }

    /* Insert yield instruction */
    op = create_operation(result);
    op->kind = EcsRuleYield;
    op->on_fail = result->operation_count - 2;
    /* Yield can only fail to match more */

    return result;
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

    /* Get register indices for in & output */
    int8_t r_in = op->r_1;
    int8_t r_out = op->r_2;

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param.is.entity;
    ecs_entity_t look_for = pair_to_entity(it, pair);
    bool wildcard = entity_is_wildcard(look_for);
    bool first = r_in == -1;

    /* If looked for entity is not a wildcard (meaning there are no unknown/
     * unconstrained variables), this is not the first With in a chain, and this
     * is a redo, there is nothing more to yield. */
    if (redo && !wildcard && !first) {
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
        /* If this is the first With in the chain, return the first table_record
         * in the table set. */
        if (first) {
            table_record = ecs_sparse_get(table_set, ecs_table_record_t, 0);
            table = table_record->table;
            op_ctx->table_index = 0;

        /* If this is not the first With in the chain, get the table from the
         * input register, and test if it's a member of the table set. */
        } else {
            table = regs[r_in].is.table;
            ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

            table_record = ecs_sparse_get_sparse(
                table_set, ecs_table_record_t, table->id);
        }

        /* If no table record was found, there are no results. */
        if (!table_record) {
            return false;
        }
        
        ecs_assert(table == table_record->table, ECS_INTERNAL_ERROR, NULL);

        /* Set current column to first occurrence of queried for entity */
        column = columns[op->column] = table_record->column;

        /* If this is the first With in the chain, store table in register */
        if (first) {
            regs[r_out].is.table = table_record->table;
        }
    
    /* If this is a redo, progress to the next match */
    } else {

        /* First test if there are any more matches for the current table, in 
         * case we're looking for a wildcard. */
        if (wildcard) {
            if (first) {
                table = regs[r_out].is.table;
            } else {
                table = regs[r_in].is.table;
            }

            ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

            column = columns[op->column];
            column = find_next_match(table->type, column + 1, look_for);

            columns[op->column] = column;
        }

        /* If no next match was found for this table, move to next table */
        if (column == -1) {
            if (first) {
                int32_t table_index = ++ op_ctx->table_index;
                if (table_index >= ecs_sparse_count(table_set)) {
                    /* If no more records were found, nothing more to be done */
                    return false;
                }

                table_record = ecs_sparse_get(
                    table_set, ecs_table_record_t, table_index);
                ecs_assert(table_record != NULL, ECS_INTERNAL_ERROR, NULL);

                /* Assign new table to table register */
                table = regs[r_out].is.table = table_record->table;

                /* Assign first matching column */
                column = columns[op->column] = table_record->column;
            
            /* If this is not the first With in the chain, nothing to be done */
            } else {
                return false;
            }
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
    case EcsRuleYield:
        return eval_yield(it, op, op_index, redo);
    case EcsRuleWith:
        return eval_with(it, op, op_index, redo);
    case EcsRuleFrom:
        return eval_from(it, op, op_index, redo);      
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
