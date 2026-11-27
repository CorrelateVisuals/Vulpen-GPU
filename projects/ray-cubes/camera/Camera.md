# camera

<!-- "But these are written, that ye might believe that Jesus is the Christ, the Son of
God; and that believing ye might have life through his name." John 20:31 -->

A camera you turn and pan with the pointer and glide with the keys, for any view that looks through a camera: one C++ node, `Camera.cpp`, writing one `Camera` a frame at its port `camera` (`contracts/Camera.h`, which a drop brings along). ray-cubes' `rays` cast from it, the mesh part projects through it.

| Pointer or key | Does |
| --- | --- |
| left drag | turns the scene about the target: with `turn=ball` as if rolling a ball under the pointer; with `turn=level` about `up` (a drag across) and over the target (a drag down), the horizon level and short of straight down |
| right or middle drag | moves the target across the picture; the scene follows the pointer |
| wheel | brings the eye nearer or farther |
| left double click | home again, where the params put it; with `double_click=none` nothing, for a view whose own double click acts on what it lands on |
| W, A, S, D or the arrows, held | glide the target, and the eye with it, over the plane square to `up`: ahead, back, left and right as the picture faces, laid flat, so a tilted camera neither climbs nor dives; two keys between their ways and no faster. `glide_speed` units a second, eased in and out, counted in the run's frame time, so a replay glides alike; 0, as dropped, leaves the keys to the view |

Its params say where home is: `eye`, looking at `target`, `up` pointing up the picture, and `lens`, how far the picture is from the eye for a half height of 1 (the longer, the narrower; 3.17 is a 35° view). As dropped, home shows a triangle in clip space as it shows without a camera: `target=0,0,0`, `eye=0,0,-2`, `up=0,-1,0` (clip space's y points down), `lens=2`. `frames_per_turn` turns it about `up` by itself, a whole turn every so many frames, 0 still. `near` and `far` are what a draw's depth counts from and to, which the camera hands on in `right.w` and `up.w`. Where it was turned, panned or glided stays through the rebuilds an edit of the view brings; it goes home only as `target`, `eye` or `up` change.

A ground keeps a glide over it: connect a node that writes a `Ground` (`contracts/Ground.h`: heights on a grid, row by row, from `origin` along `across` and `along`, which lie square to `up`) to the camera's `ground`. As it glides, the eye rides `ground_margin` over the ground where the ground rises to that under it, and comes back down to its own height where it falls away; past the ground's edge it reads the edge's height, so it passes over the edge with no drop. The target stops at the ground's edge and slides along it. Without the connection it glides at its height with no edge.

The camera takes the pointer and the keys only where the ide hands it its view's: in perform mode (`mode perform`, or the menubar's `run`), or in the Perform panel while that panel's `interact` button is lit; there the first press only gives the panel the focus. With `interact` off, a drag and the wheel in the Perform panel inspect the picture, as before.

## Into a triangle project

A triangle draws in clip space with no camera, so its vertex shader learns to look through one: four edits, then a connection. In the node whose `.vert` sets `gl_Position` (`triangle/Triangle.vert` in `triangle`, `face/Face.vert` in `triangle-files`, `screen/Screen.vert` where a compute shader rasterizes the triangle into a picture, which the camera then turns as a rectangle):

1. Below the other structs, the camera as it writes it:

   ```glsl
   struct Camera {
     vec4 eye;
     vec4 ahead; // w: the lens
     vec4 right; // w: near
     vec4 up;    // w: far
   };

   VP_READS(Cameras, Camera);
   ```

2. In the `Pass` block, a field for it: `Cameras camera; // a connection: the camera's`.

3. Above `main`, a point as the camera sees it, square in the middle of any window as `square()` keeps it, its depth 0 at near and 1 at far:

   ```glsl
   vec4 seen(vec3 at) {
     const Camera camera = pass.camera.at[0];
     const vec3 from = at - camera.eye.xyz;
     const vec3 view = vec3(dot(from, camera.right.xyz), dot(from, camera.up.xyz),
                            dot(from, camera.ahead.xyz));
     const vec2 size = vec2(frame.resolution);
     const vec2 lens = camera.ahead.w * min(size.yx / size, vec2(1.0));
     const float near = camera.right.w;
     const float far = camera.up.w;
     return vec4(view.x * lens.x, -view.y * lens.y,
                 far * (view.z - near) / (far - near), view.z);
   }
   ```

4. The position through it: `gl_Position = vec4(square(X), ...);` becomes `gl_Position = seen(vec3(X, 0.0));`, whatever `X` is there (`corner.position.xy`, `at`).

Then, with the project loaded:

```text
recipe drop camera
connect camera camera.camera triangle.camera
```

naming the node you edited in place of `triangle` (`face`, `screen`). Until the line connects them, both nodes say what they wait for, naming that very line. By the menus: `connect` › `camera` › `triangle.camera` on `camera`'s box. A triangle that turns by itself (`frames_per_turn`) keeps turning under the camera; `param set triangle frames_per_turn 0` holds it.
