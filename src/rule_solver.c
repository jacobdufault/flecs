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
 * With(Type)
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
typedef enum ecs_register_kind_t {
    EcsRuleRegisterEntity,
    EcsRuleRegisterType,
    EcsRuleRegisterTable
} ecs_register_kind_t;

typedef struct ecs_register_t {
    ecs_register_kind_t kind;
    union {
        ecs_entity_t entity;
        ecs_vector_t *type;
        ecs_table_t *table;
    } is;
} ecs_register_t;

/* Operations describe how the rule should be evaluated */
typedef enum ecs_operation_kind_t {
    EcsRuleWith,
    EcsRuleFromEntity,
    EcsRuleFromType,
    EcsRuleFromTable,
    EcsRuleMatchEntity,
    EcsRuleMatchType,
    EcsRuleMatchSubject,
    EcsRuleMatchRole,
    EcsRuleMatchPair,
    EcsRuleYield
} ecs_operation_kind_t;

typedef struct ecs_operation_t {
    ecs_operation_kind_t kind;  /* What kind of operation is it */
    ecs_rule_param_t param;     /* Parameter that contains optional filter */

    uint8_t r_1;                /* Optional In/Out registers */
    uint8_t r_2;
    uint8_t r_3;
    
    uint8_t on_ok;              /* Jump location when match succeeds */
    uint8_t on_fail;            /* Jump location when match fails */
} ecs_operation_t;

/* Rule variables allow for the rule to be parameterized */
typedef struct ecs_variable_t {
    const char *name;
    int8_t reg;
} ecs_variable_t;

struct ecs_rule_t {
    ecs_operation_t *operations;
    ecs_variable_t *variables;

    int8_t operation_count;
    int8_t variable_count;
};


static
ecs_operation_t* create_operation(
    ecs_rule_t *rule)
{
    int8_t cur = rule->operation_count ++;
    rule->operations = ecs_os_realloc(rule->operations, (cur + 1) * ECS_SIZEOF(ecs_operation_t));
    return &rule->operations[cur];
}

static
const ecs_variable_t* create_variable(
    ecs_rule_t *rule,
    const char *name)
{
    int8_t cur = rule->variable_count ++;
    rule->variables = ecs_os_realloc(rule->variables, (cur + 1) * ECS_SIZEOF(ecs_variable_t));
    
    ecs_variable_t *var = &rule->variables[cur];
    var->name = ecs_os_strdup(name);
    var->reg = cur;
    return var;
}

static
const ecs_variable_t* find_variable(
    ecs_rule_t *rule,
    const char *name)
{
    ecs_assert(rule != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(name != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_variable_t *variables = rule->variables;
    int32_t i, count = rule->variable_count;
    
    for (i = 0; i < count; i ++) {
        ecs_variable_t *variable = &variables[i];
        if (!strcmp(name, variable->name)) {
            return variable;
        }
    }

    return NULL;
}

static
const ecs_variable_t* ensure_variable(
    ecs_rule_t *rule,
    const char *name)
{
    const ecs_variable_t *var = find_variable(rule, name);
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
        const ecs_variable_t *var = ensure_variable(rule, column->type.name);
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
        const ecs_variable_t *var = ensure_variable(rule, column->argv[1].name);
        result.subject = var->reg;
        result.reg_mask |= 2;
    } else {
        result.subject = subject_id;
    }

    return result;
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
    int32_t i, arg, count = ecs_vector_count(sig.columns);

    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];
        printf("%s", column->type.name);
        if (column->argc) {
            printf("(");
            for (arg = 0; arg < column->argc; arg ++) {
                if (arg) {
                    printf(",");
                }
                printf("%s", column->argv[arg].name);
            }
            printf(")");
        }
        printf("\n");
    }

    /* Step 1: find all expressions that match the type of This */
    ecs_vector_t *from_this = NULL;
    for (i = 0; i < count; i ++) {
        ecs_sig_column_t *column = &columns[i];

        /* Find columns where arg count is 0 or first argument is This  */
        if (!column->argc || column->argv[0].entity == EcsThis) {
            ecs_rule_pair_t *elem = ecs_vector_add(&from_this, ecs_rule_pair_t);
            *elem = column_to_pair(result, column);
        }
    }

    /* If expressions were found that apply to This, insert With instruction */
    if (from_this) {
        ecs_operation_t *op = create_operation(result);
        op->kind = EcsRuleWith;
        op->param.kind = EcsRuleParamType;
        op->param.is.type = from_this;
    }

    return result;
}
