#include <api.h>

static
void test_1_comp(const char *expr) {
    ecs_world_t *world = ecs_init();

    ECS_COMPONENT(world, Position);
    ECS_COMPONENT(world, Velocity);

    ecs_rule_t *r = ecs_rule_new(world, expr);
    test_assert(r != NULL);

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
    test_assert(r != NULL);

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
"IsA(CelestialBody, Thing)\n"
"IsA(Planet, CelestialBody)\n"
"IsA(Moon, CelestialBody)\n"
"IsA(Person, Thing)\n"
"IsA(Machine, Thing)\n"
"IsA(SentientMachine, Machine)\n"
"IsA(Character, Person)\n"
"IsA(Human, Character)\n"
"IsA(Droid, Character)\n"
"IsA(Droid, SentientMachine)\n"
"IsA(Creature, Character)\n"
"IsA(Wookie, Creature)\n"
"IsA(Vehicle, Machine)\n"
"IsA(Transport, Vehicle)\n"
"IsA(Transport, Container)\n"
"IsA(SpaceShip, Transport)\n"
"IsA(Speeder, Transport)\n"
"IsA(CorellianLightFreighter, SpaceShip)\n"
"IsA(MilleniumFalcon, CorellianLightFreighter)\n"
"IsA(XWing, SpaceShip)\n"
"IsA(YWing, SpaceShip)\n"
"IsA(Cyborg, SentientMachine)\n"
"IsA(Cyborg, Human)\n"
"Sentient(Droid)\n"
"Sentient(Human)\n"
"Faction(XWing, Rebellion)\n"
"IsA(Rebellion, Faction)\n"
"IsA(FirstOrder, Faction)\n"
"AtWar(FirstOrder, Rebellion)\n"
"AtWar(Rebellion, FirstOrder)\n"
"Human(Luke)\n"
"Human(Leia)\n"
"Human(Rey)\n"
"Human(HanSolo)\n"
"Human(BenSolo)\n"
"Cyborg(Grievous)\n"
"Creature(Yoda)\n"
"Jedi(Yoda)\n"
"Jedi(Luke)\n"
"Jedi(Leia)\n"
"Jedi(Rey)\n"
"Sith(DarthVader)\n"
"Sith(Palpatine)\n"
"Droid(R2D2)\n"
"Droid(C3PO)\n"
"Droid(BB8)\n"
"Wookie(Chewbacca)\n"
"HomePlanet(Yoda, Dagobah)\n"
"HomePlanet(Luke, Tatooine)\n"
"HomePlanet(Rey, Tatooine)\n"
"HomePlanet(BB8, Tatooine)\n"
"HomePlanet(DarthVader, Mustafar)\n"
"Parent(Luke, DarthVader)\n"
"Parent(Leia, DarthVader)\n"
"Parent(BenSolo, HanSolo)\n"
"Parent(BenSolo, Leia)\n"
"Enemy(Luke, Palpatine)\n"
"Enemy(Luke, DarthVader)\n"
"Enemy(Yoda, Palpatine)\n"
"Enemy(Yoda, DarthVader)\n"
"Enemy(Rey, Palpatine)\n"
"Likes(Leia, HanSolo)\n"
"Likes(HanSolo, Leia)\n"
"Likes(Fin, Rey)\n"
"Likes(Rey, Ben)\n"
;

void Rules_1_fact_true() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Jedi(Yoda)");
    test_assert(r != NULL);

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
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_facts_true() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Jedi(Yoda), Sith(DarthVader)");
    test_assert(r != NULL);

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
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_facts_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Sith(Yoda), Jedi(DarthVader)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_1_fact_pair_true() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(Yoda, Dagobah)");
    test_assert(r != NULL);

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
    test_assert(r != NULL);
    
    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_fact_pairs_true() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(Yoda, Dagobah), HomePlanet(Luke, Tatooine)");
    test_assert(r != NULL);

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
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_2_fact_pairs_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(Yoda, Tatooine), HomePlanet(Luke, Mustafar)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_find_1_pair() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(., Tatooine)");
    test_assert(r != NULL);

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
    test_assert(r != NULL);

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

#define test_column_entity(it, column_id, str) {\
    ecs_entity_t _column_entity_e = ecs_column_entity(it, column_id);\
    test_assert(_column_entity_e != 0);\
    char _column_entity[512];\
    ecs_entity_str((it)->world, _column_entity_e, _column_entity, sizeof(_column_entity));\
    test_str(_column_entity, str);\
}

