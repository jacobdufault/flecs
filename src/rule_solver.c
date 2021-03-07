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
    int32_t reg_mask; /* bit 1 = predicate, bit 2 = object, bit 4 = wildcard */
    bool transitive; /* Is predicate transitive */
    bool final; /* Is predicate final */
} ecs_rule_pair_t;

/* Filter for evaluating & reifing types and variables. Filters are created ad-
 * hoc from pairs, and take into account all variables that had been resolved
 * up to that point. */
typedef struct ecs_rule_filter_t {
    ecs_entity_t mask;  /* Mask with wildcard in place of variables */

    /* Bloom filter for quickly eliminating ids in a type */
    ecs_entity_t expr_mask; /* AND filter to pass through non-wildcard ids */
    ecs_entity_t expr_match; /* Used to compare with AND expression result */

    bool wildcard; /* Does the filter contain wildcards */
    bool pred_wildcard; /* Is predicate a wildcard */
    bool obj_wildcard; /* Is object a wildcard */
    bool same_var; /* True if pred & obj are both the same variable */

    int32_t hi_var; /* If hi part should be stored in var, this is the var id */
    int32_t lo_var; /* If lo part should be stored in var, this is the var id */
} ecs_rule_filter_t;

/* A rule register stores temporary values for rule variables */
typedef enum ecs_rule_var_kind_t {
    EcsRuleVarKindTable, /* Used for sorting, must be smallest */
    EcsRuleVarKindEntity,
    EcsRuleVarKindUnknown
} ecs_rule_var_kind_t;

typedef struct ecs_rule_table_reg_t {
    ecs_table_t *table;
    int32_t offset;
    int32_t count;
} ecs_rule_table_reg_t;

typedef struct ecs_rule_reg_t {
    int32_t var_id;
    union {
        ecs_entity_t entity;
        ecs_rule_table_reg_t table;
    } is;
} ecs_rule_reg_t;

/* Operations describe how the rule should be evaluated */
typedef enum ecs_rule_op_kind_t {
    EcsRuleInput,       /* Input placeholder, first instruction in every rule */
    EcsRuleSelect,      /* Selects all ables for a given predicate */
    EcsRuleWith,        /* Applies a filter to a table or entity */
    EcsRuleSubSet,      /* Finds all subsets for transitive relationship */
    EcsRuleSuperSet,    /* Finds all supersets for a transitive relationship */
    EcsRuleStore,       /* Store entity in table or entity variable */
    EcsRuleEach,        /* Forwards each entity in a table */
    EcsRuleSetJmp,      /* Set label for jump operation to one of two values */
    EcsRuleJump,        /* Jump to an operation label */
    EcsRuleYield        /* Yield result */
} ecs_rule_op_kind_t;

/* Single operation */
typedef struct ecs_rule_op_t {
    ecs_rule_op_kind_t kind;    /* What kind of operation is it */
    ecs_rule_pair_t param;      /* Parameter that contains optional filter */
    ecs_entity_t subject;       /* If set, operation has a constant subject */

    int32_t on_pass;              /* Jump location when match succeeds */
    int32_t on_fail;            /* Jump location when match fails */

    int32_t column;              /* Corresponding column index in signature */
    int32_t r_in;               /* Optional In/Out registers */
    int32_t r_out;

    bool has_in, has_out;       /* Keep track of whether operation uses input
                                 * and/or output registers. This helps with
                                 * debugging rule programs. */
} ecs_rule_op_t;

/* With context. Shared with select. */
typedef struct ecs_rule_with_ctx_t {
    ecs_sparse_t *table_set;    /* Currently evaluated table set */
    int32_t table_index;        /* Currently evaluated index in table set */

    ecs_sparse_t *all_for_pred; /* Table set that blanks out object with a 
                                 * wildcard. Used for transitive queries */
} ecs_rule_with_ctx_t;

/* Subset context */
typedef struct ecs_rule_subset_frame_t {
    ecs_rule_with_ctx_t with_ctx;
    ecs_table_t *table;
    int32_t row;
    int32_t column;
} ecs_rule_subset_frame_t;

typedef struct ecs_rule_subset_ctx_t {
    ecs_rule_subset_frame_t storage[16]; /* Alloc-free array for small trees */
    ecs_rule_subset_frame_t *stack;
    int32_t sp;
} ecs_rule_subset_ctx_t;

/* Superset context */
typedef struct ecs_rule_superset_frame_t {
    ecs_table_t *table;
    int32_t column;
} ecs_rule_superset_frame_t;

typedef struct ecs_rule_superset_ctx_t {
    ecs_rule_superset_frame_t storage[16]; /* Alloc-free array for small trees */
    ecs_rule_superset_frame_t *stack;
    ecs_sparse_t *table_set;
    int32_t sp;
} ecs_rule_superset_ctx_t;

/* Each context */
typedef struct ecs_rule_each_ctx_t {
    int32_t row;                /* Currently evaluated row in evaluated table */
} ecs_rule_each_ctx_t;

/* Jump context */
typedef struct ecs_rule_setjmp_ctx_t {
    int32_t label;             /* Operation label to jump to */
} ecs_rule_setjmp_ctx_t;

/* Operation context. This is a per-operation, per-iterator structure that
 * stores information for stateful operations. */
