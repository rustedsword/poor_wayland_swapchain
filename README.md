# poor_wayland_swapchain

libpoor_wayland_swapchain presents dmabufs on a
[poor_wayland](https://github.com/rustedsword/poor_wayland) surface. It follows
the dmabuf feedback of the surface, gets buffers in the formats and modifiers
that the compositor prefers from an allocator, GBM or Vulkan on the GPU that the
feedback names or one of the caller, and tells when to render the next frame. EGL and
Vulkan can't take poor_wayland's objects for their Wayland platforms, since Mesa
drives those with the system libwayland, so they render into the buffers of the
swapchain instead, with explicit synchronization through linux-drm-syncobj-v1 if
the compositor has it.

Vulkan apps should set `NODEVICE_SELECT=1`, or Mesa's device-select layer opens
its own blocking connection with the system libwayland.

It needs Linux 6.16 or newer, poor_wayland, wayland-protocols 1.49 or newer and
libgbm, and Vulkan 1.4 and
[vulkan_initializers](https://github.com/rustedsword/vulkan_initializers) for the
Vulkan images. Installed poor_wayland, wayland-protocols and vulkan_initializers
are used if there are, the submodules otherwise:
`--force-fallback-for=poor_wayland,wayland-protocols,vulkan_initializers` forces
them.

[examples/simple-shm.c](examples/simple-shm.c),
[examples/simple-egl.c](examples/simple-egl.c), which draws with OpenGL ES
through the swapchain, and [examples/simple-vulkan.c](examples/simple-vulkan.c),
which draws with Vulkan, are small clients, built with `-Dexamples=true`.

    $ meson setup build/ --prefix=PREFIX
    $ ninja -C build/ install