#define test_column_source(it, column_id, str) {\
    ecs_entity_t _column_source_e = ecs_column_source(it, column_id);\
    test_assert(_column_source_e != 0);\
    char _column_source[512];\
    ecs_entity_str((it)->world, _column_source_e, _column_source, sizeof(_column_source));\
    test_str(_column_source, str);\
}

#define test_var(it, var_id, str) {\
    ecs_entity_t _var_e = ecs_rule_variable(it, var_id);\
    test_assert(_var_e != 0);\
    char _var[512];\
    ecs_entity_str((it)->world, _var_e, _var, sizeof(_var));\
    test_str(_var, str);\
}

void Rules_find_w_pred_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X(.), Jedi(.)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "Name");
    test_var(&it, x_var, "Name");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "Human");  
    test_var(&it, x_var, "Human");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "Jedi");  
    test_var(&it, x_var, "Jedi");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "Name");
    test_var(&it, x_var, "Name");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "Creature");
    test_var(&it, x_var, "Creature");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "Jedi");
    test_var(&it, x_var, "Jedi");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "Name");
    test_var(&it, x_var, "Name");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "Human"); 
    test_var(&it, x_var, "Human");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "Jedi");
    test_var(&it, x_var, "Jedi");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "Name");
    test_var(&it, x_var, "Name");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "Human");  
    test_var(&it, x_var, "Human");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "Jedi");
    test_var(&it, x_var, "Jedi");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_find_w_pred_var_explicit_subject() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X(Luke)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 0);
    test_var(&it, x_var, "Name");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 0);
    test_var(&it, x_var, "Human");  

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 0);
    test_var(&it, x_var, "Jedi");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_find_1_pair_w_object_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(., X)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BB8"); 
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_var(&it, x_var, "Tatooine");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "DarthVader");
    test_column_entity(&it, 1, "(HomePlanet,Mustafar)");
    test_var(&it, x_var, "Mustafar");
    
    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_var(&it, x_var, "Tatooine");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda"); 
    test_column_entity(&it, 1, "(HomePlanet,Dagobah)");
    test_var(&it, x_var, "Dagobah");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_var(&it, x_var, "Tatooine");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_find_2_pairs_w_object_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "HomePlanet(., X), Enemy(., Y)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);
    int32_t y_var = ecs_rule_find_variable(r, "Y");
    test_assert(y_var != -1);  

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,DarthVader)");
    test_var(&it, x_var, "Tatooine");
    test_var(&it, y_var, "DarthVader");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");
    test_var(&it, x_var, "Tatooine");
    test_var(&it, y_var, "Palpatine");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "(HomePlanet,Dagobah)");
    test_column_entity(&it, 2, "(Enemy,DarthVader)");
    test_var(&it, x_var, "Dagobah");
    test_var(&it, y_var, "DarthVader");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "(HomePlanet,Dagobah)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");
    test_var(&it, x_var, "Dagobah");
    test_var(&it, y_var, "Palpatine");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");
    test_var(&it, x_var, "Tatooine");
    test_var(&it, y_var, "Palpatine");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_find_1_pair_w_pred_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X(., Tatooine)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BB8"); 
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_var(&it, x_var, "HomePlanet");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_var(&it, x_var, "HomePlanet");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_var(&it, x_var, "HomePlanet");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);    
}

