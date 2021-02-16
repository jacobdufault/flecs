#include "../private_api.h"
#include "errno.h"

#define ECS_PLECS_MAX_STACK (255)

#define TOK_BRACKET_OPEN '['
#define TOK_BRACKET_CLOSE ']'
#define TOK_CURLY_OPEN '{'
#define TOK_CURLY_CLOSE '}'
#define TOK_NEWLINE '\n'

typedef struct {
    ecs_entity_t subj;

    ecs_entity_t pred_obj_stack[ECS_PLECS_MAX_STACK];
    uint8_t sp;

    ecs_entity_t pred_out;
    ecs_entity_t subj_out;
    ecs_entity_t obj_out;

    bool args_only;
} plecs_context_t;

static
ecs_entity_t ensure_entity(
    ecs_world_t *world,
    const char *path)
{
    ecs_entity_t e = ecs_lookup_fullpath(world, path);
    if (!e) {
        e = ecs_new_from_path(world, 0, path);
        ecs_assert(e != 0, ECS_INTERNAL_ERROR, NULL);
    }

    return e;
}

static
int parse_line_action(
    ecs_world_t *world, const char *name, const char *expr, int64_t column,
    ecs_sig_from_kind_t from_kind, ecs_sig_oper_kind_t oper_kind,
    ecs_sig_inout_kind_t inout_kind, ecs_entity_t role, const char *entity_id,
    const char *source_id, const char *trait_id, const char *arg_name,
    int32_t argc, char **argv, void *data)
{
    (void)from_kind;
    (void)oper_kind;
    (void)inout_kind;
    (void)role;
    (void)source_id;
    (void)trait_id;
    (void)arg_name;

    plecs_context_t *ctx = data;
    ecs_entity_t pred = 0, subj = 0, obj = 0;

    ctx->pred_out = 0;
    ctx->subj_out = 0;
    ctx->obj_out = 0;
    ctx->args_only = false;

    /* If no entity id is provided, this is a (Pred, Obj) pair inside a type
     * expression. Get the entity id from the context */
    if (!entity_id || !entity_id[0]) {
        subj = ctx->subj;

        /* Must have at least one argument in this case */
        if (!argc) {
            ecs_parser_error(name, expr, column, 
                "missing arguments in type expression");
            return -1;
        }

        pred = ensure_entity(world, argv[0]);
        if (argc > 1) {
            obj = ensure_entity(world, argv[1]);
        }

        /* Signal that expression only contained arguments */
        ctx->args_only = true;
    } else {
        if (!entity_id) {
            ecs_parser_error(name, expr, column, 
                "expected identifier");
            return -1;
        }

        /* If expr contains no arguments, this is a subject/atom */
        if (!argc) {
            subj = ensure_entity(world, entity_id);

        /* If expr contains at least one argument, it has a predicate/subject */
        } else if (argc != 0) {
            pred = ensure_entity(world, entity_id);
            subj = ensure_entity(world, argv[0]);
        }

        /* If expression contains more than one argument, it has an object */
        if (argc > 1) {
            obj = ensure_entity(world, argv[1]);
        }        
    }

    /* If expression had a subject, add predicate/object */
    if (subj) {
        if (pred && !obj) {
            ecs_add_entity(world, subj, pred);
        } else if (pred && obj) {
            ecs_add_entity(world, subj, ecs_trait(obj, pred));
        }

        /* If the expression is inside a predicate-subject expression, add all
         * predicates on the stack */
        int i;
        for (i = 0; i < ctx->sp; i ++) {
            ecs_add_entity(world, subj, ctx->pred_obj_stack[i]);
        }
    }

    ctx->pred_out = pred;
    ctx->subj_out = subj;
    ctx->obj_out = obj;

    return 0;
}

