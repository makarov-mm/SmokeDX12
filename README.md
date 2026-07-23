# SmokeDX12

A real time 3D smoke simulation written in C++17 and Direct3D 12.

The whole simulation runs on the GPU as a chain of compute dispatches over a
volumetric grid, and the result is displayed with a single pass volumetric ray
marcher. No external dependencies, no third party libraries, no legacy DirectX
SDK. Just the Windows SDK that ships with Visual Studio.

![grid](https://img.shields.io/badge/grid-128%C2%B3-blue) ![api](https://img.shields.io/badge/api-Direct3D%2012-green) ![license](https://img.shields.io/badge/license-MIT-lightgrey)

## What it actually does

The solver integrates the incompressible Navier Stokes equations on a uniform
Eulerian grid using the classic operator splitting scheme introduced by Jos Stam
in *Stable Fluids*, extended with the buoyancy and vorticity confinement terms
described by Fedkiw, Stam and Jensen in *Visual Simulation of Smoke*.

Each simulation step is eight compute passes:

| Stage | Pass | What happens |
| --- | --- | --- |
| 1 | `CSInject` | Density, temperature and momentum are added inside the emitter sphere, modulated by value noise so the plume is not perfectly symmetric. |
| 2 | `CSForces` | Thermal buoyancy `b(T - T_ambient)` minus the weight of the smoke `g*rho`, followed by vorticity confinement that restores the small scale rotation numerical diffusion destroys. |
| 3 | `CSAdvectVelocity` | Semi Lagrangian self advection: trace each cell backwards along its own velocity and sample the field there with hardware trilinear filtering. |
| 4 | `CSDivergence` | Central differences give the divergence of the intermediate field. |
| 5 | `CSClearScalar` | Pressure is reset before the iterative solve. |
| 6 | `CSJacobi` x 40 | Jacobi iterations of the pressure Poisson equation `laplacian(p) = div(u)` with Neumann boundaries. |
| 7 | `CSProject` | Subtracting the pressure gradient makes the field divergence free, which is what enforces incompressibility. |
| 8 | `CSAdvectScalars` | Density and temperature are transported by the corrected velocity field, with exponential dissipation and cooling towards the ambient temperature. |

Rendering integrates the radiative transfer equation front to back along the
view ray. At every sample a secondary march towards the light accumulates
optical depth, which gives real self shadowing inside the plume. Emission is
driven by the temperature field through a blackbody style ramp, so hot gas near
the source glows and cools to grey smoke as it rises.

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
| `R` | Recompile all shaders |
| `Backspace` | Reset the simulation to an empty grid |
| `Q` / `E` | Decrease or increase vorticity confinement |
| `Z` / `X` | Decrease or increase the rendered density scale |
| `C` / `V` | Decrease or increase the emission scale |
| `Esc` | Quit |

## Project layout

```
SmokeDX12.sln
SmokeDX12.vcxproj
shaders/
  Fluid.hlsl            all eight compute stages of the solver
  VolumeRender.hlsl     fullscreen triangle and the volumetric ray marcher
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
  layout: 24 root constants at `b0`, an SRV table covering `t0..t3` and a UAV
  table covering `u0..u3`. Slots a given pass does not use are filled with tiny
  one by one by one placeholder textures that permanently live in the matching
  resource state, which removes an entire class of state and validation bugs.
* **Reads through SRVs, writes through UAVs.** Typed UAV loads are only
  guaranteed for the 32 bit formats, so the fields are stored as `R16_FLOAT` and
  `R16G16B16A16_FLOAT`, read as shader resources and written as unordered access
  views. Advection needs trilinear sampling anyway, so this costs nothing.
* **Ping pong everywhere.** Velocity, density, temperature and pressure each own
  two textures. The transition from unordered access to shader resource between
  passes doubles as the synchronisation barrier.

## Things worth trying next

The code is deliberately small enough to hack on in an afternoon.

* Replace the Jacobi solver with red black Gauss Seidel or a multigrid V cycle
  and compare the iteration counts needed for the same visual result.
* Swap semi Lagrangian advection for MacCormack or BFECC to cut the numerical
  diffusion that currently eats the fine detail.
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