void Rules_find_2_pairs_w_pred_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X(., Tatooine), Y(., Palpatine)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);
    int32_t y_var = ecs_rule_find_variable(r, "Y");
    test_assert(y_var != -1);    

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");
    test_var(&it, x_var, "HomePlanet");
    test_var(&it, y_var, "Enemy");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "(HomePlanet,Tatooine)");
    test_column_entity(&it, 2, "(Enemy,Palpatine)");
    test_var(&it, x_var, "HomePlanet");
    test_var(&it, y_var, "Enemy");    

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_find_cyclic_pairs() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Likes(., X), Likes(X, .)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "HanSolo");
    test_column_entity(&it, 1, "(Likes,Leia)");
    test_var(&it, x_var, "Leia");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "(Likes,HanSolo)");
    test_var(&it, x_var, "HanSolo");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_join_by_object() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Parent(., X), Parent(Y, X)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);
    int32_t y_var = ecs_rule_find_variable(r, "Y");
    test_assert(y_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BenSolo");
    test_column_entity(&it, 1, "(Parent,Leia)");
    test_column_entity(&it, 2, "(Parent,Leia)");
    test_var(&it, x_var, "Leia");
    test_var(&it, y_var, "BenSolo");  

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BenSolo");
    test_column_entity(&it, 1, "(Parent,HanSolo)");
    test_column_entity(&it, 2, "(Parent,HanSolo)");
    test_var(&it, x_var, "HanSolo");
    test_var(&it, y_var, "BenSolo");  

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(Parent,DarthVader)");
    test_column_entity(&it, 2, "(Parent,DarthVader)");
    test_var(&it, x_var, "DarthVader");
    test_var(&it, y_var, "Luke");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(Parent,DarthVader)");
    test_column_entity(&it, 2, "(Parent,DarthVader)");
    test_var(&it, x_var, "DarthVader");
    test_var(&it, y_var, "Leia");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "(Parent,DarthVader)");
    test_column_entity(&it, 2, "(Parent,DarthVader)");
    test_var(&it, x_var, "DarthVader");
    test_var(&it, y_var, "Luke");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "(Parent,DarthVader)");
    test_column_entity(&it, 2, "(Parent,DarthVader)");
    test_var(&it, x_var, "DarthVader");
    test_var(&it, y_var, "Leia");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_join_by_predicate() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X(., DarthVader), X(Y, DarthVader)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);
    int32_t y_var = ecs_rule_find_variable(r, "Y");
    test_assert(y_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(Parent,DarthVader)");
    test_column_entity(&it, 2, "(Parent,DarthVader)");
    test_var(&it, x_var, "Parent");
    test_var(&it, y_var, "Luke");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(Parent,DarthVader)");
    test_column_entity(&it, 2, "(Parent,DarthVader)");
    test_var(&it, x_var, "Parent");
    test_var(&it, y_var, "Leia");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(Enemy,DarthVader)");
    test_column_entity(&it, 2, "(Enemy,DarthVader)");
    test_var(&it, x_var, "Enemy");
    test_var(&it, y_var, "Luke");    

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "(Enemy,DarthVader)");
    test_column_entity(&it, 2, "(Enemy,DarthVader)");
    test_var(&it, x_var, "Enemy");
    test_var(&it, y_var, "Yoda");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "(Enemy,DarthVader)");
    test_column_entity(&it, 2, "(Enemy,DarthVader)");
    test_var(&it, x_var, "Enemy");
    test_var(&it, y_var, "Luke");   

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "(Enemy,DarthVader)");
    test_column_entity(&it, 2, "(Enemy,DarthVader)");
    test_var(&it, x_var, "Enemy");
    test_var(&it, y_var, "Yoda");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "(Parent,DarthVader)");
    test_column_entity(&it, 2, "(Parent,DarthVader)");
    test_var(&it, x_var, "Parent");
    test_var(&it, y_var, "Luke");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "(Parent,DarthVader)");
    test_column_entity(&it, 2, "(Parent,DarthVader)");
    test_var(&it, x_var, "Parent");
    test_var(&it, y_var, "Leia");

    test_assert(!ecs_rule_next(&it));
    
    ecs_fini(world);
}

void Rules_join_by_predicate_from_subject() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Transitive(.), .(X, Character)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "IsA");
    test_column_entity(&it, 1, "Transitive");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Creature");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "IsA");
    test_column_entity(&it, 1, "Transitive");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Droid");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "IsA");
    test_column_entity(&it, 1, "Transitive");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Human");    

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_find_transitive() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(., Character)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Character");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Creature");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Creature)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Wookie");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 1);    
    test_str(ecs_get_name(world, it.entities[0]), "Human");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Human)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Cyborg");

    ecs_fini(world);
}

