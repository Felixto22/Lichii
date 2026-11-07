# Plan: cadena de montaje para el hilo de dibujos (GPUBack)

Escrito el 7 nov 2026 con las mediciones de la RX 9070 XT + Ryzen 7 7700X de Felix,
Ocarina of Time, pueblo Kokiri mirando al horizonte (la vista más exigente).

## Estado de partida (build-33, rama felix/nx2-interruptores)

- Cuadro: **~37 ms** (27 FPS) en esa vista; 28-40 FPS según la zona.
- Ajustes necesarios: `EDEN_NX2_SPLIT=2`, `EDEN_NX2_FENCE_BATCH=1`,
  `disable_buffer_reorder=true`, add-on Dynamic FPS desactivado, presentación asíncrona.

## Mediciones (no repetir; los archivos están en `%APPDATA%\eden\_backup_claude\perfiles`)

Línea de tiempo ligera (`EDEN_NX2_TIMELINE`, sin `EDEN_NX2_GPU_PROFILE`), 40 cuadros:

| Hilo | Por cuadro |
| --- | --- |
| GPUBack trabajando | 26,7 ms (72 %) |
| GPU (front) esperando al back | 19,1 ms (macro dma read 9,2; acquire 6,6; list writers 2,8) |
| Núcleos del juego bloqueados por la protección de escrituras | 12-17 ms cada uno |
| Esperas a la GPU real | ~1 ms |

Perfil de muestreo limpio (`/OPT:NOICF`, sin medidor por zonas), GPUBack, % del hilo:
Draw 23 %, métodos sin contar dibujos ~9 %, cómputo 5,4 %, presentación 5,2 %
(ya movida con presentación asíncrona), creación de imágenes 5,7 % (solo al moverse:
8-42 imágenes cada 600 cuadros en la escena quieta), ResolveMultiRange 3,4 %,
uniformes 3,3 %, lecturas de la CPU que vacían la GPU 2,4 %.

Conclusiones ya comprobadas:
- No es la GPU (~14 ms de trabajo por cuadro).
- No es la resolución (Max Resolution apagado empeoró).
- No es V-Sync, ni el giro de los núcleos del juego, ni la precisión de CPU.
- Las escrituras de registros repetidas ya se filtran en el front (FrontWrite).
- La herramienta por zonas (`EDEN_NX2_GPU_PROFILE`) infla el GPUBack ~6 ms por cuadro:
  no usarla para medir el reparto de ese hilo.

## El obstáculo principal

Casi cada dibujo trae escrituras en memoria del juego (bloques de constantes vía
`WriteBlockCached`) que invalidan los cachés. Si un núcleo A procesa métodos y un núcleo B
prepara dibujos, B leería datos de dibujos posteriores. Es la misma clase de dependencia
que hoy cuesta 19 ms por cuadro entre el front y el back.

## Etapas

1. **Cola ordenada sin ganancia.** Un hilo de dibujos que recibe paquetes (copia del
   estado de Maxwell3D necesario para el dibujo + flags sucios) y ejecuta
   `RasterizerVulkan::Draw`; el back espera a que la cola se vacíe antes de cualquier otra
   operación que toque los cachés. Objetivo: imagen idéntica. Mide el coste de copiar el
   estado (~15 KB por dibujo; ~4.300 dibujos por cuadro).
2. **Escrituras de constantes en orden en el hilo de dibujos.** `WriteBlockCached` pasa a la
   cola; el back solo espera si lee esa memoria (macros con lecturas DMA, TIC/TSC).
3. **Limpiezas, consultas y cómputo en la cola** en vez de esperar.
4. **Estado incremental**: enviar solo los registros cambiados en lugar de la copia
   completa.

Cada etapa se compila con un interruptor (`EDEN_NX2_DRAW_THREAD=0/1`) y solo se prueba
en el juego al terminarla.
