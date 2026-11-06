// C++ test shaped like game code: entity hierarchy with virtuals, STL containers, a tiny sim loop.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <algorithm>

struct Vec2 { float x, y; Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; } Vec2 operator*(float s) const { return {x * s, y * s}; } };

struct Entity {
    std::string name; Vec2 pos, vel; int hp;
    Entity(std::string n, Vec2 p, Vec2 v, int h) : name(std::move(n)), pos(p), vel(v), hp(h) {}
    virtual ~Entity() {}
    virtual void update(float dt) { pos = pos + vel * dt; }
    virtual const char *kind() const { return "entity"; }
};
struct Player : Entity {
    int score = 0;
    using Entity::Entity;
    void update(float dt) override { Entity::update(dt); score += (int)(std::sqrt(vel.x * vel.x + vel.y * vel.y) * 10); }
    const char *kind() const override { return "player"; }
};
struct Enemy : Entity {
    float phase;
    Enemy(std::string n, Vec2 p, float ph) : Entity(std::move(n), p, {0, 0}, 30), phase(ph) {}
    void update(float dt) override { phase += dt; vel = {std::cos(phase) * 2.0f, std::sin(phase) * 2.0f}; Entity::update(dt); hp -= 1; }
    const char *kind() const override { return "enemy"; }
};

int main() {
    std::vector<std::unique_ptr<Entity>> world;
    world.push_back(std::make_unique<Player>("hero", Vec2{0, 0}, Vec2{1.5f, 0.5f}, 100));
    for (int i = 0; i < 5; i++) world.push_back(std::make_unique<Enemy>("goblin" + std::to_string(i), Vec2{(float)i * 3, 10}, i * 0.7f));
    std::map<std::string, int> kills;
    uint32_t rng = 12345;
    for (int frame = 0; frame < 120; frame++) {
        for (auto &e : world) e->update(1.0f / 60);
        rng = rng * 1664525u + 1013904223u;
        if ((rng >> 24) < 8) kills[world[1 + (rng >> 8) % 5]->name]++;
    }
    std::sort(world.begin(), world.end(), [](auto &a, auto &b) { return a->pos.x < b->pos.x; });
    for (auto &e : world) std::printf("%-8s %-7s pos=(%7.3f,%7.3f) hp=%d\n", e->kind(), e->name.c_str(), e->pos.x, e->pos.y, e->hp);
    for (auto &[k, v] : kills) std::printf("kills[%s]=%d\n", k.c_str(), v);
    auto *p = dynamic_cast<Player *>(world.back().get());
    std::printf("player score=%d\n", p ? p->score : -1);
    return 0;
}