void Rules_find_transitive_2_branches() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(., Thing)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Thing");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 3);
    test_str(ecs_get_name(world, it.entities[0]), "CelestialBody");
    test_str(ecs_get_name(world, it.entities[1]), "Person");
    test_str(ecs_get_name(world, it.entities[2]), "Machine");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,CelestialBody)");
    test_int(it.count, 2);
    test_str(ecs_get_name(world, it.entities[0]), "Planet");
    test_str(ecs_get_name(world, it.entities[1]), "Moon");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Person)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Character");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Creature");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Creature)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Wookie");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Human");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Human)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Cyborg");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Machine)");
    test_int(it.count, 2);
    test_str(ecs_get_name(world, it.entities[0]), "SentientMachine");
    test_str(ecs_get_name(world, it.entities[1]), "Vehicle");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SentientMachine)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Cyborg");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SentientMachine)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Vehicle)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Transport");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Transport)");
    test_int(it.count, 2);
    test_str(ecs_get_name(world, it.entities[0]), "SpaceShip");
    test_str(ecs_get_name(world, it.entities[1]), "Speeder");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 2);
    test_str(ecs_get_name(world, it.entities[0]), "CorellianLightFreighter");
    test_str(ecs_get_name(world, it.entities[1]), "YWing");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,CorellianLightFreighter)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "MilleniumFalcon");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "XWing");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_subsets() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(., SpaceShip)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "SpaceShip");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 2);
    test_str(ecs_get_name(world, it.entities[0]), "CorellianLightFreighter");
    test_str(ecs_get_name(world, it.entities[2]), "YWing");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,CorellianLightFreighter)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "MilleniumFalcon");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "XWing");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_subsets_2_terms() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(., Character), IsA(., Machine)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Character)");
    test_column_entity(&it, 2, "(IsA,SentientMachine)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Human)");
    test_column_entity(&it, 2, "(IsA,SentientMachine)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Cyborg");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);    
}

void Rules_transitive_w_table_object() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Sentient(X), IsA(Y, X)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    int32_t y_var = ecs_rule_find_variable(r, "Y");
    test_assert(y_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Sentient");
    test_column_entity(&it, 2, "(IsA,Droid)");
    test_var(&it, x_var, "Droid");
    test_var(&it, y_var, "Droid");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Sentient");
    test_column_entity(&it, 2, "(IsA,Human)");
    test_var(&it, x_var, "Human");
    test_var(&it, y_var, "Human");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Sentient");
    test_column_entity(&it, 2, "(IsA,Human)");
    test_var(&it, x_var, "Human");
    test_var(&it, y_var, "Cyborg");
    test_int(it.count, 0);

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);   
}

