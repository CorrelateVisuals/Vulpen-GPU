# arcball

A camera you turn and pan with the pointer, for any view that looks through a camera: one C++ node, `Arcball.cpp`, writing one `Camera` a frame at its port `camera`, the struct ray-cubes' `rays` reads (`contracts/Camera.h`, which a drop brings along).

| Pointer | Does |
| --- | --- |
| left drag | turns the scene about the target, as if rolling a ball under the pointer |
| right or middle drag | moves the target across the picture; the scene follows the pointer |
| wheel | brings the eye nearer or farther |
| left double click | home again, where the params put it |

Its params say where home is: `eye`, looking at `target`, `up` pointing up the picture, and `lens`, how far the picture is from the eye for a half height of 1 (the longer, the narrower). As dropped, home shows a triangle in clip space as it shows without a camera: `target=0,0,0`, `eye=0,0,-2`, `up=0,-1,0` (clip space's y points down), `lens=2`.

The camera takes the pointer only where the ide hands it its view's: in perform mode (`mode perform`, or the menubar's `run`), or in the Perform panel while that panel's `interact` button is lit; there the first press only gives the panel the focus. With `interact` off, a drag and the wheel in the Perform panel inspect the picture, as before.

## Into ray-cubes

ray-cubes looks through `orbit`'s camera; the arcball takes its place. With ray-cubes loaded (`view load ray-cubes`), type in the terminal:

```text
recipe drop custom/arcball arcball
node set arcball param=target=0,0.6,0 param=eye=14,6.6,0 param=up=0,1,0 param=lens=1.8
remove force orbit/
connect camera arcball.camera rays.camera
```

The second line starts it where `orbit` starts (`node set` gives the node just these params); the third takes `orbit` away with its connection to `rays`, which the fourth makes from the arcball. By the menus instead: a right press on the ground, `recipe` › `drop` › `custom/`, then type `arcball arcball`; on `orbit`, `remove` › `force`; on `arcball`, `connect` › `camera` › `rays.camera`. `undo` takes each line back, `orbit` too.

## Into a triangle project

A triangle draws in clip space with no camera, so its vertex shader learns to look through one: four edits, then a connection. In the node whose `.vert` sets `gl_Position` (`triangle/Triangle.vert` in `triangle`, `face/Face.vert` in `triangle-files`, `screen/Screen.vert` where a compute shader rasterizes the triangle into a picture, which the camera then turns as a rectangle):

1. Below the other structs, the camera as the arcball writes it:

   ```glsl
   struct Camera {
     vec4 eye;
     vec4 ahead; // w: the lens
     vec4 right;
     vec4 up;
   };

   VP_READS(Cameras, Camera);
   ```

2. In the `Pass` block, a field for it: `Cameras camera; // a connection: the arcball's`.

3. Above `main`, a point as the camera sees it, square in the middle of any window as `square()` keeps it:

   ```glsl
   vec4 seen(vec3 at) {
     const Camera camera = pass.camera.at[0];
     const vec3 from = at - camera.eye.xyz;
     const vec3 view = vec3(dot(from, camera.right.xyz), dot(from, camera.up.xyz),
                            dot(from, camera.ahead.xyz));
     const vec2 size = vec2(frame.resolution);
     const vec2 lens = camera.ahead.w * min(size.yx / size, vec2(1.0));
     return vec4(view.x * lens.x, -view.y * lens.y, 0.5 * view.z, view.z);
   }
   ```

4. The position through it: `gl_Position = vec4(square(X), ...);` becomes `gl_Position = seen(vec3(X, 0.0));`, whatever `X` is there (`corner.position.xy`, `at`).

Then, with the project loaded:

```text
recipe drop custom/arcball arcball
connect camera arcball.camera triangle.camera
```

naming the node you edited in place of `triangle` (`face`, `screen`). Until the line connects them, both nodes say what they wait for, naming that very line. By the menus: `connect` › `camera` › `triangle.camera` on `arcball`'s box. A triangle that turns by itself (`frames_per_turn`) keeps turning under the camera; `param set triangle frames_per_turn 0` holds it.
