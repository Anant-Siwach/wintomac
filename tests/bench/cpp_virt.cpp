// C++ game-style code: an entity hierarchy updated through virtual calls, std::vector / std::sort,
// std::unordered_map with std::string keys, and std::function callbacks.
#include "bench.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Entity {
    float x = 0, y = 0, vx = 1, vy = 0.5f; int hp = 100; unsigned id;
    explicit Entity(unsigned i) : id(i) {}
    virtual ~Entity() {}
    virtual void update(float dt) { x += vx * dt; y += vy * dt; }
    virtual int score() const { return hp; }
};
struct Monster : Entity {
    int rage = 0;
    using Entity::Entity;
    void update(float dt) override { Entity::update(dt); if (++rage > 50) { rage = 0; vx = -vx; } }
    int score() const override { return hp * 2 + rage; }
};
struct Projectile : Entity {
    float life = 3.0f;
    using Entity::Entity;
    void update(float dt) override { x += vx * dt * 4; life -= dt; if (life < 0) { life = 3.0f; x = 0; } }
    int score() const override { return (int)life; }
};
struct Pickup : Entity {
    using Entity::Entity;
    void update(float dt) override { y += (id & 1 ? dt : -dt); }
    int score() const override { return 1; }
};

int main() {
    uint64_t s = 11;
    std::vector<std::unique_ptr<Entity>> world;
    for (unsigned i = 0; i < 20000; i++) {
        switch (bench_rng(&s) % 4) {
        case 0: world.emplace_back(new Entity(i)); break;
        case 1: world.emplace_back(new Monster(i)); break;
        case 2: world.emplace_back(new Projectile(i)); break;
        default: world.emplace_back(new Pickup(i)); break;
        }
    }
    double t0 = bench_now_ms();
    uint64_t h = 0;
    for (int frame = 0; frame < 300; frame++) {
        for (auto &e : world) e->update(0.016f);
        for (auto &e : world) h += (uint64_t)e->score();
    }
    bench_report("virtual_calls", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 10; r++) {
        std::vector<uint32_t> v(200000);
        for (auto &x : v) x = (uint32_t)bench_rng(&s);
        std::sort(v.begin(), v.end());
        std::vector<Entity *> ents;
        for (auto &e : world) ents.push_back(e.get());
        std::sort(ents.begin(), ents.end(), [](Entity *a, Entity *b) { return a->score() != b->score() ? a->score() < b->score() : a->id < b->id; });
        h += v[1000] + ents[100]->id;
    }
    bench_report("stl_sort", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    for (int r = 0; r < 4; r++) {
        std::unordered_map<std::string, int> m;
        char key[32];
        for (int i = 0; i < 50000; i++) { snprintf(key, sizeof key, "entity_%d_%d", i, r); m[key] = i; }
        for (int i = 0; i < 100000; i += 3) { snprintf(key, sizeof key, "entity_%d_%d", i, r); auto it = m.find(key); if (it != m.end()) h += (uint64_t)it->second; }
        h += m.size();
    }
    bench_report("stl_unordered_map", bench_now_ms() - t0, h);

    t0 = bench_now_ms(); h = 0;
    std::vector<std::function<int(int)>> cbs;
    for (int i = 0; i < 64; i++) cbs.push_back([i](int x) { return (x * (i + 1)) ^ i; });
    for (int r = 0; r < 100000; r++) for (auto &f : cbs) h += (uint64_t)f(r);
    bench_report("std_function", bench_now_ms() - t0, h);
    return 0;
}
