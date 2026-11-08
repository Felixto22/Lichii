# Plan: NVN directo en Lichii (el método de Hookshot)

Objetivo: que Ocarina of Time en Lichii no pase por la emulación de la GPU de la Switch 2 (Maxwell/Ampere,
GPUBack, cachés de buffers y texturas), sino que sus llamadas NVN vayan directo a Vulkan, como en Hookshot.

## Por qué
Perfil de Lichii (2026-11-07): los núcleos del juego esperan >50 % del tiempo; el cuello es GPUBack
(Maxwell3D::CallMethod 60 %, RasterizerVulkan::Draw 48 %). Hookshot no tiene ese hilo: graba las llamadas NVN y
las reproduce en Vulkan (hookshot_vk.dll).

## Piezas
1. **Renderer**: se reutiliza `hookshot_vk.dll` tal cual (API C `mkwvk_init(const MkwVkHost*)`, gpu/vk/vk_api.h).
   Lichii implementa `MkwVkHost` sobre su memoria (fastmem arena = base de guest).
2. **Puente guest→host**: SVC reservado (`svc #0x1000+i`). Cada proc NVN que pide el juego apunta a un stub
   `svc #N; ret` en una página de guest propia. `Kernel::Svc::Call` despacha N al host con los registros del hilo.
3. **NVN nativo**: se porta `host/nvn_native.c` (+ nvn_tex.c) con una capa de compatibilidad
   (Ctx/x[]/vreg, GP = arena + dirección, hos_log, hos_region_alloc, vi nativo).
4. **Vigilancia de escrituras**: el renderer necesita las páginas que el CPU escribió (Hookshot lo marca en cada
   store). En Dynarmic: un byte por página marcado en cada escritura (EmitMemoryWrite), y en Memory::Write del host.
5. **Enganche**: al cargar `sdk`, parchear `nvnBootstrapLoader` del juego para que devuelva los stubs.
6. **Presentación**: el renderer presenta en su propia ventana (v1); el presentador de Lichii queda inactivo.

## Hitos
- M1 renderer cargado con memoria de Lichii · M2 puente SVC · M3 nvn_native portado · M4 enganche + título
- M5 jugabilidad (esperas, ventana) · M6 FPS y comparación con Hookshot.
Interruptor: `EDEN_NX2_NVN_DIRECT=1` (apagado por defecto hasta M5).
