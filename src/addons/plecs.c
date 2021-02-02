#include "../private_api.h"
#include "errno.h"

static
ecs_entity_t ensure_entity(
    ecs_world_t *world,
    const char *path)
{
    ecs_entity_t e = ecs_lookup_fullpath(world, path);
    if (!e) {
        e = ecs_new_from_path(world, 0, path);
    }

    ecs_assert(e != 0, ECS_INTERNAL_ERROR, NULL);

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
    if (!entity_id) {
        ecs_os_err("expected identifier");
        return -1;
    }

    ecs_entity_t pred = ensure_entity(world, entity_id);
    ecs_entity_t subj = 0, obj = 0;

    if (argc != 0) {
        subj = ensure_entity(world, argv[0]);
    }
    if (argc > 1) {
        obj = ensure_entity(world, argv[1]);
    }

    if (argc == 1) {
        ecs_add_entity(world, subj, pred);
    } else if (argc == 2) {
        ecs_add_entity(world, subj, ecs_trait(obj, pred));
    }

    return 0;
}

int ecs_plecs_from_str(
    ecs_world_t *world,
    const char *name,
    const char *str) 
{
    const char *ptr;
    char ch, *lptr, line[512];

    for (lptr = line, ptr = str; (ch = *ptr); ptr ++) {
        if (ch == '\n') {
            lptr[0] = '\0';
            if (strlen(line)) {
                if (ecs_parse_expr(world, name, line, parse_line_action, NULL)) {
                    goto error;
                }
            }
            lptr = line;
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
    int size;

    /* Open file for reading */
    file = fopen(filename, "r");
    if (!file) {
        ecs_err("%s (%s)", strerror(errno), filename);
        goto error;
    }

    /* Determine file size */
    fseek (file, 0 , SEEK_END);
    size = ftell (file);
    if (size == -1) {
        goto error;
    }
    rewind(file);

    /* Load contents in memory */
    content = ecs_os_malloc(size + 1);
    if (!(size = fread(content, 1, size, file))) {
        ecs_err("%s: read zero bytes instead of %d", filename, size);
        ecs_os_free(content);
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
