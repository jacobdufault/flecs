#include <api.h>

static
void test_1_comp(const char *expr) {
    ecs_world_t *world = ecs_init();

    ECS_COMPONENT(world, Position);
    ECS_COMPONENT(world, Velocity);

    ecs_rule_t *r = ecs_rule_new(world, expr);

    ecs_entity_t e1 = ecs_set(world, 0, Position, {10, 20});
    ecs_entity_t e2 = ecs_set(world, 0, Position, {30, 40});
    ecs_entity_t e3 = ecs_set(world, 0, Position, {50, 60});
    ecs_entity_t e4 = ecs_set(world, 0, Position, {70, 80});
    ecs_set(world, e4, Velocity, {1, 2});

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 3);
    test_int(it.entities[0], e1);
    test_int(it.entities[1], e2);
    test_int(it.entities[2], e3);

    Position *p = ecs_column(&it, Position, 1);
    test_assert(p != NULL);

    test_int(p[0].x, 10);
    test_int(p[0].y, 20);
    test_int(p[1].x, 30);
    test_int(p[1].y, 40);
    test_int(p[2].x, 50);
    test_int(p[2].y, 60);        

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_int(it.entities[0], e4);

    p = ecs_column(&it, Position, 1);
    test_assert(p != NULL);
    test_int(p[0].x, 70);
    test_int(p[0].y, 80);

    test_assert(!ecs_rule_next(&it));

    ecs_rule_free(r);

    ecs_fini(world);
}

static
void test_2_comp(const char *expr) {
    ecs_world_t *world = ecs_init();

    ECS_COMPONENT(world, Position);
    ECS_COMPONENT(world, Velocity);
    ECS_COMPONENT(world, Mass);

    ecs_rule_t *r = ecs_rule_new(world, "Position, Velocity");

    ecs_entity_t e1 = ecs_set(world, 0, Position, {10, 20});
    ecs_set(world, e1, Velocity, {1, 2});
    ecs_entity_t e2 = ecs_set(world, 0, Position, {30, 40});
    ecs_set(world, e2, Velocity, {3, 4});
    ecs_entity_t e3 = ecs_set(world, 0, Position, {50, 60});
    ecs_set(world, e3, Velocity, {5, 6});
    ecs_entity_t e4 = ecs_set(world, 0, Position, {70, 80});
    ecs_set(world, e4, Velocity, {7, 8});
    ecs_set(world, e4, Mass, {5});

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 3);
    test_int(it.entities[0], e1);
    test_int(it.entities[1], e2);
    test_int(it.entities[2], e3);

    Position *p = ecs_column(&it, Position, 1);
    test_assert(p != NULL);
    Velocity *v = ecs_column(&it, Velocity, 2);
    test_assert(v != NULL);

    test_int(p[0].x, 10); test_int(p[0].y, 20);
    test_int(p[1].x, 30); test_int(p[1].y, 40);
    test_int(p[2].x, 50); test_int(p[2].y, 60);        

    test_int(v[0].x, 1); test_int(v[0].y, 2);
    test_int(v[1].x, 3); test_int(v[1].y, 4);
    test_int(v[2].x, 5); test_int(v[2].y, 6);    

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_int(it.entities[0], e4);

    p = ecs_column(&it, Position, 1);
    test_assert(p != NULL);
    v = ecs_column(&it, Velocity, 2);
    test_assert(v != NULL);

    test_int(p[0].x, 70); test_int(p[0].y, 80);
    test_int(v[0].x, 7); test_int(v[0].y, 8);

    test_assert(!ecs_rule_next(&it));

    ecs_rule_free(r);

    ecs_fini(world);    
}

void Rules_1_comp() {
    test_1_comp("Position");
}

void Rules_2_comp() {
    test_2_comp("Position, Velocity");
}

void Rules_1_comp_explicit_subject() {
    test_1_comp("Position(.)");
}

void Rules_2_comp_explicit_subject() {
    test_2_comp("Position(.), Velocity(.)");
}

const char *rules =
"Jedi(Yoda)\n"
"Jedi(Luke)\n"
"Jedi(Leia)\n"
"Jedi(Rey)\n"
"Sith(DarthVader)\n"
"Sith(Palpatine)\n"
"Robot(R2D2)\n"
"Robot(C3PO)\n"
"Robot(BB8)\n"
"HomePlanet(Yoda, Dagobah)\n"
"HomePlanet(Luke, Tatooine)\n"
"HomePlanet(Rey, Tatooine)\n"
"HomePlanet(BB8, Tatooine)\n"
"HomePlanet(DarthVader, Mustafar)\n"
"Parent(Luke, DarthVader)\n"
"Parent(Leia, DarthVader)\n"
"Enemy(Luke, Palpatine)\n"
"Enemy(Luke, DarthVader)\n"
"Enemy(Yoda, Palpatine)\n"
"Enemy(Yoda, DarthVader)\n"
"Enemy(Rey, Palpatine)\n"
;

void Rules_1_fact_true() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Jedi(Yoda)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_int(it.count, 0);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_1_fact_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Sith(Yoda)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_facts_true() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Jedi(Yoda), Sith(DarthVader)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_int(it.count, 0);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_facts_1_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Sith(Yoda), Sith(DarthVader)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_facts_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Sith(Yoda), Jedi(DarthVader)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_1_fact_pair_true() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(Yoda, Dagobah)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_int(it.count, 0);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_1_fact_pair_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(Yoda, Tatooine)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_fact_pairs_true() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(Yoda, Dagobah), HomePlanet(Luke, Tatooine)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_int(it.count, 0);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_fact_pairs_1_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(Yoda, Dagobah), HomePlanet(Luke, Mustafar)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_fact_pairs_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(Yoda, Tatooine), HomePlanet(Luke, Mustafar)");
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_find_1_pair() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(., Tatooine)");
    ecs_iter_t it = ecs_rule_iter(r);
    
    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BB8");  

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");    
    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_find_2_pairs() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(., Tatooine), Enemy(., Palpatine)");
    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    
    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");    
    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

static
void test_column_entity(ecs_iter_t *it, int32_t column_id, const char *str) {
    ecs_entity_t e = ecs_column_entity(it, column_id);
    test_assert(e != 0);

    char buff[512];
    ecs_entity_str(it->world, e, buff, sizeof(buff));
    test_str(buff, str);
}

void Rules_find_1_pair_w_object_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(., X)");
    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BB8"); 
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "DarthVader");
    test_column_entity(&it, 1, "(HomePlanet,Mustafar)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    
    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda"); 
    test_column_entity(&it, 1, "(HomePlanet,Dagobah)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_find_2_pairs_w_object_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(., X), Enemy(., Y)");
    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,DarthVader)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "(HomePlanet,Dagobah)");
    test_column_entity(&it, 2, "(Enemy,DarthVader)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "(HomePlanet,Dagobah)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_find_1_pair_w_pred_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X(., Tatooine)");
    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BB8"); 
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);    
}

void Rules_find_2_pairs_w_pred_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X(., Tatooine), Y(., Palpatine)");
    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}
