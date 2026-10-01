# GPU interface

Everything in Fjell that uses the GPU goes through one set of types in
`src/ftk/gpu/`: a device that makes things, handles for what it made, frames, and
command lists to record into. The graphics API behind it is a *backend*. Today
that is Vulkan, in `src/ftk/gpu/vulkan/`; the interface is shaped so a Metal 4
backend can sit beside it without the rest of the engine noticing.

The rule that keeps it that way: nothing outside `src/ftk/gpu/vulkan/` names
Vulkan. No Vulkan or VMA header, no `Vk*` type, no `vk*` call, no `VK_*`
constant, and no reaching into the backend's own headers.
`cmake/GpuBackendCheck.cmake` fails the build on any of them, with the file
and line. The backend's own tests and benchmarks are the only exception, by
their names: `tests/test_vulkan_*.cpp` and `tests/bench_vulkan_*.cpp`. The
libraries back the check up: `ftk::gpu` links Vulkan and VMA privately, so
code outside the backend does not see VMA's headers at all, and on Windows
not Vulkan's either.

If the interface lacks something you need, add it to `src/ftk/gpu/` and implement
it in `src/ftk/gpu/vulkan/`. Name it by what it does for the caller, not by the
Vulkan call behind it, and ask how Metal would do the same thing.

## Libraries

These are ftk's, the toolkit Fjell is built on:

| Library | What it is |
|---|---|
| `ftk::gpu` | The interface, its Vulkan backend, and the SDL window (`ftk/gpu/window.hpp`) |
| `ftk::gpu-imgui` | `gpu::ImGuiRenderer`: Dear ImGui drawn through the interface |
| `ftk::framegraph` | The frame graph, which records passes into command lists and works out their barriers |

The engine, the hub and the standalone editor windows link these, and so can
any other program. `tests/library_link/gpu.cpp` links `ftk::gpu` alone and drives
every part of the interface without touching the backend. It needs a GPU, so
it is run by hand (`build/tests/ftk-link-gpu`).

## One backend, no virtual calls

A build has exactly one backend. The interface classes (`Device`, `Frame`,
`CommandList`, `Swapchain`, `Upload`, `Readback`) each hold a pointer to an
`Impl` that only the backend defines, and the backend's `.cpp` files define
their member functions. There are no virtual calls on hot paths, and a second
backend is a second set of those files, chosen at configure time.

## The device

```cpp
fjell::Window window{"Fjell", 1600, 900};
auto device = gpu::Device::create(window);          // Result<std::unique_ptr<Device>>
if (!device) throw std::runtime_error(device.error());
auto swapchain = gpu::Swapchain::create(**device, window);
```

`Device::create` picks a GPU that can show the window and brings up
everything it runs on. The window only helps choose the GPU: each swapchain
makes its own surface, so any number of windows can share one device (the
file browser and import dialog do). `device.caps()` says what differs between
GPUs: mesh shaders and their output limits, a compute queue beside the
graphics one, ray queries, frames in flight, the most MSAA samples.
`device.name()` is the GPU's name.

Two settings a program makes once: `set_shader_locator` (where compiled
shaders live, as a function from `"shaders/blur.comp"` to a path) and
`set_pipeline_cache_file` (where the driver's pipeline cache is kept between
runs).

## Things the device makes

Buffers, textures, samplers, pipelines, bind groups and acceleration
structures. Each `create(desc)` returns a `Result` holding an `Owned<T>`, a
move-only owner of a small handle. Handles are plain values: store them, pass
them, compare them. The device keeps the objects behind them.

When an `Owned` goes (it is reset, reassigned or destroyed), the object is
not destroyed there and then. The device holds on to it until the GPU has
finished every frame that might still use it, then destroys it. Nothing
waits for the GPU and nothing is retired by hand:

```cpp
gpu::Owned<gpu::Texture> map_;              // a pass's member
map_ = std::move(*device.create(desc));     // the old texture, if any, is released
```

Samplers are the exception: `device.sampler(desc)` hands back the same
sampler for the same description, and they last as long as the device.

A buffer made with `Memory::upload` or `Memory::readback` is mapped for its
whole life (`device.mapped(buffer)`). A buffer made with a `reserve` can
`device.grow` in place, keeping its handle. It grows through sparse memory
where the GPU has it, by copying otherwise, and `device.generation(buffer)`
moves whenever its address changes.

## Shaders, pipelines and binding

Shaders are GLSL (or FJSL, which compiles to GLSL) compiled to SPIR-V by the
build. A pipeline names its shaders, and its layout comes from reflecting
them:

```cpp
FJELL_TRY(pipeline_, device_->create(gpu::ComputePipelineDesc{
    .shader = "shaders/contact_shadow.comp",
    .shared = {resources.globals},
    .name = "contact_shadow",
}));
```

Resources are bound **by the names the shader gives them**. Set and binding
numbers appear only in GLSL. A draw or dispatch binds what it needs as it
records, and the command list places it:

```cpp
cmd.set_pipeline(pipeline_);
cmd.bind(ctx.globals);                                  // a group made earlier
cmd.bind({{"contact_out", gpu::storage(shadow_)}});     // by name, for this dispatch
cmd.push(ContactShadowPush{...});
cmd.dispatch(groups_x, groups_y, 1);
```