typedef struct ecs_rule_op_ctx_t {
    union {
        ecs_rule_subset_ctx_t subset;
        ecs_rule_superset_ctx_t superset;
        ecs_rule_with_ctx_t with;
        ecs_rule_each_ctx_t each;
        ecs_rule_setjmp_ctx_t setjmp;
    } is;
    int32_t last_op;
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
    const ecs_rule_t *rule,
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
    int32_t cur = rule->operation_count ++;
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
    int32_t cur = ++ rule->variable_count;
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

static
ecs_rule_var_t* create_anonymous_variable(
    ecs_rule_t *rule,
    ecs_rule_var_kind_t kind)
{
    return create_variable(rule, kind, NULL);
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
int32_t* rule_get_columns(
    ecs_rule_iter_t *it,
    int32_t op)    
{
    return &it->columns[op * it->rule->column_count];
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
void entity_reg_set(
    const ecs_rule_t *rule,
    ecs_rule_reg_t *regs,
    int32_t r,
    ecs_entity_t entity)
{
    ecs_assert(rule->variables[regs[r].var_id].kind == EcsRuleVarKindEntity, 
        ECS_INTERNAL_ERROR, NULL);

    regs[r].is.entity = entity;
}

static 
ecs_entity_t entity_reg_get(
    const ecs_rule_t *rule,
    ecs_rule_reg_t *regs,
    int32_t r)
{
    ecs_assert(rule->variables[regs[r].var_id].kind == EcsRuleVarKindEntity, 
        ECS_INTERNAL_ERROR, NULL);
    
    return regs[r].is.entity;
}

static
void table_reg_set(
    const ecs_rule_t *rule,
    ecs_rule_reg_t *regs,
    int32_t r,
    ecs_table_t *table)
{
    ecs_assert(rule->variables[regs[r].var_id].kind == EcsRuleVarKindTable, 
        ECS_INTERNAL_ERROR, NULL);

    regs[r].is.table.table = table;
    regs[r].is.table.offset = 0;
    regs[r].is.table.count = 0;
}

static 
ecs_table_t* table_reg_get(
    const ecs_rule_t *rule,
    ecs_rule_reg_t *regs,
    int32_t r)
{
    ecs_assert(rule->variables[regs[r].var_id].kind == EcsRuleVarKindTable, 
        ECS_INTERNAL_ERROR, NULL);

    return regs[r].is.table.table;       
}

static
ecs_entity_t reg_get_entity(
    const ecs_rule_t *rule,
    ecs_rule_op_t *op,
    ecs_rule_reg_t *regs,
    int32_t r)
{
    if (r == UINT8_MAX) {
        ecs_assert(op->subject != 0, ECS_INTERNAL_ERROR, NULL);
        return op->subject;
    }
    if (rule->variables[r].kind == EcsRuleVarKindTable) {
        int32_t count = regs[r].is.table.count;
        int32_t offset = regs[r].is.table.offset;

        ecs_assert(count == 1, ECS_INTERNAL_ERROR, NULL);
        ecs_data_t *data = ecs_table_get_data(table_reg_get(rule, regs, r));
        ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);
        ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);
        ecs_assert(entities != NULL, ECS_INTERNAL_ERROR, NULL);
        ecs_assert(offset < ecs_vector_count(data->entities), 
            ECS_INTERNAL_ERROR, NULL);
        
        return entities[offset];
    }
    if (rule->variables[r].kind == EcsRuleVarKindEntity) {
        return entity_reg_get(rule, regs, r);
    }

    /* Must return an entity */
    ecs_assert(false, ECS_INTERNAL_ERROR, NULL);

    return 0;
}

static
ecs_table_t* reg_get_table(
    const ecs_rule_t *rule,
    ecs_rule_op_t *op,
    ecs_rule_reg_t *regs,
    int32_t r)
{
    if (r == UINT8_MAX) {
        ecs_assert(op->subject != 0, ECS_INTERNAL_ERROR, NULL);
        return table_from_entity(rule->world, op->subject);
    }
    if (rule->variables[r].kind == EcsRuleVarKindTable) {
        return table_reg_get(rule, regs, r);
    }
    if (rule->variables[r].kind == EcsRuleVarKindEntity) {
        return table_from_entity(rule->world, entity_reg_get(rule, regs, r));
    } 
    return NULL;
}

static
void reg_set_entity(
    const ecs_rule_t *rule,
    ecs_rule_reg_t *regs,
    int32_t r,
    ecs_entity_t entity)
{
    if (rule->variables[r].kind == EcsRuleVarKindTable) {
        ecs_world_t *world = rule->world;
        ecs_record_t *record = ecs_eis_get(world, entity);
        if (!record) {
            rule_error(rule, "failed to store entity %d, has no table", entity);
        } else {
            bool is_monitored;
            regs[r].is.table.table = record->table;
            regs[r].is.table.offset = ecs_record_to_row(record->row, &is_monitored);
            regs[r].is.table.count = 1;
        }
    } else {
        entity_reg_set(rule, regs, r, entity);
    }
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
        result.final = true;
    } else {
        /* If the predicate is not a variable, simply store its id. */
        result.pred = pred_id;

        /* Test if predicate is transitive. When evaluating the predicate, this
         * will also take into account transitive relationships */
        if (ecs_has_entity(rule->world, pred_id, EcsTransitive)) {
            /* Transitive queries must have an object */
            if (column->argc == 2) {
                result.transitive = true;
            }
        }

        if (ecs_has_entity(rule->world, pred_id, EcsFinal)) {
            result.final = true;
        }
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

static
void set_filter_expr_mask(
    ecs_rule_filter_t *result,
    ecs_entity_t mask)
{
    ecs_entity_t lo = ecs_entity_t_lo(mask);
    ecs_entity_t hi = ecs_entity_t_hi(mask & ECS_COMPONENT_MASK);

    /* Make sure roles match between expr & eq mask */
    result->expr_mask = ECS_ROLE_MASK & mask;
    result->expr_match = ECS_ROLE_MASK & mask;

    /* Set parts that are not wildcards to F's. This ensures that when the
     * expr mask is AND'd with a type id, only the non-wildcard parts are
     * set in the id returned by the expression. */
    result->expr_mask |= 0xFFFFFFFF * (lo != EcsWildcard);
    result->expr_mask |= ((uint64_t)0xFFFFFFFF << 32) * (hi != EcsWildcard);

    /* Only assign the non-wildcard parts to the id. This is compared with
     * the result of the AND operation between the expr_mask and id from the
     * entity's type. If it matches, it means that the non-wildcard parts of
     * the filter match */
    result->expr_match |= lo * (lo != EcsWildcard);
    result->expr_match |= (hi << 32) * (hi != EcsWildcard);
}

/* When an operation has a pair, it is used to filter its input. This function
 * translates a pair back into an entity id, and in the process substitutes the
 * variables that have already been filled out. It's one of the most important
 * functions, as a lot of the filtering logic depends on having an entity that
 * has all of the reified variables correctly filled out. */
static
ecs_rule_filter_t pair_to_filter(
    ecs_rule_iter_t *it,
    ecs_rule_pair_t pair)
{
    ecs_entity_t pred = pair.pred;
    ecs_entity_t obj = pair.obj;
    ecs_rule_filter_t result = {
        .lo_var = -1,
        .hi_var = -1
    };

    /* Get registers in case we need to resolve ids from registers. Get them
     * from the previous, not the current stack frame as the current operation
     * hasn't reified its variables yet. */
    ecs_rule_reg_t *regs = get_registers(it, it->op_ctx[it->op].last_op);

    if (pair.reg_mask & RULE_PAIR_OBJECT) {
        obj = entity_reg_get(it->rule, regs, obj);
        if (obj == EcsWildcard) {
            result.wildcard = true;
            result.obj_wildcard = true;
            result.lo_var = pair.obj;
        }
    }

    if (pair.reg_mask & RULE_PAIR_PREDICATE) {
        pred = entity_reg_get(it->rule, regs, pred);
        if (pred == EcsWildcard) {
            if (result.wildcard) {
                result.same_var = pair.pred == pair.obj;
            }

            result.wildcard = true;
            result.pred_wildcard = true;

            if (obj) {
                result.hi_var = pair.pred;
            } else {
                result.lo_var = pair.pred;
            }
        }
    }

    if (!obj) {
        result.mask = pred;
    } else {
        result.mask = ecs_trait(obj, pred);
    }

    /* Construct masks for quick evaluation of a filter. These masks act as a
     * bloom filter that is used to quickly eliminate non-matching elements in
     * an entity's type. */
    if (result.wildcard) {
        set_filter_expr_mask(&result, result.mask);
    }

    return result;
}

/* This function is responsible for reifying the variables (filling them out 
 * with their actual values as soon as they are known). It uses the pair 
 * expression returned by pair_get_most_specific_var, and attempts to fill out each of the
 * wildcards in the pair. If a variable isn't reified yet, the pair expression
 * will still contain one or more wildcards, which is harmless as the respective
 * registers will also point to a wildcard. */
static
void reify_variables(
    ecs_rule_iter_t *it, 
    ecs_rule_filter_t *filter,
    ecs_type_t type,
    int32_t column)
{
    const ecs_rule_t *rule = it->rule;
    const ecs_rule_var_t *vars = rule->variables;
     
    ecs_rule_reg_t *regs = get_registers(it, it->op);
    ecs_entity_t *elem = ecs_vector_get(type, ecs_entity_t, column);
    ecs_assert(elem != NULL, ECS_INTERNAL_ERROR, NULL);

    int32_t lo_var = filter->lo_var;
    int32_t hi_var = filter->hi_var;

    if (lo_var != -1) {
        ecs_assert(vars[lo_var].kind == EcsRuleVarKindEntity, 
            ECS_INTERNAL_ERROR, NULL);

        entity_reg_set(rule, regs, lo_var, ecs_entity_t_lo(*elem));
    }

    if (hi_var != -1) {
        ecs_assert(vars[hi_var].kind == EcsRuleVarKindEntity, 
            ECS_INTERNAL_ERROR, NULL);            

        entity_reg_set(rule, regs, hi_var, 
            ecs_entity_t_hi(*elem & ECS_COMPONENT_MASK));
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
int32_t get_variable_depth(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    ecs_rule_var_t *root,
    int recur);

static
int32_t crawl_variable(
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

        /* Variable must at least appear once in term */
        if (var != pred && var != subj && var != obj) {
            continue;
        }

        if (pred && pred != var && !pred->marked) {
            get_variable_depth(rule, pred, root, recur + 1);
        }

        if (subj && subj != var && !subj->marked) {
            get_variable_depth(rule, subj, root, recur + 1);
        }

        if (obj && obj != var && !obj->marked) {
            get_variable_depth(rule, obj, root, recur + 1);
        }
    }

    return 0;
}

static
int32_t get_depth_from_var(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    ecs_rule_var_t *root,
    int recur)
{
    /* If variable is the root or if depth has been set, return depth + 1 */
    if (var == root || var->depth != UINT8_MAX) {
        return var->depth + 1;
    }

    /* Variable is already being evaluated, so this indicates a cycle. Stop */
    if (var->marked) {
        return 0;
    }
    
    /* Variable is not yet being evaluated and depth has not yet been set. 
     * Calculate depth. */
    int32_t depth = get_variable_depth(rule, var, root, recur + 1);
    if (depth == UINT8_MAX) {
        return depth;
    } else {
        return depth + 1;
    }
}

static
int32_t get_depth_from_term(
    ecs_rule_t *rule,
    ecs_rule_var_t *cur,
    ecs_rule_var_t *pred,
    ecs_rule_var_t *obj,
    ecs_rule_var_t *root,
    int recur)
{
    int32_t result = UINT8_MAX;

    ecs_assert(cur != pred || cur != obj, ECS_INTERNAL_ERROR, NULL);

    /* If neither of the other parts of the terms are variables, this
     * variable is guaranteed to have no dependencies. */
    if (!pred && !obj) {
        result = 0;
    } else {
        /* If this is a variable that is not the same as the current, 
         * we can use it to determine dependency depth. */
        if (pred && cur != pred) {
            int32_t depth = get_depth_from_var(rule, pred, root, recur);
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
            int32_t depth = get_depth_from_var(rule, obj, root, recur);
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
int32_t get_variable_depth(
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
    int32_t result = UINT8_MAX;

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

        int32_t depth = get_depth_from_term(rule, var, pred, obj, root, recur);
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
     * variables are found, loop again & follow predicate & object links */
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        ecs_rule_var_t 
        *subj = column_subj(rule, column),
        *pred = column_pred(rule, column),
        *obj = column_obj(rule, column);

        /* Only evaluate pred & obj for current subject. This ensures that we
         * won't evaluate variables that are unreachable from the root. This
         * must be detected as unconstrained variables are not allowed. */
        if (subj != var) {
            continue;
        }

        crawl_variable(rule, subj, root, recur);

        if (pred && pred != var) {
            crawl_variable(rule, pred, root, recur);
        }

        if (obj && obj != var) {
            crawl_variable(rule, obj, root, recur);
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
    int32_t subject_count = 0;

    /* If this (.) is found, it always takes precedence in root election */
    int32_t this_var = UINT8_MAX;

    /* Keep track of the subject variable that occurs the most. In the absence of
     * this (.) the variable with the most occurrences will be elected root. */
    int32_t max_occur = 0;
    int32_t max_occur_var = UINT8_MAX;

    /* Step 1: find all possible roots */
    ecs_sig_column_t *columns = ecs_vector_first(rule->sig.columns, ecs_sig_column_t);
    int32_t i, count = rule->column_count;
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        /* Validate if predicate does not have too many arguments */
        if (column->argc > 2) {
            rule_error(rule, "too many arguments for term %d", i);
            goto error;
        }

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
    int32_t root_var = this_var;
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

    /* Verify that there are no unconstrained variables. Unconstrained variables
     * are variables that are unreachable from the root. */
    for (int i = 0; i < rule->subject_variable_count; i ++) {
        if (rule->variables[i].depth == UINT8_MAX) {
            rule_error(rule, "unconstrained variable '%s'", 
                rule->variables[i].name);
            goto error;
        }
    }

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

/* Get entity variable from table variable */
static
ecs_rule_var_t* to_entity(
    ecs_rule_t *rule,
    ecs_rule_var_t *var)
{
    if (!var) {
        return NULL;
    }

    ecs_rule_var_t *evar = NULL;
    if (var->kind == EcsRuleVarKindTable) {
        evar = find_variable(rule, EcsRuleVarKindEntity, var->name);
    } else {
        evar = var;
    }

    return evar;
}

/* Ensure that if a table variable has been written, the corresponding entity
 * variable is populated. The function will return the most specific, populated
 * variable. */
static
ecs_rule_var_t* get_most_specific_var(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    bool *written)
{
    if (!var) {
        return NULL;
    }

    ecs_rule_var_t *tvar, *evar = to_entity(rule, var);
    if (!evar) {
        return var;
    }

    if (var->kind == EcsRuleVarKindTable) {
        tvar = var;
    } else {
        tvar = find_variable(rule, EcsRuleVarKindTable, var->name);
    }

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
        if (evar) {
            if (!written[evar->id]) {
                ecs_rule_op_t *op = create_operation(rule);
                op->kind = EcsRuleEach;
                op->on_pass = rule->operation_count;
                op->on_fail = rule->operation_count - 2;
                op->has_in = true;
                op->has_out = true;
                op->r_in = tvar->id;
                op->r_out = evar->id;

                /* Entity will either be written or has been written */
                written[evar->id] = true;
            }

            return evar;
        }
    } else if (evar && written[evar->id]) {
        return evar;
    }

    return var;
}

/* Ensure that an entity variable is written before using it */
static
ecs_rule_var_t* ensure_entity_written(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    int32_t column,
    bool *written)
{
    if (!var) {
        return NULL;
    }

    /* Ensure we're working with the most specific version of subj we can get */
    ecs_rule_var_t *evar = get_most_specific_var(rule, var, written);

    /* The post condition of this function is that there is an entity variable,
     * and that it is written. Make sure that the result is an entity */
    ecs_assert(evar != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(evar->kind == EcsRuleVarKindEntity, ECS_INTERNAL_ERROR, NULL);

    /* Make sure the variable has been written */
    ecs_assert(written[evar->id] == true, ECS_INTERNAL_ERROR, NULL);
    
    return evar;
}

static
ecs_rule_op_t* insert_operation(
    ecs_rule_t *rule,
    int32_t column_index,
    bool *written)
{
    ecs_rule_pair_t pair = {0};

    /* Parse the column's type into a pair. A pair extracts the ids from
     * the column, and replaces variables with wildcards which can then
     * be matched against actual relationships. A pair retains the 
     * information about the variables, so that when a match happens,
     * the pair can be used to reify the variable. */
    if (column_index != -1) {
        ecs_sig_column_t *column = ecs_vector_get(
            rule->sig.columns, ecs_sig_column_t, column_index);
        ecs_assert(column != NULL, ECS_INTERNAL_ERROR, NULL);    

        pair = column_to_pair(rule, column);

        /* If the pair contains entity variables that have not yet been written,
         * insert each instructions in case their tables are known. Variables in
         * a pair that are truly unknown will be populated by the operation,
         * but an operation should never overwrite an entity variable if the 
         * corresponding table variable has already been resolved. */
        if (pair.reg_mask & RULE_PAIR_PREDICATE) {
            ecs_rule_var_t *pred = &rule->variables[pair.pred];
            pred = get_most_specific_var(rule, pred, written);
            pair.pred = pred->id;
        }

        if (pair.reg_mask & RULE_PAIR_OBJECT) {
            ecs_rule_var_t *obj = &rule->variables[pair.obj];
            obj = get_most_specific_var(rule, obj, written);
            pair.obj = obj->id;
        }
    } else {
        /* Not all operations have a filter (like Each) */
    }

    ecs_rule_op_t *op = create_operation(rule);
    op->on_pass = rule->operation_count;
    op->on_fail = rule->operation_count - 2;
    op->param = pair;  

    /* Store corresponding signature column so we can correlate and
     * store the table columns with signature columns. */
    op->column = column_index;

    return op;
}

/* Insert first operation, which is always Input. This creates an entry in
 * the register stack for the initial state. */
static
void insert_input(
    ecs_rule_t *rule)
{
    ecs_rule_op_t *op = create_operation(rule);
    op->kind = EcsRuleInput;

    /* The first time Input is evaluated it goes to the next/first operation */
    op->on_pass = 1;

    /* When Input is evaluated with redo = true it will return false, which will
     * finish the program as op becomes -1. */
    op->on_fail = -1;  
}

/* Insert last operation, which is always Yield. When the program hits Yield,
 * data is returned to the application. */
static
void insert_yield(
    ecs_rule_t *rule)
{
    ecs_rule_op_t *op = create_operation(rule);
    op->kind = EcsRuleYield;
    op->has_in = true;
    op->on_fail = rule->operation_count - 2;
    /* Yield can only "fail" since it is the end of the program */

    /* Find variable associated with this. It is possible that the variable
     * exists both as a table and as an entity. This can happen when a rule
     * first selects a table for this, but then subsequently needs to evaluate
     * each entity in that table. In that case the yield instruction should
     * return the entity, so look for that first. */
    ecs_rule_var_t *var = find_variable(rule, EcsRuleVarKindEntity, ".");
    if (!var) {
        var = find_variable(rule, EcsRuleVarKindTable, ".");
    }

    /* If there is no this, there is nothing to yield. In that case the rule
     * simply returns true or false. */
    if (!var) {
        op->r_in = UINT8_MAX;
    } else {
        op->r_in = var->id;
    }
}

/* Return superset/subset including the root */
static
void insert_inclusive_set(
    ecs_rule_t *rule,
    ecs_rule_op_kind_t op_kind,
    ecs_rule_var_t *out,
    ecs_rule_pair_t param,
    ecs_rule_var_t *root,
    ecs_entity_t root_entity,
    int32_t c,
    bool *written)
{
    ecs_assert(out != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_assert((op_kind != EcsRuleSuperSet) || 
        out->kind == EcsRuleVarKindEntity, ECS_INTERNAL_ERROR, NULL);

    int32_t setjmp_lbl = rule->operation_count;
    int32_t store_lbl = setjmp_lbl + 1;
    int32_t set_lbl = setjmp_lbl + 2;
    int32_t next_op = setjmp_lbl + 4;
    int32_t prev_op = setjmp_lbl - 1;

    /* Insert 4 operations at once, so we don't have to worry about how
     * the instruction array reallocs */
    insert_operation(rule, -1, written);
    insert_operation(rule, -1, written);
    insert_operation(rule, -1, written);
    ecs_rule_op_t *op = insert_operation(rule, -1, written);

    ecs_rule_op_t *setjmp = op - 3;
    ecs_rule_op_t *store = op - 2;
    ecs_rule_op_t *set = op - 1;
    ecs_rule_op_t *jump = op;

    /* The SetJmp operation stores a conditional jump label that either
     * points to the Store or *Set operation */
    setjmp->kind = EcsRuleSetJmp;
    setjmp->on_pass = store_lbl;
    setjmp->on_fail = set_lbl;

    /* The Store operation yields the root of the subtree. After yielding,
     * this operation will fail and return to SetJmp, which will cause it
     * to switch to the *Set operation. */
    store->kind = EcsRuleStore;
    store->param.pred = param.pred;
    store->on_pass = next_op;
    store->on_fail = setjmp_lbl;
    store->has_in = true;
    store->has_out = true;
    store->r_out = out->id;
    store->column = c;

    /* If the object of the filter is not a variable, store literal */
    if (!root) {
        store->r_in = UINT8_MAX;
        store->subject = root_entity;
        store->param.obj = root_entity;
    } else {
        store->r_in = root->id;
        store->param.obj = root->id;
        store->param.reg_mask = RULE_PAIR_OBJECT;
    }

    /* This is either a SubSet or SuperSet operation */
    set->kind = op_kind;
    set->param.pred = param.pred;
    set->on_pass = next_op;
    set->on_fail = prev_op;
    set->has_out = true;
    set->r_out = out->id;
    set->column = c;

    if (!root) {
        set->param.obj = root_entity;
    } else {
        set->param.obj = root->id;
        set->param.reg_mask = RULE_PAIR_OBJECT;
    }

    /* The jump operation jumps to either the store or subset operation,
     * depending on whether the store operation already yielded. The 
     * operation is inserted last, so that the on_fail label of the next 
     * operation will point to it */
    jump->kind = EcsRuleJump;
    
    /* The pass/fail labels of the Jump operation are not used, since it
     * jumps to a variable location. Instead, the pass label is (ab)used to
     * store the label of the SetJmp operation, so that the jump can access
     * the label it needs to jump to from the setjmp op_ctx. */
    jump->on_pass = setjmp_lbl;
    jump->on_fail = -1;

    written[out->id] = true;
}

static
ecs_rule_var_t* store_inclusive_set(
    ecs_rule_t *rule,
    ecs_rule_op_kind_t op_kind,
    ecs_rule_pair_t param,
    ecs_rule_var_t *root,
    ecs_entity_t root_entity,
    int32_t c,
    bool *written)
{
    /* The subset operation returns tables */
    ecs_rule_var_kind_t var_kind = EcsRuleVarKindTable;

    int32_t root_id;
    if (root) {
        root_id = root->id;
    }

    /* The superset operation returns entities */
    if (op_kind == EcsRuleSuperSet) {
        var_kind = EcsRuleVarKindEntity;   
    }

    /* Create anonymous variable for storing the set */
    ecs_rule_var_t *av = create_anonymous_variable(rule, var_kind);
    ecs_rule_var_t *ave = NULL;

    /* If the variable kind is a table, also create an entity variable as the
     * result of the set operation should be returned as an entity */
    if (var_kind == EcsRuleVarKindTable) {
        ave = create_variable(rule, EcsRuleVarKindEntity, av->name);
        av = &rule->variables[ave->id - 1];
    }

    /* Since we added variables, refetch pointer to root, since it might have
     * moved if the variables table was reallocd */
    if (root) {
        root = &rule->variables[root_id];
    }

    /* Ensure we're using the most specific version of root */
    root = get_most_specific_var(rule, root, written);

    /* Generate the operations */
    insert_inclusive_set(
        rule, op_kind, av, param, root, root_entity, -1, written);

    /* Make sure to return entity variable, and that it is populated */
    return ensure_entity_written(rule, av, c, written);
}

static
bool is_known(
    ecs_rule_t *rule,
    ecs_rule_var_t *var,
    bool *written)
{
    if (!var) {
        return true;
    } else {
        return written[var->id];
    }
}

static
void set_input_to_subj(
    ecs_rule_op_t *op,
    ecs_sig_column_t *column,
    ecs_rule_var_t *var)
{
    op->has_in = true;
    if (!var) {
        op->r_in = UINT8_MAX;
        op->subject = column->argv[0].entity;
    } else {
        op->r_in = var->id;
    }
}

static
void set_output_to_subj(
    ecs_rule_op_t *op,
    ecs_sig_column_t *column,
    ecs_rule_var_t *var)
{
    op->has_out = true;
    if (!var) {
        op->r_out = UINT8_MAX;
        op->subject = column->argv[0].entity;
    } else {
        op->r_out = var->id;
    }
}

static
void insert_select_or_with(
    ecs_rule_t *rule,
    ecs_rule_op_t *op,
    ecs_sig_column_t *column,
    ecs_rule_var_t *subj,
    bool *written)
{
    ecs_rule_var_t *tvar = NULL, *evar = to_entity(rule, subj);
    if (subj && subj->kind == EcsRuleVarKindTable) {
        tvar = subj;
    }

    /* If entity variable is known and resolved, create with for it */
    if (evar && is_known(rule, evar, written)) {
        op->kind = EcsRuleWith;
        op->r_in = evar->id;
        set_input_to_subj(op, column, subj);

    /* If table variable is known and resolved, create with for it */
    } else if (tvar && is_known(rule, tvar, written)) {
        op->kind = EcsRuleWith;
        op->r_in = tvar->id;
        set_input_to_subj(op, column, subj);

    /* If subject is neither table nor entitiy, with operates on literal */        
    } else if (!tvar && !evar) {
        op->kind = EcsRuleWith;
        set_input_to_subj(op, column, subj);

    /* If subject is table or entity but not known, use select */
    } else {
        /* Subject must be NULL, since otherwise we would be writing to a
         * variable that is already known */
        ecs_assert(subj != NULL, ECS_INTERNAL_ERROR, NULL);
        op->kind = EcsRuleSelect;
        set_output_to_subj(op, column, subj);

        written[subj->id] = true;
    }

    if (op->param.reg_mask & RULE_PAIR_PREDICATE) {
        written[op->param.pred] = true;
    }

    if (op->param.reg_mask & RULE_PAIR_OBJECT) {
        written[op->param.obj] = true;
    }
}

static
void insert_nonfinal_select_or_with(
    ecs_rule_t *rule,
    ecs_sig_column_t *column,
    ecs_rule_pair_t param,
    ecs_rule_var_t *subj,
    int32_t c,
    bool *written)    
{
    ecs_assert(!param.final, ECS_INTERNAL_ERROR, NULL);
    
    int32_t subj_id;
    if (subj) {
        subj_id = subj->id;
    }

    /* If predicate is not final, evaluate with all subsets of predicate.
     * Create a param with only the predicate set. */
    ecs_rule_pair_t pred_param;
    pred_param.pred = EcsIsA;
    pred_param.obj = param.pred;
    pred_param.reg_mask = 0;
    ecs_rule_var_t *pred_subsets = store_inclusive_set(
        rule, EcsRuleSubSet, pred_param, NULL, param.pred, c, written);
    
    /* Refetch subject variable, as variable array may have reallocated */
    if (subj) {
        subj = &rule->variables[subj_id];
    }

    /*  Make sure to use the most specific version of the object */
    if (param.reg_mask & RULE_PAIR_OBJECT) {
        ecs_rule_var_t *obj = &rule->variables[param.obj];
        obj = get_most_specific_var(rule, obj, written);
    }    

    ecs_rule_op_t *op = insert_operation(rule, -1, written);

    /* Use subset variable for predicate */
    op->param.pred = pred_subsets->id;
    op->param.obj = param.obj;
    op->param.reg_mask = param.reg_mask | RULE_PAIR_PREDICATE;

    /* Associate last operation with column to ensure that the resolved
     * component id gets written. */
    op->column = c;

    insert_select_or_with(rule, op, column, subj, written);
}

static
void insert_term_2(
    ecs_rule_t *rule,
    ecs_sig_column_t *column,
    int32_t c,
    bool *written)
{
    ecs_rule_var_t *pred = column_pred(rule, column);
    ecs_rule_var_t *subj = column_subj(rule, column);
    ecs_rule_var_t *obj = column_obj(rule, column);
    ecs_rule_pair_t param = column_to_pair(rule, column);

    int32_t subj_id;
    if (subj) {
        subj_id = subj->id;
    }

    int32_t obj_id;
    if (obj){
        obj_id = obj->id;
    }

    /* Ensure we're working with the most specific version of subj we can get */
    subj = get_most_specific_var(rule, subj, written);

    if (pred || (param.final && !param.transitive)) {
        ecs_rule_op_t *op = insert_operation(rule, c, written);
        insert_select_or_with(rule, op, column, subj, written);

    } else if (!param.final) {
        insert_nonfinal_select_or_with(rule, column, param, subj, c, written);
    } else if (param.transitive) {
        if (is_known(rule, subj, written)) {
            if (is_known(rule, obj, written)) {
                ecs_rule_var_t *obj_subsets = store_inclusive_set(
                    rule, EcsRuleSubSet, param, obj, 
                    column->argv[1].entity, c, written);

                if (subj) {
                    subj = &rule->variables[subj_id];

                    /* Try to resolve subj as entity again */
                    if (subj->kind == EcsRuleVarKindTable) {
                        subj = get_most_specific_var(rule, subj, written);
                    }
                }

                ecs_rule_op_t *op = insert_operation(rule, c, written);
                op->kind = EcsRuleWith;
                set_input_to_subj(op, column, subj);

                /* Use subset variable for object */
                op->param.obj = obj_subsets->id;
                op->param.reg_mask = RULE_PAIR_OBJECT;
            } else {
                ecs_assert(obj != NULL, ECS_INTERNAL_ERROR, NULL);

                obj = to_entity(rule, obj);

                insert_inclusive_set(
                    rule, EcsRuleSuperSet, obj, param, subj, 
                    column->argv[0].entity, c, written);
            }
        } else {
            ecs_assert(subj != NULL, ECS_INTERNAL_ERROR, NULL);

            if (is_known(rule, obj, written)) {
                /* Object variable is known, but this does not guarantee that
                 * we are working with the entity. Make sure that we get (and
                 * populate) the entity variable, as insert_inclusive_set does
                 * not do this */
                obj = get_most_specific_var(rule, obj, written);

                insert_inclusive_set(
                    rule, EcsRuleSubSet, subj, param, obj, 
                    column->argv[1].entity, c, written);
            } else {
                ecs_assert(obj != NULL, ECS_INTERNAL_ERROR, NULL);

                ecs_rule_var_t *av = create_anonymous_variable(
                    rule, EcsRuleVarKindEntity);

                subj = &rule->variables[subj_id];
                obj = &rule->variables[obj_id];

                /* TODO: this instruction currently does not return inclusive
                 * results. For example, it will return IsA(XWing, Machine) and
                 * IsA(XWing, Thing), but not IsA(XWing, XWing). To enable
                 * inclusive behavior, we need to be able to find all subjects
                 * that have IsA relationships, without expanding to all
                 * IsA relationships. For this a new mode needs to be supported
                 * where an operation never does a redo.
                 *
                 * This select can then be used to find all subjects, and those
                 * same subjects can then be used to find all (inclusive) 
                 * supersets for those subjects. */

                /* Insert instruction to find all subjects and objects */
                ecs_rule_op_t *op = insert_operation(rule, -1, written);
                op->kind = EcsRuleSelect;
                set_output_to_subj(op, column, subj);

                /* Set object to anonymous variable */
                op->param.pred = param.pred;
                op->param.obj = av->id;
                op->param.reg_mask = param.reg_mask | RULE_PAIR_OBJECT;

                written[subj->id] = true;
                written[av->id] = true;

                /* Insert superset instruction to find all supersets */
                insert_inclusive_set(
                    rule, EcsRuleSuperSet, obj, op->param, av,
                    0, c, written);

            }
        }
    }
}

static
void insert_term_1(
    ecs_rule_t *rule,
    ecs_sig_column_t *column,
    int32_t c,
    bool *written)
{
    ecs_rule_var_t *pred = column_pred(rule, column);
    ecs_rule_var_t *subj = column_subj(rule, column);
    ecs_rule_pair_t param = column_to_pair(rule, column);

    /* Ensure we're working with the most specific version of subj we can get */
    subj = get_most_specific_var(rule, subj, written);

    if (pred || param.final) {
        ecs_rule_op_t *op = insert_operation(rule, c, written);
        insert_select_or_with(rule, op, column, subj, written);      
    } else {
        insert_nonfinal_select_or_with(rule, column, param, subj, c, written);
    }
}

static
void insert_term(
    ecs_rule_t *rule,
    ecs_sig_column_t *column,
    int32_t c,
    bool *written)
{
    if (column->argc == 1) {
        insert_term_1(rule, column, c, written);
    } else if (column->argc == 2) {
        insert_term_2(rule, column, c, written);
    }
}

/* Create program from operations that will execute the query */
static
void compile_program(
    ecs_world_t *world,
    ecs_rule_t *rule)
{
    /* Trace which variables have been written while inserting instructions.
     * This determines which instruction needs to be inserted */
    bool written[ECS_RULE_MAX_VARIABLE_COUNT] = { false };

    ecs_sig_t *sig = &rule->sig;
    ecs_sig_column_t *columns = ecs_vector_first(sig->columns, ecs_sig_column_t);
    int32_t v, c, column_count = ecs_vector_count(sig->columns);
    ecs_rule_op_t *op;

    /* Insert input, which is always the first instruction */
    insert_input(rule);

    /* First insert all instructions that do not have a variable subject. Such
     * instructions iterate the type of an entity literal and are usually good
     * candidates for quickly narrowing down the set of potential results. */
    for (c = 0; c < column_count; c ++) {
        ecs_sig_column_t *column = &columns[c];
        ecs_rule_var_t* subj = column_subj(rule, column);
        if (subj) {
            continue;
        }

        insert_term(rule, column, c, written);
    }

    /* Insert variables based on dependency order */
    for (v = 0; v < rule->subject_variable_count; v ++) {
        ecs_rule_var_t *var = &rule->variables[v];

        ecs_assert(var->kind == EcsRuleVarKindTable, ECS_INTERNAL_ERROR, NULL);

        for (c = 0; c < column_count; c ++) {
            ecs_sig_column_t *column = &columns[c];

            /* Only process columns for which variable is subject */
            ecs_rule_var_t* subj = column_subj(rule, column);
            if (subj != var) {
                continue;
            }

            insert_term(rule, column, c, written);

            var = &rule->variables[v];
        }
    }

    /* Verify all subject variables have been written. Subject variables are of
     * the table type, and a select/subset should have been inserted for each */
    for (v = 0; v < rule->subject_variable_count; v ++) {
        if (!written[v]) {
            /* If the table variable hasn't been written, this can only happen
             * if an instruction wrote the variable before a select/subset could
             * have been inserted for it. Make sure that this is the case by
             * testing if an entity variable exists and whether it has been
             * written. */
            ecs_rule_var_t *var = find_variable(
                rule, EcsRuleVarKindEntity, rule->variables[v].name);
            ecs_assert(written[var->id], ECS_INTERNAL_ERROR, var->name);
        }
    }

    /* Make sure that all entity variables are written. With the exception of
     * the this variable, which can be returned as a table, other variables need
     * to be available as entities. This ensures that all permutations for all
     * variables are correctly returned by the iterator. When an entity variable
     * hasn't been written yet at this point, it is because it only constrained
     * through a common predicate or object. */
    for (; v < rule->variable_count; v ++) {
        if (!written[v]) {
            ecs_rule_var_t *var = &rule->variables[v];
            ecs_assert(var->kind == EcsRuleVarKindEntity, 
                ECS_INTERNAL_ERROR, NULL);

            ecs_rule_var_t *table_var = find_variable(
                rule, EcsRuleVarKindTable, var->name);
            
            /* A table variable must exist if the variable hasn't been resolved
             * yet. If there doesn't exist one, this could indicate an 
             * unconstrained variable which should have been caught earlier */
            ecs_assert(table_var != NULL, ECS_INTERNAL_ERROR, var->name);

            /* Insert each operation that takes the table variable as input, and
             * yields each entity in the table */
            op = insert_operation(rule, -1, written);
            op->kind = EcsRuleEach;
            op->r_in = table_var->id;
            op->r_out = var->id;
            op->has_in = true;
            op->has_out = true;
            written[var->id] = true;
        }
    }     

    /* Insert yield, which is always the last operation */
    insert_yield(rule);
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

    result->world = world;
    result->column_count = ecs_vector_count(result->sig.columns);

    /* Find all variables & resolve dependencies */
    if (scan_variables(result) != 0) {
        goto error;
    }

    compile_program(world, result);

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
    int32_t var_id)
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

        ecs_strbuf_append(&buf, "%2d: [P:%2d, F:%2d] ", i, 
            op->on_pass, op->on_fail);

        bool has_filter = false;

        switch(op->kind) {
        case EcsRuleSelect:
            ecs_strbuf_append(&buf, "select   ");
            has_filter = true;
            break;
        case EcsRuleWith:
            ecs_strbuf_append(&buf, "with     ");
            has_filter = true;
            break;
        case EcsRuleStore:
            ecs_strbuf_append(&buf, "store    ");
            break;
        case EcsRuleSuperSet:
            ecs_strbuf_append(&buf, "superset ");
            has_filter = true;
            break;             
        case EcsRuleSubSet:
            ecs_strbuf_append(&buf, "subset   ");
            has_filter = true;
            break;            
        case EcsRuleEach:
            ecs_strbuf_append(&buf, "each     ");
            break;
        case EcsRuleSetJmp:
            ecs_strbuf_append(&buf, "setjmp   ");
            break;
        case EcsRuleJump:
            ecs_strbuf_append(&buf, "jump     ");
            break;            
        case EcsRuleYield:
            ecs_strbuf_append(&buf, "yield    ");
            break;
        default:
            continue;
        }

        if (op->has_in) {
            ecs_rule_var_t *r_in = get_variable(rule, op->r_in);
            if (r_in) {
                ecs_strbuf_append(&buf, "I:%s%s ", 
                    r_in->kind == EcsRuleVarKindTable ? "t" : "",
                    r_in->name);
            } else if (op->subject) {
                ecs_strbuf_append(&buf, "I:%s ", 
                    ecs_get_name(rule->world, op->subject));
            }
        }

        if (op->has_out) {
            ecs_rule_var_t *r_out = get_variable(rule, op->r_out);
            if (r_out) {
                ecs_strbuf_append(&buf, "O:%s%s ", 
                    r_out->kind == EcsRuleVarKindTable ? "t" : "",
                    r_out->name);
            } else if (op->subject) {
                ecs_strbuf_append(&buf, "O:%s ", 
                    ecs_get_name(rule->world, op->subject));
            }
        }

        if (has_filter) {
            if (!object) {
                sprintf(filter_expr, "(%s)", type_name);
            } else {
                sprintf(filter_expr, "(%s, %s)", type_name, object_name);
            }
            ecs_strbuf_append(&buf, "F:%s", filter_expr);
        }

        ecs_strbuf_appendstr(&buf, "\n");
    }

    return ecs_strbuf_get(&buf);
}

/* Public function that returns number of terms. */
int32_t ecs_rule_term_count(
    const ecs_rule_t *rule)
{
    ecs_assert(rule != NULL, ECS_INTERNAL_ERROR, NULL);
    return rule->column_count;
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
        ecs_rule_reg_t *regs = get_registers(it, it->rule->operation_count - 1);
        return entity_reg_get(it->rule, regs, var_id);
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
        
        it->op_ctx = ecs_os_calloc(rule->operation_count * 
            ECS_SIZEOF(ecs_rule_op_ctx_t));

        if (rule->column_count) {
            it->columns = ecs_os_malloc(rule->operation_count * 
                rule->column_count * ECS_SIZEOF(int32_t));
        }
    }

    it->op = 0;

    int i;
    for (i = 0; i < rule->variable_count; i ++) {
        it->registers[i].var_id = i;
        if (rule->variables[i].kind == EcsRuleVarKindEntity) {
            entity_reg_set(rule, it->registers, i, EcsWildcard);
        } else {
            table_reg_set(rule, it->registers, i, NULL);
        }
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

/* This function iterates a type with a provided pair expression, as is returned
 * by pair_get_most_specific_var. It starts looking in the type at an offset ('column') and
 * returns the first matching element. */
static
int32_t find_next_match(
    ecs_type_t type, 
    int32_t column,
    ecs_rule_filter_t *filter)
{
    /* Scan the type for the next match */
    int32_t i, count = ecs_vector_count(type);
    ecs_entity_t *entities = ecs_vector_first(type, ecs_entity_t);

    /* If the predicate is not a wildcard, the next element must match the 
     * queried for entity, or the type won't contain any more matches. The
     * reason for this is that ids in a type are sorted, and the predicate
     * occupies the most significant bits in the type */
    if (!filter->pred_wildcard) {
        /* Evaluate at most one element if column is not 0. If column is 0,
         * the entire type is evaluated. */
        if (column && column < count) {
            count = column + 1;
        }
    }

    /* Find next column that equals look_for after masking out the wildcards */
    ecs_entity_t expr_mask = filter->expr_mask;
    ecs_entity_t expr_match = filter->expr_match;

    for (i = column; i < count; i ++) {
        if ((entities[i] & expr_mask) == expr_match) {
            if (filter->same_var) {
                ecs_entity_t lo_id = ecs_entity_t_lo(entities[i]);
                ecs_entity_t hi_id = ecs_entity_t_hi(
                    entities[i] & ECS_COMPONENT_MASK);

                /* If pair contains the same variable twice but the matched id
                 * has different values, this is not a match */
                if (lo_id != hi_id) {
                    continue;
                }
            }

            return i;
        }
    }

    /* No matching columns were found in remainder of type */
    return -1;
}

/* This function finds the next table in a table set, and is used by the select
 * operation. The function automatically skips empty tables, so that subsequent
 * operations don't waste a lot of processing for nothing. */
static
ecs_table_record_t find_next_table(
    ecs_sparse_t *table_set,
    ecs_rule_filter_t *filter,
    ecs_rule_with_ctx_t *op_ctx)
{
    ecs_table_record_t *table_record;
    ecs_table_t *table;
    int32_t count, column;

    /* If the current index is higher than the number of tables in the table
     * set, we've exhausted all matching tables. */
    if (op_ctx->table_index >= ecs_sparse_count(table_set)) {
        return (ecs_table_record_t){0};
    }

    /* Find the next non-empty table */
    do {
        op_ctx->table_index ++;

        table_record = ecs_sparse_get(
            table_set, ecs_table_record_t, op_ctx->table_index);
        if (!table_record) {
            return (ecs_table_record_t){0};
        }

        table = table_record->table;
        count = ecs_table_count(table);
        if (!count) {
            column = -1;
            continue;
        }

        column = find_next_match(table->type, table_record->column, filter);
    } while (column == -1);

    return (ecs_table_record_t){.table = table, .column = column};
}

static
ecs_sparse_t* find_table_set(
    ecs_world_t *world,
    ecs_entity_t mask)
{
    return ecs_map_get_ptr(
        world->store.table_index, ecs_sparse_t*, mask);
}

static
ecs_entity_t rule_get_column(
    ecs_type_t type,
    int32_t column)
{
    ecs_entity_t *comp = ecs_vector_get(type, ecs_entity_t, column);
    ecs_assert(comp != NULL, ECS_INTERNAL_ERROR, NULL);
    return *comp;
}

static
ecs_entity_t set_column(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    ecs_type_t type,
    int32_t column)
{
    if (op->column == -1) {
        return -1;
    }

    if (type) {
        return it->table.components[op->column] = rule_get_column(type, column);
    } else {
        return it->table.components[op->column] = 0;
    }
}

/* Input operation. The input operation acts as a placeholder for the start of
 * the program, and creates an entry in the register array that can serve to
 * store variables passed to an iterator. */
static
bool eval_input(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
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
bool eval_superset(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
    bool redo)
{
    const ecs_rule_t  *rule = it->rule;
    ecs_world_t *world = rule->world;
    ecs_rule_superset_ctx_t *op_ctx = &it->op_ctx[op_index].is.superset;
    ecs_rule_superset_frame_t *frame = NULL;
    ecs_table_record_t table_record;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for output */
    int32_t sp, row;
    int32_t r = op->r_out;

    /* Register cannot be a literal, since we need to store things in it */
    ecs_assert(r != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);

    /* Superset results are always stored in an entity variable */
    ecs_assert(rule->variables[regs[r].var_id].kind == EcsRuleVarKindEntity,    
        ECS_INTERNAL_ERROR, NULL);

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param;
    ecs_rule_filter_t filter = pair_to_filter(it, pair);
    ecs_sparse_t *table_set;
    ecs_table_t *table = NULL;

    if (!redo) {
        op_ctx->stack = op_ctx->storage;
        sp = op_ctx->sp = 0;
        frame = &op_ctx->stack[sp];

        ecs_entity_t mask = ecs_trait(EcsWildcard, pair.pred);
        table_set = op_ctx->table_set = find_table_set(world, mask);

        /* If no table set is found for the transitive relationship, there are 
         * no supersets */
        if (!table_set) {
            return false;
        }

        /* Get table of object for which to get supersets */
        ecs_entity_t obj = ecs_entity_t_lo(filter.mask);

        /* If obj is wildcard, there's nothing to determine a superset for */
        ecs_assert(obj != EcsWildcard, ECS_INTERNAL_ERROR, NULL);

        /* Find first matching column in table */
        table = table_from_entity(world, obj);
        filter.mask = mask;
        set_filter_expr_mask(&filter, mask);
        int32_t column = find_next_match(table->type, 0, &filter);

        /* If no matching column was found, there are no supersets */
        if (column == -1) {
            return false;
        }

        ecs_entity_t col_entity = rule_get_column(table->type, column);
        ecs_entity_t col_obj = ecs_entity_t_lo(col_entity);

        entity_reg_set(rule, regs, r, col_obj);
        set_column(it, op, table->type, column);

        frame->table = table;
        frame->column = column;

        return true;
    }

    table_set = op_ctx->table_set;
    sp = op_ctx->sp;
    frame = &op_ctx->stack[sp];
    table = frame->table;
    int32_t column = frame->column;

    ecs_entity_t mask = ecs_trait(EcsWildcard, pair.pred);
    filter.mask = mask;
    set_filter_expr_mask(&filter, mask);

    ecs_entity_t col_entity = rule_get_column(table->type, column);
    ecs_entity_t col_obj = ecs_entity_t_lo(col_entity);
    ecs_table_t *next_table = table_from_entity(world, col_obj);

    if (next_table) {
        sp ++;
        frame = &op_ctx->stack[sp];
        frame->table = next_table;
        frame->column = -1;
    }

    do {
        frame = &op_ctx->stack[sp];
        table = frame->table;
        column = frame->column;

        column = find_next_match(table->type, column + 1, &filter);
        if (column != -1) {
            op_ctx->sp = sp;
            frame->column = column;
            col_entity = rule_get_column(table->type, column);
            col_obj = ecs_entity_t_lo(col_entity);

            entity_reg_set(rule, regs, r, col_obj);
            set_column(it, op, table->type, column);

            return true;        
        }

        sp --;
    } while (sp >= 0);

    return false;
}

static
bool eval_subset(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
    bool redo)
{
    const ecs_rule_t  *rule = it->rule;
    ecs_world_t *world = rule->world;
    ecs_rule_subset_ctx_t *op_ctx = &it->op_ctx[op_index].is.subset;
    ecs_rule_subset_frame_t *frame = NULL;
    ecs_table_record_t table_record;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for output */
    int32_t sp, row;
    int32_t r = op->r_out;
    ecs_assert(r != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param;
    ecs_rule_filter_t filter = pair_to_filter(it, pair);
    ecs_sparse_t *table_set;
    ecs_table_t *table = NULL;

    if (!redo) {
        op_ctx->stack = op_ctx->storage;
        sp = op_ctx->sp = 0;
        frame = &op_ctx->stack[sp];
        table_set = frame->with_ctx.table_set = find_table_set(
            world, filter.mask);
        
        /* If no table set could be found for expression, yield nothing */
        if (!table_set) {
            return false;
        }

        frame->with_ctx.table_index = -1;
        table_record = find_next_table(table_set, &filter, &frame->with_ctx);
        
        /* If first table set has no non-empty table, yield nothing */
        if (!table_record.table) {
            return false;
        }

        frame->row = 0;
        frame->column = table_record.column;
        table_reg_set(rule, regs, r, (frame->table = table_record.table));
        set_column(it, op, table_record.table->type, table_record.column);
        return true;
    }

    do {
        sp = op_ctx->sp;
        frame = &op_ctx->stack[sp];
        table = frame->table;
        table_set = frame->with_ctx.table_set;
        row = frame->row;

        /* If row exceeds number of elements in table, find next table in frame that
         * still has entities */
        while ((sp >= 0) && (row >= ecs_table_count(table))) {
            table_record = find_next_table(table_set, &filter, &frame->with_ctx);

            if (table_record.table) {
                table = frame->table = table_record.table;
                ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
                row = frame->row = 0;
                frame->column = table_record.column;
                set_column(it, op, table_record.table->type, table_record.column);
                table_reg_set(rule, regs, r, table);
                return true;
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

                ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
                ecs_assert(table_set != NULL, ECS_INTERNAL_ERROR, NULL);
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
            filter = pair_to_filter(it, pair);

            /* Find table set for expression */
            table = NULL;
            table_set = find_table_set(world, filter.mask);

            /* If table set is found, find first non-empty table */
            if (table_set) {
                ecs_rule_subset_frame_t *new_frame = &op_ctx->stack[sp + 1];
                new_frame->with_ctx.table_set = table_set;
                new_frame->with_ctx.table_index = -1;
                table_record = find_next_table(table_set, &filter, &new_frame->with_ctx);

                /* If set contains non-empty table, push it to stack */
                if (table_record.table) {
                    table = table_record.table;
                    op_ctx->sp ++;
                    new_frame->table = table;
                    new_frame->row = 0;
                    new_frame->column = table_record.column;
                    frame = new_frame;
                }
            }

            /* If no table was found for the current entity, advance row */
            if (!table) {
                row = ++ frame->row;
            }
        } while (!table && row < row_count);
    } while (!table);

    table_reg_set(rule, regs, r, table);
    set_column(it, op, table->type, frame->column);

    return true;
}

/* Select operation. The select operation finds and iterates a table set that
 * corresponds to its pair expression.  */
static
bool eval_select(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
    bool redo)
{
    const ecs_rule_t  *rule = it->rule;
    ecs_world_t *world = rule->world;
    ecs_rule_with_ctx_t *op_ctx = &it->op_ctx[op_index].is.with;
    ecs_table_record_t table_record;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for output */
    int32_t r = op->r_out;
    ecs_assert(r != UINT8_MAX, ECS_INTERNAL_ERROR, NULL);

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param;
    ecs_rule_filter_t filter = pair_to_filter(it, pair);

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

        table_set = op_ctx->table_set = find_table_set(world, filter.mask);
    }

    /* If no table set was found for queried for entity, there are no results */
    if (!table_set) {
        return false;
    }

    int32_t *columns = rule_get_columns(it, op_index);

    /* If this is not a redo, start at the beginning */
    if (!redo) {
        op_ctx->table_index = -1;

        /* Return the first table_record in the table set. */
        table_record = find_next_table(table_set, &filter, op_ctx);
    
        /* If no table record was found, there are no results. */
        if (!table_record.table) {
            return false;
        }

        table = table_record.table;

        /* Set current column to first occurrence of queried for entity */
        column = columns[op->column] = table_record.column;

        /* Store table in register */
        table_reg_set(rule, regs, r, table);
    
    /* If this is a redo, progress to the next match */
    } else {
        /* First test if there are any more matches for the current table, in 
         * case we're looking for a wildcard. */
        if (filter.wildcard) {
            table = table_reg_get(rule, regs, r);
            ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

            column = columns[op->column];
            column = find_next_match(table->type, column + 1, &filter);

            columns[op->column] = column;
        }

        /* If no next match was found for this table, move to next table */
        if (column == -1) {
            table_record = find_next_table(table_set, &filter, op_ctx);
            if (!table_record.table) {
                return false;
            }

            /* Assign new table to table register */
            table_reg_set(rule, regs, r, (table = table_record.table));

            /* Assign first matching column */
            column = columns[op->column] = table_record.column;
        }
    }

    /* If we got here, we found a match. Table and column must be set */
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(column != -1, ECS_INTERNAL_ERROR, NULL);

    /* If this is a wildcard query, fill out the variable registers */
    if (filter.wildcard) {
        reify_variables(it, &filter, table->type, column);
    }
    
    set_column(it, op, table->type, column);

    return true;
}

/* With operation. The With operation always comes after either the Select or
 * another With operation, and applies additional filters to the table. */
static
bool eval_with(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
    bool redo)
{
    const ecs_rule_t *rule = it->rule;
    ecs_world_t *world = rule->world;
    ecs_rule_with_ctx_t *op_ctx = &it->op_ctx[op_index].is.with;
    ecs_table_record_t *table_record = NULL;
    ecs_rule_reg_t *regs = get_registers(it, op_index);

    /* Get register indices for input */
    int32_t r = op->r_in;

    /* Get queried for id, fill out potential variables */
    ecs_rule_pair_t pair = op->param;
    ecs_rule_filter_t filter = pair_to_filter(it, pair);

    /* If looked for entity is not a wildcard (meaning there are no unknown/
     * unconstrained variables) and this is a redo, nothing more to yield. */
    if (redo && !filter.wildcard) {
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
        /* Transitive queries are inclusive, which means that if we have a
         * transitive predicate which is provided with the same subject and
         * object, it should return true. By default with will not return true
         * as the subject likely does not have itself as a relationship, which
         * is why this is a special case. 
         *
         * TODO: might want to move this code to a separate with_inclusive
         * instruction to limit branches for non-transitive queries (and to keep
         * code more readable).
         */
        if (pair.transitive) {
            ecs_entity_t subj = 0, obj = 0;
            
            if (r == UINT8_MAX) {
                subj = op->subject;
            } else {
                ecs_rule_var_t *v_subj = &rule->variables[r];
                if (v_subj->kind == EcsRuleVarKindEntity) {
                    subj = entity_reg_get(rule, regs, r);

                    /* This is the input for the op, so should always be set */
                    ecs_assert(subj != 0, ECS_INTERNAL_ERROR, NULL);
                }
            }

            /* If subj is set, it means that it is an entity. Try to also 
             * resolve the object. */
            if (subj) {
                /* If the object is not a wildcard, it has been reified. Get the
                 * value from either the register or as a literal */
                if (!filter.obj_wildcard) {
                    obj = ecs_entity_t_lo(filter.mask);
                    if (subj == obj) {
                        it->table.components[op->column] = filter.mask;
                        return true;
                    }
                }
            }
        }

        /* The With operation finds the table set that belongs to its pair
         * filter. The table set is a sparse set that provides an O(1) operation
         * to check whether the current table has the required expression. */
        table_set = op_ctx->table_set = find_table_set(world, filter.mask);
    }

    /* If no table set was found for queried for entity, there are no results. 
     * If this result is a transitive query, the table we're evaluating may not
     * be in the returned table set. Regardless, if the filter that contains a
     * transitive predicate does not have any tables associated with it, there
     * can be no transitive matches for the filter.  */
    if (!table_set) {
        return false;
    }

    int32_t *columns = rule_get_columns(it, op_index);
    int32_t new_column = -1;

    /* If this is not a redo, start at the beginning */
    if (!redo) {
        table = reg_get_table(rule, op, regs, r);
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
        } else {
            ecs_assert(table == table_record->table, ECS_INTERNAL_ERROR, NULL);

            /* Set current column to first occurrence of queried for entity */
            column = table_record->column;
            new_column = find_next_match(table->type, column, &filter);
        }
    
    /* If this is a redo, progress to the next match */
    } else {
        table = reg_get_table(rule, op, regs, r);
        
        /* First test if there are any more matches for the current table, in 
         * case we're looking for a wildcard. */
        if (filter.wildcard) {
            if (!table) {
                return false;
            }

            /* Find the next match for the expression in the column. The columns
             * array keeps track of the state for each With operation, so that
             * even after redoing a With, the search doesn't have to start from
             * the beginning. */
            column = columns[op->column] + 1;
            new_column = find_next_match(table->type, column, &filter);
        }
    }

    /* If no next match was found for this table, no more data */
    if (new_column == -1) {
        return false;
    }

    column = columns[op->column] = new_column;

    /* If we got here, we found a match. Table and column must be set */
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(column != -1, ECS_INTERNAL_ERROR, NULL);

    /* If this is a wildcard query, fill out the variable registers */
    if (filter.wildcard) {
        reify_variables(it, &filter, table->type, column);
    }

    set_column(it, op, table->type, column);

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
    int32_t op_index,
    bool redo)
{
    ecs_rule_each_ctx_t *op_ctx = &it->op_ctx[op_index].is.each;
    ecs_rule_reg_t *regs = get_registers(it, op_index);
    int32_t r_in = op->r_in;
    int32_t r_out = op->r_out;
    int32_t row;

    /* Make sure in/out registers are of the correct kind */
    ecs_assert(it->rule->variables[r_in].kind == EcsRuleVarKindTable, 
        ECS_INTERNAL_ERROR, NULL);
    ecs_assert(it->rule->variables[r_out].kind == EcsRuleVarKindEntity, 
        ECS_INTERNAL_ERROR, NULL);

    /* Get table, make sure that it contains data. The select operation should
     * ensure that empty tables are never forwarded. */
    ecs_table_t *table = table_reg_get(it->rule, regs, r_in);
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_data_t *data = ecs_table_get_data(table);
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);
    
    int32_t count = regs[r_in].is.table.count;
    int32_t offset = regs[r_in].is.table.offset;
    if (!count) {
        count = ecs_table_data_count(data);
        ecs_assert(count != 0, ECS_INTERNAL_ERROR, NULL);
    } else {
        count += offset;
    }

    ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);
    ecs_assert(entities != NULL, ECS_INTERNAL_ERROR, NULL);

    /* If this is is not a redo, start from row 0, otherwise go to the
     * next entity. */
    if (!redo) {
        row = op_ctx->row = offset;
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
    entity_reg_set(it->rule, regs, r_out, e);

    return true;
}

/* Store operation. Stores entity in register. This can either be an entity 
 * literal or an entity variable that will be stored in a table register. The
 * latter facilitates scenarios where an iterator only need to return a single
 * entity but where the Yield returns tables. */
static
bool eval_store(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
    bool redo)
{
    if (redo) {
        /* Only ever return result once */
        return false;
    }

    const ecs_rule_t *rule = it->rule;
    ecs_rule_reg_t *regs = get_registers(it, op_index);
    int32_t r_in = op->r_in;
    int32_t r_out = op->r_out;

    ecs_entity_t e = reg_get_entity(rule, op, regs, r_in);
    reg_set_entity(rule, regs, r_out, e);

    if (op->column >= 0) {
        ecs_rule_filter_t filter = pair_to_filter(it, op->param);
        it->table.components[op->column] = filter.mask;
    }

    return true;
}

/* A setjmp operation sets the jump label for a subsequent jump label. When the
 * operation is first evaluated (redo=false) it sets the label to the on_pass
 * label, and returns true. When the operation is evaluated again (redo=true)
 * the label is set to on_fail and the operation returns false. */
static
bool eval_setjmp(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
    bool redo)
{
    ecs_rule_setjmp_ctx_t *ctx = &it->op_ctx[op_index].is.setjmp;

    if (!redo) {
        ctx->label = op->on_pass;
        return true;
    } else {
        ctx->label = op->on_fail;
        return false;
    }
}

/* The jump operation jumps to an operation label. The operation always returns
 * true. Since the operation modifies the control flow of the program directly, 
 * the dispatcher does not look at the on_pass or on_fail labels of the jump
 * instruction. Instead, the on_pass label is used to store the label of the
 * operation that contains the label to jump to. */
static
bool eval_jump(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
    bool redo)
{
    /* Passthrough, result is not used for control flow */
    return !redo;
}

/* Yield operation. This is the simplest operation, as all it does is return
 * false. This will move the solver back to the previous instruction which
 * forces redo's on previous operations, for as long as there are matching
 * results. */
static
bool eval_yield(
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index,
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
    int32_t op_index,
    bool redo)
{
    switch(op->kind) {
    case EcsRuleInput:
        return eval_input(it, op, op_index, redo);
    case EcsRuleSelect:
        return eval_select(it, op, op_index, redo);
    case EcsRuleWith:
        return eval_with(it, op, op_index, redo);
    case EcsRuleSubSet:
        return eval_subset(it, op, op_index, redo);
    case EcsRuleSuperSet:
        return eval_superset(it, op, op_index, redo);
    case EcsRuleEach:
        return eval_each(it, op, op_index, redo);
    case EcsRuleStore:
        return eval_store(it, op, op_index, redo);
    case EcsRuleSetJmp:
        return eval_setjmp(it, op, op_index, redo);
    case EcsRuleJump:
        return eval_jump(it, op, op_index, redo);
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
    if (!it->rule->variable_count) {
        return;
    }

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
    if (!it->rule->column_count) {
        return;
    }

    int32_t *src_cols = rule_get_columns(it, cur);
    int32_t *dst_cols = rule_get_columns(it, next);

    memcpy(dst_cols, src_cols, ECS_SIZEOF(int32_t) * it->rule->column_count);
}

/* Set iterator data from table */
static
void set_iter_table(
    ecs_iter_t *iter,
    ecs_table_t *table,
    int32_t cur,
    int32_t offset)
{
    ecs_rule_iter_t *it = &iter->iter.rule;

    ecs_data_t *data = ecs_table_get_data(table);

    /* Table must have data, or otherwise it wouldn't yield */
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

    /* Tell the iterator how many entities there are */
    iter->count = ecs_table_data_count(data);
    ecs_assert(iter->count != 0, ECS_INTERNAL_ERROR, NULL);

    /* Set the entities array */
    ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);
    ecs_assert(entities != NULL, ECS_INTERNAL_ERROR, NULL);
    iter->entities = &entities[offset];

    /* Set table parameters */
    it->table.columns = rule_get_columns(it, cur);
    it->table.data = data;
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

/* Populate iterator with data before yielding to application */
static
void populate_iterator(
    const ecs_rule_t *rule,
    ecs_iter_t *iter,
    ecs_rule_iter_t *it,
    ecs_rule_op_t *op,
    int32_t op_index)
{
    int32_t r = op->r_in;

    iter->table = &it->table;

    /* If the input register for the yield does not point to a variable,
     * the rule doesn't contain a this (.) variable. In that case, the
     * iterator doesn't contain any data, and this function will simply
     * return true or false. An application will still be able to obtain
     * the variables that were resolved. */
    if (r == UINT8_MAX) {
        iter->count = 0;
    } else {
        ecs_rule_var_t *var = &rule->variables[r];
        ecs_rule_reg_t *regs = get_registers(it, op_index);
        ecs_rule_reg_t *reg = &regs[r];

        if (var->kind == EcsRuleVarKindTable) {
            ecs_table_t *table = table_reg_get(rule, regs, r);
            int32_t count = regs[r].is.table.count;
            int32_t offset = regs[r].is.table.offset;

            set_iter_table(iter, table, op_index, offset);

            if (count) {
                iter->offset = offset;
                iter->count = count;
            }
        } else {
            /* If a single entity is returned, simply return the
             * iterator with count 1 and a pointer to the entity id */
            ecs_assert(var->kind == EcsRuleVarKindEntity, 
                ECS_INTERNAL_ERROR, NULL);

            ecs_entity_t e = reg->is.entity;
            ecs_record_t *record = ecs_eis_get(rule->world, e);
            
            bool is_monitored;
            int32_t offset = iter->offset = ecs_record_to_row(
                record->row, &is_monitored);

            /* If an entity is not stored in a table, it could not have
             * been matched by anything */
            ecs_assert(record != NULL, ECS_INTERNAL_ERROR, NULL);
            set_iter_table(iter, record->table, op_index, offset);
            iter->count = 1;
        }
    }   
}

static
bool is_control_flow(
    ecs_rule_op_t *op)
{
    switch(op->kind) {
    case EcsRuleSetJmp:
    case EcsRuleJump:
        return true;
    default:
        return false;
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
    int32_t last_index = 0;

    do {
        /* Evaluate an operation. The result of an operation determines the
         * flow of the program. If an operation returns true, the program 
         * continues to the operation pointed to by 'on_pass'. If the operation
         * returns false, the program continues to the operation pointed to by
         * 'on_fail'.
         *
         * In most scenarios, on_pass points to the next operation, and on_fail
         * points to the previous operation.
         *
         * When an operation fails, the previous operation will be invoked with
         * redo=true. This will cause the operation to continue its search from
         * where it left off. When the operation succeeds, the next operation
         * will be invoked with redo=false. This causes the operation to start
         * from the beginning, which is necessary since it just received a new
         * input. */
        int32_t op_index = it->op;
        ecs_rule_op_t *op = &rule->operations[op_index];

        /* If this is not the first operation and is also not a control flow
         * operation, push a new frame on the stack for the next operation */
        if (!redo && op_index && !is_control_flow(op)) {
            push_registers(it, last_index, op_index);
            push_columns(it, last_index, op_index);
            it->op_ctx[op_index].last_op = last_index;
        }

        /* Dispatch the operation */
        bool result = eval_op(it, op, op_index, redo);
        it->op = result ? op->on_pass : op->on_fail;
        redo = !result;

        /* If the current operation is yield, return results */
        if (op->kind == EcsRuleYield) {
            populate_iterator(rule, iter, it, op, op_index);
            it->redo = true;
            return true;
        }

        /* If the current operation is a jump, goto stored label */
        if (op->kind == EcsRuleJump) {
            /* Label is stored in setjmp context */
            it->op = it->op_ctx[op->on_pass].is.setjmp.label;

        /* The SetJmp sets the jump label and represents the first time that a
         * a branch is evaluated, so always set redo to false */
        } else if (op->kind == EcsRuleSetJmp) {
            redo = false;

        /* Store the index of the last non-control flow operation */
        } else {
            last_index = op_index;
        }
    } while (it->op != -1);

    ecs_rule_iter_free(iter);

    return false;
}
