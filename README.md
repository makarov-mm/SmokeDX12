# SmokeDX12

A real time 3D smoke simulation written in modern C++ (built with `/std:c++latest`)
and Direct3D 12.

The whole simulation runs on the GPU as a chain of compute dispatches over a
volumetric grid, and the result is displayed with a single pass volumetric ray
marcher. No external dependencies, no third party libraries, no legacy DirectX
SDK. Just the Windows SDK that ships with Visual Studio.

![grid](https://img.shields.io/badge/grid-192x192x96-blue) ![api](https://img.shields.io/badge/api-Direct3D%2012-green) ![license](https://img.shields.io/badge/license-MIT-lightgrey)

## What it actually does

The solver integrates the incompressible Navier Stokes equations on a uniform
Eulerian grid using the classic operator splitting scheme introduced by Jos Stam
in *Stable Fluids*, extended with the buoyancy and vorticity confinement terms
described by Fedkiw, Stam and Jensen in *Visual Simulation of Smoke*.

The domain is deliberately not a cube. Cells are cubic, so the ratio between
the grid axes is also the world aspect of the box: 192x192x96 gives a domain
that is twice as tall and twice as long along the wind as it is deep, which is
what lets the plume rise and bend downwind instead of flattening against a
wall. Change the three `GridResolution` constants in `Application.cpp` and
everything else (box intersection, floor position, emitter shape) follows
automatically.

Each simulation step is eleven compute passes:

| Stage | Pass | What happens |
| --- | --- | --- |
| 1 | `CSInject` | Density, temperature and momentum are added inside the emitter sphere, modulated by noise so the plume is not perfectly symmetric. The injected momentum is a rim swirl plus curl noise. |
| 2 | `CSForces` | Thermal buoyancy `b(T - T_ambient)` minus the weight of the smoke `g*rho`, wind, vorticity confinement that restores the small scale rotation numerical diffusion destroys, and a divergence free turbulence field. Everything except buoyancy and wind is weighted by the local density, so the empty part of the domain is left alone. |
| 3 | `CSAdvectVelocity` | Forward semi Lagrangian self advection: trace each cell backwards along its own velocity and sample the field there with hardware trilinear filtering. |
| 4 | `CSAdvectVelocityCorrect` | The MacCormack correction of that step, clamped to the extrema of the trilinear footprint it read from. |
| 5 | `CSDivergence` | Central differences give the divergence of the intermediate field. |
| 6 | `CSJacobi` x 40 | Jacobi iterations of the pressure Poisson equation `laplacian(p) = div(u)`, Neumann on the solid walls, Dirichlet `p = 0` on the open top. Started from the previous frame's solution rather than from zero. |
| 7 | `CSProject` | Subtracting the pressure gradient makes the field divergence free, which is what enforces incompressibility. |
| 8 | `CSAdvectScalars` | Forward semi Lagrangian transport of density and temperature by the corrected velocity field. |
| 9 | `CSAdvectDensityCorrect` | MacCormack correction of the density, plus dissipation and the boundary fade. |
| 10 | `CSAdvectTemperatureCorrect` | The same for temperature, plus cooling towards the ambient temperature. |

### Advection

Plain semi Lagrangian advection is first order, and its numerical diffusion is
the single largest reason a plume turns into a featureless grey blob a few
seconds after it starts. Every step the field is resampled with trilinear
filtering, and trilinear filtering is a low pass filter, so detail is destroyed
at a rate that has nothing to do with the physics.

The solver therefore runs MacCormack. The forward step is the usual backtrace.
A second step advects that result backwards, and the difference between where it
lands and where it started is an estimate of the error of the forward step. Half
of it is added back:

```
phi_hat = A(phi)
phi_bar = A_reverse(phi_hat)
phi_new = phi_hat + (phi - phi_bar) / 2
```

The correction is second order but not monotone, so on its own it overshoots and
rings around sharp gradients. The result is clamped to the minimum and maximum of
the eight cells the forward step interpolated between, which is the limiter from
Selle et al. It costs one extra dispatch and one extra texture per field. A
gaussian pulse advected two hundred times at CFL 0.5 keeps 83 percent of its peak
under this scheme against 48 percent under the first order one.

Because advection no longer eats the small scales, vorticity confinement no
longer has to be turned up to compensate for it, and the default came down from
24 to 15. Confinement pushed that hard on top of a sharp advection scheme makes
the plume boil rather than swirl.

### Boundaries

The floor and the four side walls are solid, the top is open. A sealed box has
nowhere for a continuously fed plume to go, so the column reaches the lid and
spreads sideways into a flat sheet, which is both wrong and immediately readable
as wrong. The open top is a Dirichlet `p = 0` condition in the pressure solve,
which lets mass leave the domain, and density is faded out in a thin shell along
the top and the side walls so nothing accumulates against a boundary.

Pinning the pressure at the outlet also fixes the constant that a pure Neumann
problem leaves undetermined, which is what makes it safe to warm start the Jacobi
solve from the previous frame. The same forty iterations then refine an already
good guess instead of walking out from zero every frame.


### Wind

Wind is not a constant force. A constant force keeps accelerating the gas
forever, which looks like the smoke being fired out of a cannon. Instead the
solver relaxes the velocity field towards a target wind field:

```
u += (u_wind - u) * saturate(drag * dt)
```

and only the horizontal velocity components take part, because dragging the
vertical component would fight buoyancy and silently cap the rise height (this
is exactly the bug an earlier version of this project had). The gas can never
exceed the wind speed itself. The target field is modulated
by gusts (low frequency noise over time and height), its direction slowly
meanders, and the whole thing fades in with altitude so the base of the plume
stays attached to the source while the column above it bends and tears
downwind. Strength and direction are adjustable live with the arrow keys.

### Rendering

The pixel shader resolves the background first, an infinite ground plane or the
sky, and then integrates the radiative transfer equation front to back through
the simulation box and composites the result over it.

* **Sub grid detail.** The solver runs on a finite grid and its finest features
  are a couple of cells across, which reads as blobby no matter how good the
  integration is. The renderer erodes the sampled field with three octaves of
  value noise, weighted so that only material that is already thin is carved
  away and the dense core is left intact. The noise drifts upwards with the
  plume rather than sitting still in world space, otherwise the smoke visibly
  slides through a fixed pattern. This is by far the cheapest way to buy detail:
  it costs nothing in simulation time and nothing in memory.
* **Self shadowing.** At every sample a secondary march towards the light
  accumulates optical depth through the volume. The step grows exponentially,
  so the samples land near the shading point where the shadow gradient actually
  is, instead of being spread uniformly across a box that is mostly smooth.
  Secondary rays read the raw field without the detail noise: almost every
  sample in the frame is a shadow sample, and the shadow term is an exponential
  of an integral that the multiple scattering octaves blur further, so the
  erosion is invisible in it.
* **Sky occlusion.** Ambient light comes from above, so it is attenuated by a
  short march straight up. A flat ambient term added at every sample is exactly
  what flattens a volume out.
* **Powder term.** Single scattering overestimates the brightness of thin gas
  facing the light. Darkening it back down is what makes the lit side of the
  plume read as volume rather than as fog.
* **Multiple scattering approximation.** That same optical depth is reused
  across three octaves with progressively weaker extinction, weaker
  contribution and a flatter phase function. It costs nothing extra in samples
  and is the difference between fluffy smoke and a black silhouette.
* **Dual lobe Henyey Greenstein phase function**, a strong forward lobe plus a
  weak backward one, so the plume brightens when the light is behind it without
  going flat from every other angle.
* **Cast shadows on the ground.** Ground pixels run the same optical depth march
  towards the light, so the plume actually darkens the floor underneath it.
* **Emission from temperature** through a blackbody style ramp normalised by a
  reference temperature, with a steep falloff so only the genuinely hot core
  glows and the rest cools into grey smoke.
* **Empty space striding.** Most of a ray through the box is empty. One cheap
  fetch of the raw field decides whether to take a full step or a long one.
* **Interleaved gradient noise** jitters the march start offset to trade banding
  for a small amount of noise, an ACES filmic tone map at the end, and a one bit
  dither on the way out because the sky gradient bands badly without it.
* An antialiased procedural grid on the floor, distance fog, a sun disc in the
  sky and a warm bounce pool of light under the source.

## Requirements

* Windows 10 version 1903 or newer, or Windows 11
* Visual Studio 2022 with the **Desktop development with C++** workload
  (that workload already contains the Windows 10/11 SDK, which is where
  `d3d12.h`, `dxgi1_6.h` and `d3dcompiler.h` live)
* A GPU that supports Direct3D feature level 11_0

There is nothing else to install. The standalone DirectX SDK has been part of
the Windows SDK since Windows 8, so the old June 2010 DirectX SDK is neither
needed nor wanted.

Optional but useful:

* **Graphics Tools**, an optional Windows feature
  (`Settings > System > Optional features > Add a feature > Graphics Tools`).
  Without it the Direct3D 12 debug layer is unavailable and the Debug build
  simply runs without validation.
* PIX for Windows or RenderDoc for frame captures.

## Building

```
git clone https://github.com/<your-account>/SmokeDX12.git
cd SmokeDX12
start SmokeDX12.sln
```

Select the `x64` platform, pick `Debug` or `Release`, and build. The post build
step copies the `shaders` folder next to the executable, and the debugger
working directory is set to the output folder.

HLSL is compiled at run time with `D3DCompileFromFile` against shader model 5.1,
so shaders are never part of the MSBuild graph. That is what makes hot reloading
possible: edit a `.hlsl` file, press `R` in the running application, and the
pipeline states are rebuilt in place. If a shader fails to compile the error text
goes to the debug output and the previous pipelines keep running.

## Controls

| Input | Action |
| --- | --- |
| Left mouse drag | Orbit the camera |
| Mouse wheel | Zoom |
| `Space` | Pause and resume the simulation |
| `T` | Toggle the slow turntable rotation |
| `R` | Recompile all shaders |
| `Backspace` | Reset the simulation to an empty grid |
| `Q` / `E` | Decrease or increase vorticity confinement |
| `,` / `.` | Decrease or increase the turbulence strength |
| `N` / `M` | Decrease or increase the sub grid detail strength |
| `Z` / `X` | Decrease or increase the rendered density scale |
| `C` / `V` | Decrease or increase the emission scale |
| `F` / `G` | Decrease or increase the light intensity |
| `1` / `2` | Decrease or increase the exposure |
| `Left` / `Right` | Decrease or increase the wind strength |
| `Up` / `Down` | Rotate the wind direction |
| `Esc` | Quit |

The current value of the main tunable parameters is shown in the window title, so
dialling in a look and then copying the numbers back into the defaults in
`FluidSolver.h` and `VolumeRenderer.h` is the intended workflow. The grid
resolution lives at the top of `Application.cpp`.

## Project layout

```
SmokeDX12.sln
SmokeDX12.vcxproj
shaders/
  Fluid.hlsl            all eight compute stages of the solver
  VolumeRender.hlsl     fullscreen triangle, environment and the ray marcher
src/
  Main.cpp              entry point
  Application.*         main loop, input handling, per frame orchestration
  Window.*              Win32 window and input state
  GraphicsDevice.*      device, swap chain, command list, frame synchronisation
  DescriptorAllocator.* persistent CPU heap plus a per frame shader visible ring
  GpuTexture3D.*        3D texture with SRV, UAV and tracked resource state
  ShaderLibrary.*       run time HLSL compilation
  FluidSolver.*         the simulation itself
  VolumeRenderer.*      the ray marching pass
  OrbitCamera.*         orbit camera math
```

### Notes on the Direct3D 12 design

* **Descriptor management.** Every view is created once in a non shader visible
  CPU heap. Right before a dispatch the four SRVs and four UAVs a pass needs are
  copied as a contiguous block into a per frame segment of the shader visible
  heap, and the two root descriptor tables point at those blocks. The segment is
  only reused once the fence proves the GPU has finished with that frame.
* **One root signature for the whole solver.** All compute passes share the same
  layout: 32 root constants at `b0`, an SRV table covering `t0..t4` and a UAV
  table covering `u0..u3`. `t4` is a second vector slot, which the MacCormack
  correction needs because it reads the source field and the forward advected
  field at the same time. Slots a given pass does not use are filled with tiny
  one by one by one placeholder textures that permanently live in the matching
  resource state, which removes an entire class of state and validation bugs.
* **Reads through SRVs, writes through UAVs.** Typed UAV loads are only
  guaranteed for the 32 bit formats, so the fields are stored as `R16_FLOAT` and
  `R16G16B16A16_FLOAT`, read as shader resources and written as unordered access
  views. Advection needs trilinear sampling anyway, so this costs nothing.
* **Ping pong everywhere.** Velocity, density, temperature and pressure each own
  two textures, and each of the three advected fields owns a third for the
  forward half of the MacCormack step, which cannot share the pair because the
  correction pass reads both halves at once. The transition from unordered
  access to shader resource between passes doubles as the synchronisation
  barrier.

## Things worth trying next

The code is deliberately small enough to hack on in an afternoon.

* Replace the Jacobi solver with red black Gauss Seidel or a multigrid V cycle
  and compare the iteration counts needed for the same visual result. It is by
  far the most expensive part of the step now.
* Bake the renderer's detail noise into a small tiling 3D texture generated once
  at startup. Hardware trilinear filtering would then make it nearly free and
  the detail could also be afforded on the shadow rays.
* Add a static obstacle field and enforce no slip boundaries in the projection.
* Introduce a temperature dependent expansion term so the divergence constraint
  becomes `div(u) = alpha * dT/dt` instead of zero.
* Move rendering to a half resolution buffer with a temporal reprojection pass
  and spend the saved time on more ray marching steps.
* Store the fields in a sparse structure so the empty part of the domain costs
  nothing.

## References

* Jos Stam, *Stable Fluids*, SIGGRAPH 1999
* Ronald Fedkiw, Jos Stam, Henrik Wann Jensen, *Visual Simulation of Smoke*, SIGGRAPH 2001
* Mark Harris, *Fast Fluid Dynamics Simulation on the GPU*, GPU Gems

## License

MIT, see `LICENSE`.