/* Read plecs string line by line, invoke signature parser for terms */
int ecs_plecs_from_str(
    ecs_world_t *world,
    const char *name,
    const char *str) 
{
    const char *ptr;
    char ch, *lptr, line[512];
    bool obj_pred_list = false;
    bool expect_newline = false;

    plecs_context_t ctx = { 0 };

    for (lptr = line, ptr = str; (ch = *ptr); ptr ++) {   

        /* A newline should always follow a closing parenthesis */    
        if (expect_newline) {
            /* Can't use is_space, since that would skip newline */
            while (ch && (ch == ' ' || ch == '\t')) {
                ch = (++ ptr)[0];
            }

            if (ch && ch != '\n') {
                ecs_err("expected newline");
                goto error;
            }
        }

        /* A newline indicates that a new statement has been parsed ... */
        if (ch == '\n') {
            /* ... unless we are in a subject predicate list */
            if (!obj_pred_list) {
                lptr[0] = '\0';
                lptr = line;

                /* Use the regular signature parser to parse statements */
                if (ecs_parse_expr(world, name, line, parse_line_action, &ctx)){
                    goto error;
                }
            }

            expect_newline = false;

        /* If an opening bracket is found, this is a subject-predicate list */
        } else if (ch == TOK_BRACKET_OPEN) {
            *lptr = '\0';
            lptr = line;

            /* Parse expression before the [ */
            if (ecs_parse_expr(world, name, line, parse_line_action, &ctx)) {
                goto error;
            }

            if (ctx.args_only) {
                ecs_err("unexpected (pred, obj)");
                goto error;
            }

            if (obj_pred_list) {
                ecs_err("invalid nested object predicate list");
                goto error;
            }

            /* Set the subject to the parsed expression */
            ctx.subj = ctx.subj_out;

            /* Signal that we're in a subject-predicate list */
            obj_pred_list = true;

        /* If an opening curly brace is found, it's a predicate-subject list */
        } else if (ch == TOK_CURLY_OPEN) {
            *lptr = '\0';
            lptr = line;

            /* Parse expression before the {. Set the sp temporarily to 0 as
             * we don't want to add the entities from previous frames to the
             * entity that indicates the next frame. */
            int sp_temp = ctx.sp;
            ctx.sp = 0;
            if (ecs_parse_expr(world, name, line, parse_line_action, &ctx)) {
                goto error;
            }
            ctx.sp = sp_temp;

            ecs_entity_t pred = 0, obj = 0;
            if (!ctx.args_only) {
                if (ctx.pred_out || ctx.obj_out) {
                    ecs_err("unexpected predicate and/or object");
                    goto error;
                }
                pred = ctx.subj_out;
            } else {
                pred = ctx.pred_out;
                obj = ctx.obj_out;
            }

            ctx.sp ++;
            if (ctx.sp == ECS_PLECS_MAX_STACK) {
                ecs_err("predicate-object list is nested too deep");
                goto error;
            }

            /* Push predicate or pair to the stack so that it is added to every
             * subject inside the predicate-subject list */
            if (!obj) {
                ctx.pred_obj_stack[ctx.sp - 1] = pred;
            } else {
                ctx.pred_obj_stack[ctx.sp - 1] = ecs_trait(obj, pred);
            }

        /* If a closing bracket is found, close an subject-predicate list */
        } else if (ch == TOK_BRACKET_CLOSE) {
            if (!obj_pred_list) {
                ecs_err("invalid ']' without a '['");
                goto error;
            }

            lptr[0] = '\0';
            lptr = line;
            if (ecs_parse_expr(world, name, line, parse_line_action, &ctx)){
                goto error;
            }

            obj_pred_list = false;
        
        /* If a closing curly brace is found, close a predicate-subject list */
        } else if (ch == TOK_CURLY_CLOSE) {
            if (!ctx.sp) {
                ecs_err("invalid ']' without an '[");
                goto error;
            }

            ctx.sp --;

            expect_newline = true;

        /* Add character to line */            
        } else {
            *lptr = ch;
            lptr ++;
        }
    }

    return 0;
error:
    return -1;
}

int ecs_plecs_from_file(
    ecs_world_t *world,
    const char *filename) 
{
    FILE* file;
    char* content = NULL;
    int32_t bytes;
    size_t size;

    /* Open file for reading */
    file = fopen(filename, "r");
    if (!file) {
        ecs_err("%s (%s)", strerror(errno), filename);
        goto error;
    }

    /* Determine file size */
    fseek(file, 0 , SEEK_END);
    bytes = (int32_t)ftell(file);
    if (bytes == -1) {
        goto error;
    }
    rewind(file);

    /* Load contents in memory */
    content = ecs_os_malloc(bytes + 1);
    size = (size_t)bytes;
    if (!(size = fread(content, 1, size, file)) && bytes) {
        ecs_err("%s: read zero bytes instead of %d", filename, size);
        ecs_os_free(content);
        content = NULL;
        goto error;
    } else {
        content[size] = '\0';
    }

    fclose(file);

    int result = ecs_plecs_from_str(world, filename, content);
    ecs_os_free(content);
    return result;
error:
    ecs_os_free(content);
    return -1;
}