void Rules_transitive_supersets() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(SpaceShip, .)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "SpaceShip");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Transport)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Transport");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Vehicle)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Vehicle");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Machine)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Machine");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Thing");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Container)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Container");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_superset_w_subj_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, ".(R2D2), IsA(., X)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Name");
    test_column_entity(&it, 2, "(IsA,Name)");
    test_var(&it, x_var, "Name");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Name");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Droid)");
    test_var(&it, x_var, "Droid");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,SentientMachine)");
    test_var(&it, x_var, "SentientMachine");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Machine)");
    test_var(&it, x_var, "Machine");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Thing)");
    test_var(&it, x_var, "Thing");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Character");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Person)");
    test_var(&it, x_var, "Person");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");
    
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Thing)");
    test_var(&it, x_var, "Thing");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Droid");    

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_superset_w_subj_var_2_term() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(SentientMachine, .), IsA(., X)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SentientMachine)");
    test_column_entity(&it, 2, "(IsA,SentientMachine)");
    test_var(&it, x_var, "SentientMachine");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "SentientMachine");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SentientMachine)");
    test_column_entity(&it, 2, "(IsA,Machine)");
    test_var(&it, x_var, "Machine");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "SentientMachine");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SentientMachine)");
    test_column_entity(&it, 2, "(IsA,Thing)");
    test_var(&it, x_var, "Thing");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "SentientMachine");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Machine)");
    test_column_entity(&it, 2, "(IsA,Machine)");
    test_var(&it, x_var, "Machine");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Machine");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Machine)");
    test_column_entity(&it, 2, "(IsA,Thing)");
    test_var(&it, x_var, "Thing");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Machine");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_column_entity(&it, 2, "(IsA,Thing)");
    test_var(&it, x_var, "Thing");   
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Thing");    

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_supersets_2_terms() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(SpaceShip, .), IsA(Container, .)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Container)");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Container");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_constraint_on_superset_var() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X(C3PO), IsA(X, Y), Sentient(Y)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    int32_t y_var = ecs_rule_find_variable(r, "Y");
    test_assert(y_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Droid)");
    test_column_entity(&it, 3, "Sentient");
    test_var(&it, x_var, "Droid");
    test_var(&it, x_var, "Droid");
    test_int(it.count, 0);

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_instances() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X, IsA(X, Character)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Yoda");
    test_column_entity(&it, 1, "Creature");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Creature");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);    
    test_str(ecs_get_name(world, it.entities[0]), "Chewbacca");
    test_column_entity(&it, 1, "Wookie");
    test_column_entity(&it, 2, "(IsA,Creature)");
    test_var(&it, x_var, "Wookie");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 2);
    test_str(ecs_get_name(world, it.entities[0]), "R2D2");
    test_str(ecs_get_name(world, it.entities[1]), "C3PO");
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Droid");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);    
    test_str(ecs_get_name(world, it.entities[0]), "BB8");
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Droid");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BenSolo");
    test_column_entity(&it, 1, "Human");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Human");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Luke");
    test_column_entity(&it, 1, "Human");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Human");    

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Leia");
    test_column_entity(&it, 1, "Human");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Human");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "HanSolo");
    test_column_entity(&it, 1, "Human");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Human");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);    
    test_str(ecs_get_name(world, it.entities[0]), "Rey");
    test_column_entity(&it, 1, "Human");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_var(&it, x_var, "Human");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);    
    test_str(ecs_get_name(world, it.entities[0]), "Grievous");
    test_column_entity(&it, 1, "Cyborg");
    test_column_entity(&it, 2, "(IsA,Human)");
    test_var(&it, x_var, "Cyborg");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_instances_2_terms() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "X, IsA(X, Character), IsA(X, Machine)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 2);    
    test_str(ecs_get_name(world, it.entities[0]), "R2D2");
    test_str(ecs_get_name(world, it.entities[1]), "C3PO");
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_column_entity(&it, 3, "(IsA,SentientMachine)");
    test_var(&it, x_var, "Droid");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);    
    test_str(ecs_get_name(world, it.entities[0]), "BB8");
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_column_entity(&it, 3, "(IsA,SentientMachine)");
    test_var(&it, x_var, "Droid");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 2);    
    test_str(ecs_get_name(world, it.entities[0]), "R2D2");
    test_str(ecs_get_name(world, it.entities[1]), "C3PO");
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_column_entity(&it, 3, "(IsA,Droid)");
    test_var(&it, x_var, "Droid");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);    
    test_str(ecs_get_name(world, it.entities[0]), "BB8");
    test_column_entity(&it, 1, "Droid");
    test_column_entity(&it, 2, "(IsA,Character)");
    test_column_entity(&it, 3, "(IsA,Droid)");
    test_var(&it, x_var, "Droid");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Grievous");
    test_column_entity(&it, 1, "Cyborg");
    test_column_entity(&it, 2, "(IsA,Human)");
    test_column_entity(&it, 3, "(IsA,SentientMachine)");
    test_var(&it, x_var, "Cyborg");

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);    
    test_str(ecs_get_name(world, it.entities[0]), "Grievous");
    test_column_entity(&it, 1, "Cyborg");
    test_column_entity(&it, 2, "(IsA,Human)");
    test_column_entity(&it, 3, "(IsA,Cyborg)");
    test_var(&it, x_var, "Cyborg");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_implicit_isa_instances() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Machine");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Cyborg");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Grievous");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_int(it.count, 2);
    test_str(ecs_get_name(world, it.entities[0]), "R2D2");
    test_str(ecs_get_name(world, it.entities[1]), "C3PO");

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Droid");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "BB8");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_implicit_isa_instances_2_terms() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "Human, Machine");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "Cyborg");
    test_column_entity(&it, 2, "Cyborg");
    test_int(it.count, 1);
    test_str(ecs_get_name(world, it.entities[0]), "Grievous");

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_same_pred_obj() {
    ecs_world_t *world = ecs_init();

    ECS_TAG(world, Foo);
    ECS_TAG(world, Bar);

    ecs_entity_t e1 = ecs_new_id(world);
    ecs_entity_t e2 = ecs_new_id(world);

    ecs_entity_t t1 = ecs_trait(Bar, Foo);
    ecs_entity_t t2 = ecs_trait(Foo, Foo);

    ecs_add_entity(world, e1, t1);
    ecs_add_entity(world, e2, t2);

    ecs_rule_t *r = ecs_rule_new(world, "X(., X)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(ecs_rule_next(&it));
    test_int(it.count, 1);
    test_int(it.entities[0], e2);

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_same_pred_obj_explicit_subject() {
    ecs_world_t *world = ecs_init();

    ECS_TAG(world, Foo);
    ECS_TAG(world, Bar);

    ECS_ENTITY(world, Ent, 0);

    ecs_entity_t t1 = ecs_trait(Bar, Foo);

    ecs_add_entity(world, Ent, t1);

    ecs_rule_t *r = ecs_rule_new(world, "X(Ent, X)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_true_depth_1() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(XWing, SpaceShip)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,XWing)");
    test_int(it.count, 0);

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_false() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(XWing, Creature)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_true_depth_2() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(XWing, Transport)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,XWing)");
    test_int(it.count, 0);    

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_true_depth_3() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(XWing, Vehicle)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,XWing)");
    test_int(it.count, 0);        

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_true_depth_4() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(XWing, Machine)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,XWing)");
    test_int(it.count, 0);

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_true_depth_5() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(XWing, Thing)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,XWing)");
    test_int(it.count, 0);    

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_true_2_relationships() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(Cyborg, Thing)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Human)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Cyborg)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SentientMachine)");
    test_int(it.count, 0); 

    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,Cyborg)");
    test_int(it.count, 0);        

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_all() {
    ecs_world_t *world = ecs_init();

    /* Use smaller rule set so testcase doens't blow up */
    const char *small_ruleset = 
        "IsA(CelestialBody, Thing)\n"
        "IsA(Planet, CelestialBody)\n"
        "IsA(Moon, CelestialBody)\n"
        "IsA(Character, Thing)\n"
        "IsA(Machine, Thing)\n"
        "IsA(Human, Character)\n"
        "IsA(Droid, Character)\n"
        "IsA(Droid, Machine)\n"
        "IsA(Creature, Character)\n";

    test_assert(ecs_plecs_from_str(world, NULL, small_ruleset) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(X, Y)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    int32_t y_var = ecs_rule_find_variable(r, "Y");
    test_assert(y_var != -1);

    /* All permutations of all valid IsA relationships */

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "CelestialBody");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Character");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0);    

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Machine");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0);    

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Planet");
    test_var(&it, y_var, "CelestialBody");
    test_column_entity(&it, 1, "(IsA,CelestialBody)");
    test_int(it.count, 0); 

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Moon");
    test_var(&it, y_var, "CelestialBody");
    test_column_entity(&it, 1, "(IsA,CelestialBody)");
    test_int(it.count, 0); 

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Planet");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0); 

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Moon");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Human");
    test_var(&it, y_var, "Character");
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Creature");
    test_var(&it, y_var, "Character");
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Human");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Creature");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Droid");
    test_var(&it, y_var, "Character");
    test_column_entity(&it, 1, "(IsA,Character)");
    test_int(it.count, 0);    

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Droid");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Droid");
    test_var(&it, y_var, "Machine");
    test_column_entity(&it, 1, "(IsA,Machine)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Droid");
    test_var(&it, y_var, "Thing");
    test_column_entity(&it, 1, "(IsA,Thing)");
    test_int(it.count, 0);          

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_same_subj_obj() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(SpaceShip, SpaceShip)");
    test_assert(r != NULL);

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_int(it.count, 0); 

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}

