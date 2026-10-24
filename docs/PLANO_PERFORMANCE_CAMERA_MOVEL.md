# Plano — Queda de desempenho ao mover a câmera no jogo (rasterizer 3.79 → 15.45 ms)

> **EXECUÇÃO (2026-10-20/21):** Fases B1, B2, C1 implementadas e compiladas; Fase A
> parcial (instrumentação instalada, mas a medição dirigida definitiva exige a cena real
> em sessão interativa — ver "Resultados da execução" no final. Toggles: `BGE_TAA_RESET=1`
> (legado), `BGE_SHADOW_CLIPMAP_HALFRATE=1` (B2), `BGE_RENDER_AHEAD=0` (opt-out C1).
> O dump do profiler ganhou a linha `[taa]` (contadores dos gatilhos de reset e estado
> da acumulação — diagnóstico da Fase A/B1).



> **Data:** 2026-10-20. **Sintoma reportado (profile na tela):** câmera parada →
> Frametime 8.36 ms (119 fps), Rasterizer 3.79 ms (45%), GPU Latency 0.34 ms;
> câmera em movimento → Frametime 17.85 ms (56 fps), Rasterizer 15.45 ms (86%),
> GPU Latency 2.11 ms. Cena: City Street, Vulkan, Raytracing Screen-Trace +
> Denoising (Spatial Reuse + Temporal Accumulation + Bilateral) + Fast GI + SPFD.
> Complementa `docs/PLANO_PERFORMANCE_RASTERIZACAO.md` (Fases 0–4, plano anterior).

---

## 1. Diagnóstico (causas verificadas no código, com referências)

### 1.1 O que o bucket `tc_rasterizer` realmente mede

`tc_rasterizer` mede **wall-time de CPU** de toda a região de render
(`KX_KetsjiEngine.cpp:809` → `Render()` → `RenderCamera()` → `EndFrame`),
incluindo **back-pressure de submissão Vulkan**: não há NENHUM sync explícito
com a GPU no caminho (`DRW_game_render_loop` = acquire → sync → draw → release;
grep de `GPU_finish`/fence/readback no caminho do jogo = zero hits). O único wait
explícito é `SwapBuffers()`, sempre contabilizado em `tc_latency` ("GPU Latency")
— nos dois modos, com ou sem render-ahead (`KX_KetsjiEngine.cpp:306-308, 367-369`).

Conclusão: o salto 3.79 → 15.45 ms **não é wait de apresentação** (GPU Latency
subiu só 0.34 → 2.11 ms) — é **a GPU fazendo mais trabalho por frame com a câmera
em movimento e a CPU travando dentro da submissão** (fila de comandos/driver
saturada). Isso confirma a conclusão da Fase 3 do plano anterior: **o caso de
câmera móvel é GPU-bound**. O rasterizer é o termômetro; a doença é custo GPU.

Corolário importante: **nenhum passo EEVEE fica mais barato com a câmera parada**
(raytrace = 1 raio/px/closure sempre, denoise temporal+à-trous sempre, HiZ rebuild
todo frame, gbuffer/eval_light por pixel sempre). A linha de base de 3.79 ms não
é "barata por early-out" — o delta é trabalho NOVO disparado pelo movimento.

### 1.2 Causa A — `sampling.reset()` TODO frame em movimento (estabilidade temporal destruída)

Dois gatilhos independentes, ambos confirmados:

1. **`eevee_instance.cc:181-183`** — `depsgraph_last_update_ != DEG_get_update_count(depsgraph)`
   → `sampling.reset()`. Qualquer avaliação de depsgraph (objeto com modificador,
   constraint, driver, partículas — ou câmera logic-driven que force eval)
   reseta a acumulação. Em jogo com objetos animados, **isso tende a disparar
   todo frame**, não só com câmera em movimento.
2. **`eevee_instance.cc:414-416`** — `is_viewport() && velocity.camera_has_motion()`
   → `sampling.reset()`. O jogo roda no caminho viewport (`is_viewport()=true`),
   então **mover a câmera reseta a acumulação explicitamente, todo frame**.

Efeitos em cadeia do reset (`viewport_sample_ = 0`):
- **Film/TAA:** `use_history = false` (`eevee_film.cc:718-726`) — acumulação
  temporal desligada; sem convergência, imagem fica ruidosa (visível no
  screenshot 2: reflexos screen-trace granulados).
- **Volume:** `exponential_frame_count = 0` (`eevee_volume.cc:376-380`) —
  histórico volumétrico reinicia.
- Resets em cascata em light (`eevee_light.cc:398-402`), sphere probes
  (`eevee_lightprobe_sphere.cc:165`), materiais (`eevee_material.cc:196`),
  world (`eevee_world.cc:114`).

O histórico do **raytrace/denoise sobrevive ao movimento de câmera por
reprojeção** (`denoise_history_persmat`, `eevee_raytrace.cc:762-763`;
rejeição por tilemask + clamp YCoCg em `eevee_ray_denoise.bsl.hh:462-521`) —
ou seja, o pipeline JÁ TEM a infraestrutura de reprojeção; o reset blunt por
movimento de câmera é desnecessário para film/raytrace e só derruba a
convergência.

### 1.3 Causa B — clipmap de sombras do sol: tiles de borda re-renderizados por frame

Com a câmera parada e objetos estáticos, **zero tiles de sombra são
re-renderizados** (Fase 3 media `shadows 1.29 ms` só no caso móvel). Com a câmera
em movimento, a translação shifta a grade do clipmap e **todas as tiles que
entram pelas bordas de cada LOD são re-renderizadas todo frame**
(`eevee_shadow_tilemap_init.bsl.hh:94-108`: `do_update = !in_range_inclusive(tile_shifted, ...)`;
`grid_shift` em `eevee_shadow.cc:46-47`, comentário "Tag update from the tile-map
for sun shadow clip-maps shifting" em `:930`). Custo proporcional à velocidade
da câmera — é o maior candidato a fatia única do delta.

### 1.4 Causa C — screen-trace sob paralaxe (hipótese a confirmar por profile)

Screen-trace dispara **1 raio por pixel por closure ativa todo frame**, sem
redução por histórico (`eevee_ray_generate.bsl.hh:108-150`); marcha até 255
passos HiZ + 32 de refinamento (`eevee_ray_trace_screen_lib.bsl.hh:105-106, 172-173`).
Com câmera parada e histórico convergido, muitos pixels reutilizam radiância
reprojetada; em movimento contínuo a rejeição por disoclusão é alta → mais pixels
fazem marcha completa e o custo por pixel efetivamente sobe. **Requer confirmação**
com o breakdown `[deferred]` (timers `t_df_eval_light`/`t_df_denoise`) em
câmera parada vs. móvel — a Fase A abaixo cobre isso.

### 1.5 Causa D — render-ahead desligado por padrão

`BGE_RENDER_AHEAD` default é **OFF** (`KX_KetsjiEngine.cpp:293-301`). A Fase 4a
do plano anterior mediu, com ele ligado: GPU latency −81%, fps +7%. Não resolve
o GPU-bound, mas estabiliza o pacing e paga o almoço.

### 1.6 O que NÃO é causa (verificado, não perder tempo)

- Depsgraph/sync CPU: Fase 0/3 do plano anterior já mediram `tc_depsgraph ≈ 0.05 ms`,
  sync incremental caracterizado; a imagem parada/móvel não muda esse custo.
- Wait de swap/present: vai para `tc_latency` sempre (1.1).
- Culling/sync por objeto: GPU-bound — culling CPU não melhora fps (conclusão Fase 2c).
- `samples_per_frame`: auditado, = 1 (no-op).

---

## 2. Plano por fases

### FASE A — Medição dirigida (sem build, imediata)

**Objetivo:** quantificar o delta parado vs. móvel por passo GPU antes de codar.

1. Rodar o jogo (player embutido) com:
   `BGE_RENDER_PROFILE=C:/Users/alexa/Downloads/bge_prof.txt BGE_RENDER_AHEAD=1 "K:/Blitz-Engine/build_windows_Release_x64_vc17_Release/bin/Release/blender.exe"`
   (dump a cada 60 frames por padrão; `BGE_RENDER_PROFILE_INTERVAL=30` para janelas
   menores). Primeiro dump com câmera **parada** ≥ 60 frames; depois manter a
   câmera em **movimento contínuo** por ≥ 120 frames e capturar os dumps seguintes
   (o arquivo é append — janelas alternadas parado/móvel são comparáveis).
2. Comparar `[deferred] setup/shadows/gbuffer/eval_light/denoise/combine` e
   `[view]` entre janelas. **Critério de decisão:** se `shadows` dominar o delta
   → prioriza Fase B2; se `eval_light`/`denoise` dominarem → prioriza Fase B3/C2.
3. A/B com `BGE_RENDER_AHEAD=0` vs. `=1` no mesmo percurso de câmera.

**Aceite:** tabela ms por passo (parado vs. móvel) com o delta explicado ≥ 80%.

### FASE B — Estabilidade temporal em jogo (as mudanças comportamentais centrais)

**B1. Reset fino da acumulação em contexto de jogo** (médio risco, maior impacto
em qualidade/estabilidade):
- Suprimir o gatilho `camera_has_motion()` (`eevee_instance.cc:414-416`) quando
  em game render loop — a reprojeção por velocidade (film + raytrace) já cobre
  movimento de câmera.
- Trocar o gatilho de update count (`eevee_instance.cc:181-183`) por checagem
  granular (mesma filosofia do `m_requiresDepsgraphUpdate` do fork): resetar só
  em mudanças estruturais (resolução/rect `:191-199`, texturas deferidas
  `:506-508`, debug `:189`), não em transformadas.
- **Manter** o reset volumétrico se a reprojeção de volume se mostrar ruim em
  A/B (volumes não reprojetam bem — decidir com screenshots).
- Toggle runtime `BGE_TAA_RESET=0/1` (default novo comportamento em jogo) para
  A/B e rollback instantâneo.
- **Aceite:** 300 frames de câmera orbitando: sem ghosting perceptível em A/B
  screenshot; ruído de reflexo/penumbra visivelmente menor; frametime móvel ≤
  baseline atual.

**B2. Custo do clipmap de sombras em movimento** (médio risco):
- Opção barata: re-render das tiles shiftadas a cada 2 frames (alternando
  paridade), aceitando 1 frame de latência nas bordas da penumbra.
- Opção configurável: escala de resolução/LOD do shadow atlas para jogos
  (quality preset "Game").
- **Aceite:** `t_df_shadows` móvel −40% ou mais; sem popping visível em A/B.

**B3. Screen-trace sob movimento** (baixo risco, config + documentação):
- Verificar resolução efetiva do raytrace (1:2 já ajuda); orientar preset de
  qualidade para jogos (raytrace 1:4, precision 0.5) quando o alvo é 60 fps.
- Se a Fase A confirmar marcha longa dominando: avaliar cap de steps do
  screen-trace em game (patch mínimo, flag de cena) — cuidado com divergência
  upstream em `.bsl.hh`.

### FASE C — Throughput / pipelining (herda Fase 4 do plano anterior)

- **C1 (já implementado, só falta ligar):** default do render-ahead em jogo
  vira ON com opt-out (`BGE_RENDER_AHEAD=0`), ou decisão explícita de documentar
  `=1` no launcher. Ganho medido anterior: latency −81%, fps +7%.
- **C2 (futuro, alto risco):** Fase 4b/4c do plano anterior — thread de sync e
  batching Vulkan. Só depois que o custo GPU por frame estiver reduzido
  (B1–B3); pipelining não cura GPU-bound, apenas esconde wait.

### FASE D — Validação

- Headless A/B (scripts existentes do fork): screenshots byte-diff + métricas
  numpy (sem scipy) em câmera parada e em trajetória fixa.
- Soak test 300 frames com profile; comparar `tc_rasterizer`/fps contra os
  números deste relatório (8.36/3.79 ms parado; 17.85/15.45 ms móvel).
- Regredir `BGE_FORCE_DEPSGRAPH=1` como sanity check.

## 3. Riscos e o que não fazer

- **Não** reescrever passes EEVEE nem o draw manager (política do plano anterior).
- **Não** remover resets cegamente: texturas deferidas, mudança de resolução e
  overlays precisam continuar resetando (ghosting real).
- Mudanças B1 tocam comportamento de viewport DCC se mal gateadas — sempre
  condicionar ao game render loop (`DRW_game_render_loop` / contexto de jogo).
- A remoção do reset por update count é a mudança de maior risco (pode reintroduzir
  ghosting de objetos animados em viewport) — por isso o toggle `BGE_TAA_RESET`.

## 4. Arquivos-chave deste plano

| Arquivo | Papel |
|---|---|
| `source/blender/draw/engines/eevee/eevee_instance.cc:181-183, 414-416` | gatilhos de `sampling.reset()` — alvo B1 |
| `source/blender/draw/engines/eevee/eevee_sampling.cc:131-136, 316-320` | efeitos do reset |
| `source/blender/draw/engines/eevee/eevee_film.cc:718-726` | `use_history` — TAA em jogo |
| `source/blender/draw/engines/eevee/eevee_volume.cc:114, 376-380` | histórico volumétrico |
| `source/blender/draw/engines/eevee/shaders/eevee_shadow_tilemap_init.bsl.hh:94-108` | tiles shiftadas → re-render — alvo B2 |
| `source/blender/draw/engines/eevee/eevee_shadow.cc:30-70, 930` | `grid_shift` do clipmap |
| `source/blender/draw/engines/eevee/eevee_raytrace.cc:681, 747-775, 762-763` | reprojeção do histórico de raytrace (já existe) |
| `source/blender/draw/engines/eevee/shaders/eevee_ray_trace_screen_lib.bsl.hh:105-106` | marcha screen-trace — alvo B3 |
| `source/gameengine/Ketsji/KX_KetsjiEngine.cpp:292-311, 362-369` | render-ahead (C1) |
| `source/blender/draw/intern/draw_context.cc:3294-3413` | `BGE_RENDER_PROFILE` — instrumentação da Fase A |

---

## 5. Resultados da execução (2026-10-20/21)

### Implementado

**B1 — Reset fino da acumulação em jogo** (`eevee_instance.cc`, `eevee_sampling.cc`,
`draw_context.cc`, `DRW_render.hh`):
- Gatilho `camera_has_motion()` e gatilho de update count suprimidos no game render
  loop (`DRW_game_render_loop_is_active()`); viewport DCC inalterada.
- **Bug encontrado durante a execução:** suprimir os resets sem mais nada faz
  `viewport_sample_` convergir (16) e `finished_viewport()` ficar permanentemente
  verdadeiro — o EEVEE parava de re-renderizar e a imagem congelava. Correção: no game
  render loop o `interactive_mode_` fica permanentemente ativo (reprojeção por
  velocidade a cada frame), espelhando o que a navegação faz na viewport DCC.
- Toggle `BGE_TAA_RESET=1` restaura o comportamento legado (reset por frame + freeze
  de convergência). Diagnóstico no dump: linha `[taa] reset_camera | reset_depsgraph |
  viewport_sample | interactive`.

**B2 — Clipmap de sombras half-rate** (`eevee_shadow.cc`): em frames de paridade ímpar o
`grid_offset` do clipmap não segue a câmera (`grid_shift = 0`), zero tiles de borda
re-renderizadas; o delta acumula e é aplicado no frame par. Default OFF
(`BGE_SHADOW_CLIPMAP_HALFRATE=1` liga). A escolha do default final fica condicionada à
Fase A na cena real.

**C1 — Render-ahead default ON** (`KX_KetsjiEngine.cpp`): `BGE_RENDER_AHEAD` sem valor
ou com valor verdadeiro liga; `=0` desliga. Herda o ganho medido na Fase 4a.

**Fase A — Instrumentação:** contadores `[taa]` no dump; scripts de cena-proxy e A/B em
`areatest_tmp/fase_a/` (gen_proxy_scene.py, run_proxy.py, compare_shots.py).

### Medido (proxy headless, Vulkan, 720p/1080p, cidade sintética ~170 objetos)

- Comportamento B1 confirmado: com câmera móvel e TAA novo, `interactive 1.03`,
  re-render a cada frame (sem freeze); com `BGE_TAA_RESET=1`, `interactive 0.13` e
  freeze de convergência (baseline legado preservado).
- Regressão EEVEE fora de jogo: `docs/test_shadow_denoise_ab.py` passou (penumbra
  reduction 0.48 ≥ 0.20; highlight inerte).

### Limitações encontradas (importantes)

1. **A câmera movida por Python (`worldPosition`) no player embutido headless não
   altera a visão renderizada** — `camera_has_motion()` ficou 0.00 mesmo orbitando, e
   screenshots idênticos (objeto KX se move, mas o render EEVEE não acompanha nesse
   modo de execução). Consequentemente o custo "câmera móvel" (Causas A/B/C do §1)
   **não foi reproduzível no proxy headless**, e a tabela ms por passo parado-vs-móvel
   do aceite da Fase A precisa ser medida na sessão interativa com a City Street real
   (como o §2 originalmente descreve). A linha `[taa]` deve ler
   `reset_camera 1.00` com câmera em movimento quando o sync KX→depsgraph está ativo.
2. `makeScreenshot` falhou (0 bytes) em 1080p; funcionou em 720p mas captura a janela
   inteira — para o aceite visual de B1 (ghosting), usar screenshots do jogo em 720p
   ou captura interativa.
3. Vsync limita o proxy a 60 fps; para medir frametime móvel < 16.7 ms é preciso
   desligar vsync (`--gpu-vsync` / troca de janela) na sessão interativa.

### Pendente

- Fase A definitiva (tabela parado vs. móvel por passo) e decisão do default de B2 —
  sessão interativa na City Street real.
- Aceite visual de B1: 300 frames orbitando, A/B de screenshots, verificação de
  ghosting em reprojeção de volume (decisão documentada: manter reset volumétrico se
  ruim — o reset volumétrico em `eevee_volume.cc` não foi alterado).
- Fase D soak test contra os números do relatório (8.36/3.79 ms parado; 17.85/15.45
  ms móvel).