`gpu::sampled`, `texture`, `sampler`, `storage`, `uniform` and `acceleration`
say how each resource is bound. Sets the engine shares between many
pipelines (the globals, bindless textures, draw data and meshlets) are
*shared layouts*. A pipeline names the ones it uses in `.shared`, and they are
bound as groups made from them. Updating a shared group writes it in place
until a frame binds it. After that, each update writes a new version, so the
frame in flight keeps reading what it bound. `device.recreate(pipeline, desc)`
rebuilds a pipeline under the same handle.

## Frames and command lists

```cpp
gpu::Frame& frame = device.begin_frame();
auto image = swapchain->acquire(frame);
gpu::CommandList& cmd = frame.commands(gpu::Queue::graphics);
... record ...
frame.submit(cmd);
swapchain->present(frame, *image);                      // ends the frame too
```

`begin_frame` waits only while the GPU still has the frame that last used its
slot. A
frame can hold several lists on the graphics and compute queues, submitted in
order, with `.after` making one wait for a list on the other queue.
`frame.parallel_commands(queue)` gives each thread that asks a list of its
own, which a frame list plays with `execute`. The frame graph records its
parallel groups that way.

Memory that lives one frame comes from the frame: `cmd.transient(value)`
copies a value in and returns the range to bind, and
`frame.transient_slice(size)` gives CPU-writable bytes to fill first.

A command list records compute (`set_pipeline`, `bind`, `push`, `dispatch`),
copies, clears, fills, mip generation, acceleration structure builds and
barriers. `cmd.render(targets)` opens a render scope and returns a
`RenderEncoder` for draws (`draw`, `draw_indexed`, the indirect draws, and
mesh tasks). The scope ends when the encoder goes. `cmd.zone("name")` marks a
span for the GPU profiler. A list checks what it is asked to do. When a
mistake would make the GPU misbehave, such as a dispatch with a declared set
left unbound or a copy inside a render scope, it refuses that command and logs
the mistake once.

## Accesses instead of layouts

What a resource is used for is a `gpu::Access`: `sampled_fragment`,
`storage_write_compute`, `color_attachment`, `copy_dst`, `host_read` and so
on. Buffers have accesses of their own (`storage_buffer_write_compute`, not
the storage image one). A barrier says what a resource was used as and what
it will be used as, and the backend turns that into stages, access masks and,
on Vulkan, image layouts:

```cpp
cmd.barrier(texture, gpu::Access::copy_dst, gpu::Access::sampled_fragment);
```

Most barriers are not written by hand. Passes declare what they read and
write, and a resource imported into the graph rests in one access between
frames. The frame graph works out the transitions and records them with
`cmd.transition` (see [Framegraph](Framegraph.md)). `gpu::state_name(accesses,
depth)` is how the backend names a state in traces; on Vulkan that is the image
layout.

## Uploads and readbacks

`device.upload()` writes CPU data into buffers and textures (`to_buffer`,
`to_texture`, which can also generate the mips, and `clear`). Uploads run on
lanes of their own (buffer copies on a transfer queue where the GPU has one)
and land before the next frame's work, which waits for them.

`device.read_back(view, desc)` reads a texture back as 8-bit sRGB RGBA,
optionally a box of it, scaled on the way. It returns a `Readback` ticket to
`wait()` on or check with `ready()`. `cmd.read_back(view, desc)` reads what a
frame's list drew and is ready once that frame is done, which is how
thumbnails and captures read without stalling.

## Waiting

`device.wait_idle()` sends any uploads not yet sent and waits for the GPU to
finish everything. It is for teardown and for the few places that have yet to
rely on deferred release. New code should never need it: released objects
already wait for their frames.

## Tracing

- `FJELL_LOG_BARRIERS=<file>` writes every barrier the frame graph records,
  as the backend recorded it, for three frames starting
  `FJELL_LOG_BARRIERS_FRAME` frames in (300 by default). Diff two builds'
  traces to see what a change did to synchronisation.
- `FJELL_TRACE_LAYOUT=1` logs how the frame graph moves each texture it
  tracks from state to state.

## Where things are

| Path | What it holds |
|---|---|
| `src/ftk/gpu/device.hpp` | `Device`, `Caps`, the shader locator |
| `src/ftk/gpu/frame.hpp`, `command_list.hpp`, `render_encoder.hpp` | Frames and recording |
| `src/ftk/gpu/buffer.hpp`, `texture.hpp`, `sampler.hpp`, `acceleration.hpp` | Descriptions and handles |
| `src/ftk/gpu/pipeline.hpp`, `shader.hpp`, `binding.hpp` | Pipelines, reflection, binding by name |
| `src/ftk/gpu/access.hpp`, `transition.hpp` | Accesses and the frame graph's transitions |
| `src/ftk/gpu/upload.hpp`, `readback.hpp` | Moving data between CPU and GPU |
| `src/ftk/gpu/swapchain.hpp`, `window.hpp` | Showing frames in a window |
| `src/ftk/gpu/owned.hpp`, `release_queue.hpp` | Ownership and deferred release |
| `src/ftk/gpu/vulkan/` | The Vulkan backend: `Foundation` (instance, GPU, queues, allocator, upload lanes), the device's pools, translation tables, barriers |