void Rules_transitive_fact_subset_superset() {
    ecs_world_t *world = ecs_init();

    test_assert(ecs_plecs_from_str(world, NULL, rules) == 0);

    ecs_rule_t *r = ecs_rule_new(world, "IsA(SpaceShip, X), IsA(X, Machine)");
    test_assert(r != NULL);

    int32_t x_var = ecs_rule_find_variable(r, "X");
    test_assert(x_var != -1);

    /* Results appear twice because transitive terms are inclusive by default */

    ecs_iter_t it = ecs_rule_iter(r);
    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "SpaceShip");
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_column_entity(&it, 2, "(IsA,Transport)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "SpaceShip");
    test_column_entity(&it, 1, "(IsA,SpaceShip)");
    test_column_entity(&it, 2, "(IsA,SpaceShip)");
    test_int(it.count, 0);     

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Transport");
    test_column_entity(&it, 1, "(IsA,Transport)");
    test_column_entity(&it, 2, "(IsA,Vehicle)");
    test_int(it.count, 0);  

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Transport");
    test_column_entity(&it, 1, "(IsA,Transport)");
    test_column_entity(&it, 2, "(IsA,Transport)");
    test_int(it.count, 0);

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Vehicle");
    test_column_entity(&it, 1, "(IsA,Vehicle)");
    test_column_entity(&it, 2, "(IsA,Machine)");
    test_int(it.count, 0); 

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Vehicle");
    test_column_entity(&it, 1, "(IsA,Vehicle)");
    test_column_entity(&it, 2, "(IsA,Vehicle)");
    test_int(it.count, 0);   

    test_assert(ecs_rule_next(&it));
    test_var(&it, x_var, "Machine");
    test_column_entity(&it, 1, "(IsA,Machine)");
    test_column_entity(&it, 2, "(IsA,Machine)");
    test_int(it.count, 0);                  

    test_assert(!ecs_rule_next(&it));

    ecs_fini(world);
}
