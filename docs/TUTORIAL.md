# Ron Tutorial

From "hello" to a small 3D game in seven steps. Every program below is complete: save it to a
file and run it with `ronc run file.ron`. The full language is described in
[LANGUAGE.md](LANGUAGE.md).

## 1. Hello

```ron
import "std/io.ron";

fn main() -> int {
    println("Hello, Ron!");
    return 0;
}
```

`ronc run hello.ron` compiles the file to a native `hello.exe` and runs it. `ronc new mygame`
creates a project folder with a `main.ron` and a test.

## 2. Values and functions

```ron
import "std/io.ron";

fn area(float width, float height) -> float {
    return width * height;
}

fn main() -> int {
    var lives = 3;                // type inferred: int
    float speed = 2.5;
    const int MAX_LIVES = 5;
    lives += 1;
    println("lives {} / {}, speed {:.1}", lives, MAX_LIVES, speed);
    println("area: {}", area(3.0, 4.0));
    for (int i in 0..3) print("{} ", i);
    println();
    return 0;
}
```

Number literals adapt to their context (`float speed = 2;` works). Conversions that may lose
information need `as`: `int whole = speed as int;`.

## 3. Structs, methods and operators

```ron
import "std/io.ron";

struct Vec2 { float x, y; }

fn +(Vec2 a, Vec2 b) -> Vec2 { return Vec2{a.x + b.x, a.y + b.y}; }
fn *(Vec2 v, float s) -> Vec2 { return Vec2{v.x * s, v.y * s}; }

struct Player {
    u8* name;
    Vec2 position;
    Vec2 velocity;
    int health;
}

fn update(Player* p, float dt) {
    p.position = p.position + p.velocity * dt;
}

fn is_alive(Player p) -> bool { return p.health > 0; }

fn main() -> int {
    Player hero = Player{name: "Ada", velocity: Vec2{1.0, 0.5}, health: 100};
    for (int _frame in 0..10) hero.update(0.1);   // method syntax: update(&hero, 0.1)
    println("{} is at {} (alive: {})", hero.name, hero.position, hero.is_alive());
    return 0;
}
```

`hero.update(0.1)` calls `update(&hero, 0.1)`; `println` prints whole structs.

## 4. Collections

```ron
import "std/io.ron";
import "std/list.ron";
import "std/map.ron";

struct Enemy { int hp; float x; }

fn total_hp(Enemy[] enemies) -> int {
    int sum = 0;
    for (var e in enemies) sum += e.hp;
    return sum;
}

fn main() -> int {
    List<Enemy> enemies;
    for (int i in 0..5) enemies.push(Enemy{hp: 10 * (i + 1), x: i as float});

    for (var e in &enemies) e.hp -= 5;            // pointers: modify in place
    println("total hp: {}", total_hp(enemies.slice()));

    Map<int, String> names;
    names.put(1, string("goblin"));
    names.put(2, string("troll"));
    String* found = names.get(2);
    if (found != null) println("enemy 2 is a {}", *found);

    enemies.free_list();
    return 0;
}
```

`List<T>` grows as needed, `Map<K, V>` is a hash map, and `T[]` slices view any array or list.

## 5. Errors and sum types

```ron
import "std/io.ron";
import "std/option.ron";

enum Item { Coin(int), Potion(float), Key }

fn describe(Item item) {
    match (item) {
        Coin(value) => println("a coin worth {}", value);
        Potion(heal) => println("a potion healing {:.0}%", heal * 100.0);
        Key => println("a key");
    }
}

fn parse_level(u8* text) -> Result<int, u8*> {
    int level = 0;
    for (u64 i = 0; text[i] != 0; i++) {
        if (text[i] < '0' || text[i] > '9') return Result::Err("not a number");
        level = level * 10 + (text[i] - '0') as int;
    }
    return Result::Ok(level);
}

fn next_level(u8* text) -> Result<int, u8*> {
    var level = parse_level(text)?;     // returns the error to our caller
    return Result::Ok(level + 1);
}

fn main() -> int {
    describe(Item::Coin(5));
    describe(Item::Potion(0.25));
    describe(Item::Key);
    match (next_level("12")) {
        Ok(n) => println("next level: {}", n);
        Err(e) => println("error: {}", e);
    }
    println("{}", next_level("x2").is_err());
    return 0;
}
```

## 6. Tests

```ron
fn clamp_health(int hp) -> int {
    if (hp < 0) return 0;
    if (hp > 100) return 100;
    return hp;
}

test "health stays in range" {
    assert(clamp_health(-5) == 0);
    assert(clamp_health(150) == 100);
    assert(clamp_health(42) == 42);
}

fn main() -> int { return clamp_health(80); }
```

`ronc test file.ron` runs every `test` block (each on its own thread) and prints a report.
In default builds, out-of-bounds indexes, `panic`, failed `assert` and crashes print the source
location and the call stack.

## 7. A tiny 3D game

```ron
import "std/engine.ron";

fn main() -> int {
    if (!engine_init(1280, 720, "Dodge")) return 1;
    int cube = engine_mesh_cube();
    int ground = engine_mesh_plane(40.0);
    OrbitCamera cam = orbit_camera(vec3(0.0, 0.0, 0.0), 12.0);
    Vec3 player = vec3(0.0, 0.5, 0.0);

    while (engine_running()) {
        float dt = engine_delta();
        if (engine_key_down(KEY_LEFT)) player.x -= 6.0 * dt;
        if (engine_key_down(KEY_RIGHT)) player.x += 6.0 * dt;
        if (engine_key_down(KEY_UP)) player.z -= 6.0 * dt;
        if (engine_key_down(KEY_DOWN)) player.z += 6.0 * dt;
        cam.target = player;
        cam.update();
        cam.apply();

        if (engine_begin_frame(0.45, 0.55, 0.7)) {
            Mat4 floor_model = mat4_identity();
            engine_draw_checker(ground, &floor_model, 0.6, 0.6, 0.6);
            draw(cube, mat4_translate(player), material(rgb(0.9, 0.3, 0.2), 0.4, 0.0));
            engine_end_frame();
        }
    }
    engine_shutdown();
    return 0;
}
```

`ronc` copies `ron_engine.dll` next to your game. From here, look at `examples/arena.ron`
(physics, ECS, sound, an animated glTF character) and the engine section of the
[reference](LANGUAGE.md#standard-library).
