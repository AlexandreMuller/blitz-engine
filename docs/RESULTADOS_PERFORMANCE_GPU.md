# Resultados — Performance GPU, Estabilidade e Escalabilidade (EEVEE/UPBGE)

> Resultados por fase do `PLANO_PERFORMANCE_GPU_ESCALABILIDADE.md`.
> Artefatos locais (JSONL brutos, traces, perfis, screenshots, fingerprints) ficam em
> `areatest_tmp/perf_gpu/out/`.

## Checkpoint Fase 4C — Authoring LOD e telemetria [PARCIAL] (2026-11-24)

**Escopo:** ferramenta local `areatest_tmp/perf_gpu/lod_authoring.py`, validação do campo
`lod_selection` com LOD ativo e correção do fechamento repetido do JSONL em `bench_tick.py`.

**Cena controlada:** cópia da City Street (`city_street_bench.blend`) com o objeto `Cube`.
Mesh avaliada base: 1.316 triângulos; níveis gerados com target final 0,5, distâncias 25/50 e
histerese 10%: 986 e 658 triângulos. O relatório do authoring não encontrou diferenças de
materiais, atributos, bounds ou flags de visibilidade.

**Validação de execução:** 3 repetições de 600 frames medidos + 120 warm-up em `still` e
`translate`, 1280×720, vsync OFF, Blender player Release, NVIDIA GeForce RTX 3060 / driver
621.24. Os 6 JSONL do controlador terminaram sem registros `error`; `gpu_dropped=0` e
`gpu_scopes_invalid=0`. Em amostras `cpu_frame`, `lod_selection` reportou nível 1
(`objects=1`, `mesh_triangles=986`) parado e nível 2 (`objects=1`, `mesh_triangles=658`) durante
translação.

**Limitação bloqueante:** o controlador encerrou após 600 frames de lógica, mas o profiler
registrou 256–558 frames renderizados por run. Apenas 136–215/600 amostras `still` e 438–466/600
amostras `translate` puderam ser correlacionadas pelo analyzer atual. O frametime de lógica e o
GPU total também divergiram, portanto os números não são um A/B válido nem evidência de ganho.
Não adotar LOD nem declarar aceite de performance com esses dados.

**Ponto de parada em 2026-11-24:** alinhar explicitamente frame lógico, câmera efetivamente
renderizada e `frame_id` GPU no harness; esse bloqueio foi fechado no checkpoint de 2026-11-26.
Os dados brutos desse primeiro lote permanecem em `areatest_tmp/perf_gpu/out/phase4c_lod/`.

## Checkpoint Fase 4C — Correlação por timestamp e matriz LOD [PARCIAL] (2026-11-26)

**Correção do harness:** removida a hipótese de que o frame de lógica `f` corresponde ao frame
GPU `f+1`. `bench_tick.py` grava `timestamp_ns` por tick e um evento `phase_end`;
`draw_context.cc` grava `frame_start_ns` monotônico por frame renderizado; ambos identificam
`clock_domain=monotonic_ns`. `analyze_bench.py` associa cada frame de render ao tick lógico mais
recente anterior, restringe os percentis GPU à fase medida e exclui frames posteriores ao marcador
de término. Arquivos antigos sem esses timestamps continuam sendo lidos, mas não recebem métricas
GPU correlacionadas.

**Validação:** build Release `blenderplayer` concluído com
`cmake --build K:\Blitz-Engine\build_windows_Release_x64_vc17_Release --target blenderplayer --config Release -- /m:4`.
Smoke real em player e fixture sintética confirmaram a associação muitos-para-um e a filtragem de
fase. Na cópia controlada da City Street com LODs, foram feitas 3 repetições por trajetória,
1280×720, vsync OFF, 120 ticks de warm-up e 600 medidos:

| Trajeto | Frames de perfil medidos/correlacionados por run | Amostras `gpu_total_ms` por run | p50 GPU total (ms) | p95 GPU total (ms) |
|---|---:|---:|---:|---:|
| still | 293–295 | 292–294 | 16,75–16,88 | 18,43–18,84 |
| translate | 579–582 | 578–581 | 8,35–8,46 | 10,40–10,51 |

Todas as seis runs terminaram com `gpu_dropped=0` e `gpu_scopes_invalid=0`. LOD selecionado nos
frames renderizados correlacionados: `still` manteve nível 1 (986 triângulos); `translate`
observou níveis 1 (986) e 2 (658). Um smoke separado, com câmera a 10 m do objeto, registrou
nível 0 (1.316 triângulos) em todos os frames medidos correlacionados. Os p50/p95 acima descrevem
trajetos diferentes e **não são um A/B nem evidência de ganho**; não houve run pareado sem LOD.
Esta matriz foi coletada antes da instrumentação adicional de 4C.2; não compare seus tempos
diretamente com os smokes posteriores de contadores.

**Qualidade visual ainda pendente:** capturas exploratórias em
`areatest_tmp/perf_gpu/out/phase4c_lod_visual/` mostram posições distintas da câmera e não
isolam a mudança de geometria. Fazer pares de imagens perto do limiar na mesma trajetória e
aprovar popping/hitches antes de adotar LOD. A instrumentação objetos→protótipos→grupos→draws
está concluída para contagem global e smoke estrutural; contagens por câmera/shadow view seguem
pendentes no gate 4C.4.

**Instrumentação 4C.2 [CONCLUÍDA]:** `BGE_GPU_PROFILE` agora inclui `scene_counts` (objetos KX,
objetos iterados/populados, passes de câmera e samples), `work.draw_prototypes` (registros
`DrawPrototype` gerados), `work.draw_groups` (`DrawGroup` gerados) e os contadores Vulkan de draw
e instâncias. `analyze_bench.py` agrega médias apenas nos frames renderizados correlacionados à
fase measure.

O smoke de City Street LOD (120 warm-up + 120 ticks medidos) produziu 102 frames renderizados
correlacionados. Médias por frame correlacionado: `scene_counts.kx_objects=399`,
`objects_iter=403`, `objects_populated=403`; `work.draw_prototypes=2513`,
`draw_groups=2513`, `draws=26`, `draws_indexed=2`, `draws_indirect=2513` e `instances=222`.

**Fixture de repetição conhecida:** com uma única mesh compartilhada, controle de 2 objetos
reportou 8 protótipos, 8 grupos e 8 draws indirect médios; a mesma cena com 64 cópias vinculadas
(66 objetos no total) reportou 392 protótipos, 8 grupos e 8 draws indirect. Isso confirma que os
contadores distinguem o aumento de protótipos da agregação em grupos/draws no mesmo frame. O
counter Vulkan `instances` ficou em 189 nos dois casos; ele não representa a multiplicidade
embutida nos comandos indirect e não deve ser usado como contagem de instâncias dessa via.

4C.2 fecha a instrumentação e o smoke estrutural, não um ganho de performance. Os valores são
agregados entre câmera/shadow views e não localizam onde cada grupo foi gerado; essa atribuição e
triângulos por view permanecem no gate 4C.4. Evidências: `areatest_tmp/perf_gpu/out/phase4c_instancing_smoke/`
e `areatest_tmp/perf_gpu/out/phase4c_instancing_fixture/`.

## Entrega 4 — Fechamento de limitações da Entrega 3 + Fase 3: Bins de closure, presets e controles independentes (2026-11-15)

**Escopo executado:** fechamento das limitações registradas na Entrega 3 (cobertura no quit e
orçamento de escopos GPU) + §6.1 (número real de bins de closure), §6.2 (caracterização
eixo-a-eixo e preset combinado) e §6.3 (`screen_trace_max_steps` e
`shadow_denoise_atrous_iterations` em DNA/RNA).

**Configuração:** commit base `27578909e93` + diff local `97bf06c9860a` (18 arquivos, +221/-19);
binário `blitzplayer_e4.exe` / `blender.exe` Release x64 (build 2026-11-08); bench City
Street, 1280×720, warmup 120 + 600 frames medidos (`e4_baseline` e `e4_preset_perf_3r` com
3 reps; varredura de eixo único com 1 rep por variante em `variants/`).

### Mudanças

| Arquivo | Mudança |
|---|---|
| `gpu/GPU_timestamp.hh`, `gpu/intern/gpu_timestamp{.cc,_context_private.hh}`, `gpu/vulkan/vk_context.{cc,hh}` | `GPU_timestamp_flush()` / `VKContext::timestamp_flush()`: drain bloqueante apenas no shutdown (`wait_queue_idle()` + `poll_all()`) para publicar os frames em voo ao sair |
| `draw/intern/DRW_render.hh`, `draw/intern/draw_context.cc`, `KX_KetsjiEngine.cpp` | `DRW_game_gpu_profile_flush()` chamado em `KX_KetsjiEngine::StopEngine()`; deduplicação de escopos repetidos por camada (`#2`, `#3`) no JSONL para evitar colisão de chaves na camada de refração |
| `gpu/vulkan/vk_timestamp.{hh,cc}` | `MAX_SCOPES` elevado de 64 para 128 (~86 escopos/frame com 2 camadas deferred); sentinela `-1` em `scope_stack_` quando houver overflow para não dessincronizar escopos pais |
| `eevee_material.hh`, `eevee_raytrace.cc` | §6.1: `to_gbuffer_bin_mask()` + dimensionamento de `tile_raytrace_{denoise,tracing}_tx_` por `closure_bin_layers` real; `trace(..., false, ...)` mantido para bins inativos (preserva dummy/liberação de histórico); `wc.rt_closure_bins_active` conta bins reais (2 na City Street) |
| `DNA_scene_types.h`, `rna_scene.cc`, `eevee_raytrace{.cc,_shared.hh}`, `eevee_ray_trace_screen_lib.bsl.hh` | §6.3: `ray_tracing_options.screen_trace_max_steps` (1–512, default 255; arquivos antigos lêem 0 e caem no fallback 255) |
| `DNA_scene_types.h`, `rna_scene.cc`, `eevee_pipeline.cc` | §6.3: `eevee.shadow_denoise_atrous_iterations` (1–4, default 4; último passe ativo assume `atrous_final`; arquivos antigos caem no fallback 4) |
| `gpu/tests/storage_buffer_test.cc` | Adicionado `test_storage_buffer_read_if_ready` (§4.1) |

### Fechamento das limitações da Entrega 3

- **Cobertura no quit:** subiu para **99,8–99,9%** em todas as 6 runs de `e4_baseline`
  (719/720, 651/652, 655/656, 731/732, 720/721, 706/707 frames com `gpu_total_ms`; apenas o
  último frame sem `SwapBuffers` antes de `StopEngine()` fica fora).
- **Orçamento de escopos:** `MAX_SCOPES=128` eliminou o overflow silencioso da Entrega 3,
  revelando a decomposição real de `sv.deferred` (7,55 ms em `still`): `df.eval_light`
  (2,45 ms), `shd.temporal` + `shd.atrous0..3` (1,88 ms), `df.gbuffer` (1,55 ms),
  `rt1.*` (1,16 ms), `df.prepass` (1,14 ms) e `fastgi.*` (0,96 ms). `gpu_dropped=0` e
  `gpu_scopes_invalid=0` em todas as runs.
- **GTests (`WITH_GTESTS=OFF`):** segue pendente de execução em build com `WITH_GTESTS=ON`.

### A/B de performance — Baseline e varredura eixo-a-eixo (City Street, 1280×720, ms)

Defaults mantidos sem regressão (`e4_baseline`, 3 reps: `still` `frame_p50=8,422 ms`,
`gpu_p50=8,441 ms`, `gpu_p95=9,622 ms`; `translate` `frame_p50=7,629 ms`,
`gpu_p50=7,569 ms`).

| Variante (`still`) | Configuração alterada | `frame_p50` | `gpu_p50` | `gpu_p95` | `sv.deferred` | Passe alvo (mediana) |
|---|---|---:|---:|---:|---:|---|
| `e4_baseline` (3r) | defaults (bins=2 real) | 8,422 | 8,441 | 9,622 | 7,55 | — |
| `e4_atrous2` | `shadow_denoise_atrous_iterations=2` | 8,062 | 8,062 | 9,432 | 7,20 | `shd.*`: 1,88 → 1,23 ms (-34,6%) |
| `e4_steps64` | `screen_trace_max_steps=64` | 8,457 | 8,479 | 9,404 | 7,71 | `rt1.*`: 1,16 → 1,16 ms (sem cauda) |
| `e4_rt1_4` | `ray_tracing_options.resolution_scale='4'` | 8,430 | 8,425 | 9,588 | 7,54 | `rt1.*`: 1,16 → 0,99 ms (-14,7%) |
| `e4_fgi1_4` | `fast_gi_resolution='4'` | 8,107 | 8,146 | 9,353 | 7,24 | `fastgi.*`: 0,96 → 0,48 ms (-50,0%) |
| `e4_rt_nodenoise` | `ray_tracing_options.use_denoise=False` | 7,548 | 7,475 | 8,127 | 6,23 | `rt1.*`: 1,16 → 0,35 ms (-69,8%) |
| `e4_preset_perf_3r` (3r) | `atrous=2` + `steps=64` + `rt=1:4` + `fgi=1:4` | **7,721** | **7,729** | **8,630** | **6,47** | GPU p50 **-8,4%**, p95 **-10,3%** |

Em `translate` (3 reps), `e4_preset_perf_3r` obteve `frame_p50=7,605 ms` e
`gpu_p50=7,521 ms` (vs `7,629 / 7,569 ms` no `e4_baseline`).

### Regressão funcional

- `docs/test_shadow_denoise_ab.py` (`blender.exe --background --factory-startup`): **PASS**
  (`penumbra` high-pass std reduction = 47,9%; `highlight` grad_ratio = 1,023,
  peak_ratio = 1,000).

### Decisão e próximos passos (Fase 4)

- **Adotar** o código da Entrega 4 mantendo os defaults conservadores (`screen_trace_max_steps=255`,
  `shadow_denoise_atrous_iterations=4`), deixando os presets para opt-in por cena.
- A decomposição corrigida mostra que `df.eval_light` (2,45 ms) + `shd.*` (1,88 ms) +
  `df.gbuffer`/`df.prepass` (2,69 ms) dominam `sv.deferred` (7,55 ms), enquanto `rt1.trace`
  (0,13 ms) não é gargalo — guiando diretamente a priorização da Fase 4 (§7).

## Entrega 3 — Fase 2: Timestamps GPU Vulkan assíncronos, buckets de espera CPU e contadores de trabalho (2026-11-07)

**Escopo executado:** §5 (timestamps GPU assíncronos por frame, esperas CPU separadas por
bucket, contadores de trabalho GPU por passe; overhead ≤ 5% p50).

**Configuração:** commit base `74d07790e3b` + diff local `13db873023cb` (25 arquivos
modificados, +772/-26, + 6 arquivos novos); mesmo hardware/build das entregas 1-2; runs em
`out/e3_baseline` (sem `--gpu-profile`) e `out/e3_gpuprof2` (com `--gpu-profile`,
`BGE_GPU_PROFILE` ativo). Bench: City Street, 1280×720, warmup 120 + 600 frames medidos ×
3 reps por trajeto (still, translate).

### Mudanças

| Arquivo | Mudança |
|---|---|
| `gpu/GPU_timestamp.hh`, `gpu/intern/gpu_timestamp.cc` (novos) | API pública gated por `BGE_GPU_PROFILE`: `GPU_timestamp_frame_begin/end` + frame_id global de gravação (`GPU_timestamp_recording_frame_id()`) para amarrar múltiplos contextos ao mesmo frame |
| `gpu/vulkan/vk_timestamp.{hh,cc}` (novos) | `VKGpuTimestampRing`: ring de 8 slots × 128 queries (2 por escopo), keyed por frame_id; poll com `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` — nunca espera (`VK_NOT_READY` → slot segue PENDING); ring cheio → drop + contador `gpu_dropped`; reset só na reutilização do slot como primeiro nó do frame; registry estático multi-contexto (`poll_all`) com dedup de drop por frame |
| `gpu/vulkan/render_graph/nodes/vk_write_timestamp_node.hh` (novo) | nó do render graph que escreve o timestamp; ordem do scheduler preservada |
| `gpu/vulkan/render_graph/tests/vk_render_graph_test_timestamp.cc` (novo) | teste de ordem do scheduler (code-only: `WITH_GTESTS=OFF` neste build) |
| `gpu/vulkan/vk_context.{cc,hh}` | `frame_begin` preguiçoso no primeiro `timestamp_scope_begin` do frame; fechamento do frame no `end_frame()` (contexto offscreen) ou no swap handler (contexto da janela, após o blit); poll/totais via registry; **correção:** `deactivate()` não descarta mais a gravação em andamento |
| `gpu/intern/gpu_context_private.hh`, `gpu/CMakeLists.txt` | wiring da interface `GPUContext` de timestamps + build |
| `GHOST_ContextVK.cc` | buckets de espera CPU separados: `frame_slot_fence_wait`, `present_submit`, `swapchain_acquire`, `wait_for_submission`, `submit_enqueue`, `render_graph_build/record` |
| `draw/intern/draw_context.cc`, `DRW_render.hh` | `DRW_game_gpu_profile_begin/end_frame`; `gpu_total_ms` emitido apenas pelo registro que contém o escopo `frame_total` (contexto da janela) |
| `eevee_{pipeline,raytrace,shadow,view}.cc`, `eevee_shadow.hh` | escopos GPU nomeados por passe (`sv.*`, `df.*`, `rt*`, `shadow.*`, `shd.*`, `fastgi.*`) |
| `KX_KetsjiEngine.cpp`, `KX_GameObject.cpp` | ativação do perfil GPU no game loop quando `BGE_GPU_PROFILE` está definido |
| `areatest_tmp/perf_gpu/analyze_bench.py` | merge por frame_id dos registros de múltiplos contextos (união de escopos; `gpu_total_ms` do registro que o possui; soma de `scopes_invalid`) |

### Defeitos encontrados e corrigidos

1. **Discard no deactivate:** o game engine desativa o contexto da janela logo antes do
   present (`GPU_context_main_unlock`), e `VKContext::deactivate()` descartava a gravação
   em andamento — zerando silenciosamente a telemetria de todo frame. Corrigido deixando o
   ring intacto (a gravação é fechada e submetida depois, no swap handler, que flui o mesmo
   render graph).
2. **Render multi-contexto:** o DRW renderiza a cena EEVEE do jogo num contexto GPU
   offscreen compartilhado dedicado (O), diferente do contexto da janela (W) — todos os
   escopos EEVEE eram gravados no ring de O e descartados ao medir apenas W. Solução:
   frame_id global na thread principal + registry de rings; O fecha no `end_frame()`, W
   fecha no swap handler; merge por frame_id na análise. Diagnosticado via contadores de
   debug (`NO-RECORD` = 774 escopos/frame) em run curto instrumentado.

### Verificação do aceite (§5)

- **2 queries/escopo; ring 8 slots × 128 queries keyed por frame_id; reset só na
  reutilização como 1º nó do frame; ordem do scheduler preservada:** inspeção de código +
  gtest de scheduler (code-only neste build, `WITH_GTESTS=OFF`).
- **Poll nunca espera:** `WITH_AVAILABILITY` + `VK_NOT_READY` mantém PENDING; nenhum
  `GPU_finish()` adicionado; latência de publicação observada ~2 frames.
- **Cobertura:** 500-600 de 600 frames medidos com `gpu_total_ms` por run (83-100%); os
  frames ainda em voo no quit não resolvem — benigno. 44 escopos distintos cobrindo
  `sv.*`, `df.*`, `rt*`, `shadow.*`, `shd.*`, `fastgi.*`.
- **`gpu_dropped=0`, `scopes_invalid=0`** em todas as 6 runs (o ring de O usa 64 de 64
  escopos/frame — orçamento no limite, monitorar se o EEVEE adicionar passes).
- **`BGE_RENDER_PROFILE` inalterado:** formato dos `prof_*.txt` idêntico (valores de tempo
  diferem, como esperado).
- **Erros de harness:** 1-3 `ValueError: I/O operation on closed file` por run (flush no
  quit) são pré-existentes e aparecem igualmente no baseline.

### A/B de frametime (600 frames × 3 reps, mediana das medianas, ms)

| Trajeto | baseline p50 | gpu-profile p50 | Δ p50 | GPU p50 (novo) |
|---|---:|---:|---:|---:|
| still | 8,225 | 8,389 | +2,0% | 8,383 |
| translate | 7,559 | 7,618 | +0,8% | 7,536 |

Buckets de espera CPU (média/frame, `still_r1`): `wait_for_submission` 1,86 ms,
`render_graph_record` 1,72 ms, `frame_slot_fence_wait` 1,02 ms, `submit_enqueue` 0,41 ms,
`render_graph_build` 0,23 ms, `present_submit` 0,06 ms, `swapchain_acquire` 0,003 ms.

### Decisão

**Adotar.** Overhead de p50 ≤ 2% (abaixo dos 5% exigidos), zero drops/inválidos, e a
decomposição GPU por passe (`df.*`, `rt*`, `shadow.*`, `fastgi.*`, `shd.*`) fica disponível
para priorizar a Fase 3. Limitações conhecidas: cobertura de ~85-100% dos frames medidos
(cauda em voo no quit); orçamento de 64 escopos/frame no limite; gtest de scheduler
pendente de execução num build com `WITH_GTESTS=ON`.

## Entrega 2 — Fase 1: Readbacks Vulkan não-bloqueantes + estatísticas de sombra (2026-10-27)

**Escopo executado:** §4.1 (`read_if_ready()` Vulkan) e §4.2 (readback síncrono de
estatísticas de sombra no jogo).

**Configuração:** commit `85a4dfe5a88` + diff local `29ec854c3aba` (5 arquivos, +209/-68);
mesmo hardware/build da entrega 1; runs em `out/e2_new` (default) e `out/e2_legacy`
(`BGE_SHADOW_STATS_LEGACY_READ=1`).

### Mudanças

| Arquivo | Mudança |
|---|---|
| `vk_storage_buffer.cc`, `vk_vertex_buffer.cc` | `read_if_ready()` não chama mais `wait_for_timeline()`: consome apenas quando `submission_finished_timeline_get()` cobre a timeline pendente; sem resultado pronto retorna `false` **sem wait, sem flush e sem nova cópia** sobre o staging/mapped buffer em uso; `read_if_ready_timeline_`/`in_use_` só são limpos após consumo válido; invalidation de memória mapeada preservada |
| `DRW_gpu_wrapper.hh` | novo `StorageCommon::read_if_ready()` no wrapper DRW (usa `GPU_storagebuf_read_if_ready` já existente) |
| `eevee_shadow.cc` | no game render loop, `ShadowModule::init()` troca o `read()` síncrono por poll atrasado; warnings só com estatística válida (sem warning de dado obsoleto); o `async_flush_to_host()` do render loop é pulado nesse modo (o próprio poll submete a cópia do próximo frame — sem cópia+flush duplicados). `BGE_SHADOW_STATS_LEGACY_READ=1` restaura o comportamento antigo; F12/light bake e viewport DCC inalterados |
| `storage_buffer_test.cc` | novo teste `storage_buffer_read_if_ready`: polls sucessivos com a fila ocupada, dados corretos, ciclo repetido sem crash, caminho síncrono funcional após polling (compila quando `WITH_GTESTS=ON`; a suíte está OFF neste build) |

### Verificação do aceite

- **Zero wait no caminho non-blocking:** `read_if_ready()` não contém mais nenhuma chamada a
  `wait_for_timeline()` (verificado estaticamente; o wait permanece apenas em `read_async`,
  fence e work-in-flight — caminhos explicitamente bloqueantes).
- **Imagem semanticamente neutra:** shot `translate_r0_f0420` legacy×novo: 99,95% dos pixels
  idênticos (0,045% > 0,05; TAA jitter), zero NaN.
- **Vulkan validation:** run de 150 frames com `-d gpu` — zero erros de validation, race ou
  assert (apenas warnings pré-existentes de meshes vazios da cena).
- **Warnings de sombra:** emitidos somente após o primeiro consumo válido do poll; cadência
  preservada pela swap-chain de 5 buffers (dados com a mesma idade de antes).

### A/B de frametime (City Street, 1280×720, 600 frames × 3 reps, mediana, ms)

| Trajeto | legacy p50/p95/p99 | novo p50/p95/p99 | Δ p50 |
|---|---|---|---:|
| still | 8,65 / 10,38 / 13,85 | 8,65 / 10,24 / 14,21 | -0,0% |
| translate | 7,83 / 14,51 / 15,28 | 7,57 / 14,76 / 15,46 | -3,4% |
| teleport | 8,56 / 13,81 / 14,97 | 8,32 / 10,58 / 15,71 | -2,7% |

O ganho de p50 é pequeno e dentro da variância inter-rep (~5% medida na entrega 1). O p95 do
trajeto teleport melhora ~23% (13,81 → 10,58 ms), coerente com a remoção de um wait de
timeline por frame, mas 3 repetições não bastam para declará-lo. Conforme o aceite do plano
(§4.2): "se o ganho for pequeno, a correção de contrato ainda permanece válida, mas sem
promessa de speedup".

### Decisão

**Adotar** como correção de contrato (o caminho documentado como non-blocking agora é de
fato non-blocking e não agenda cópias concorrentes), **sem** contabilizar speedup rumo às
metas de 30%. O fallback `BGE_SHADOW_STATS_LEGACY_READ=1` permanece disponível para A/B.
Próximo passo: entrega 3 (timestamps GPU Vulkan assíncronos e contadores de trabalho, §5),
que dará a decomposição por passe necessária para priorizar a Fase 3.

## Entrega 1 — Fase 0: Harness confiável e baseline interativa (2026-10-25)

**Escopo executado:** §3.1 (instrumentação da cadeia de câmera + contadores TAA separados),
§3.2 (controlador de benchmark com trajetos determinísticos, JSONL por frame, sumário
p50/p95/p99 e fingerprint de configuração) e §3.2 matriz em cena-proxy e City Street.

**Configuração:** commit `70d12c23837` + diff local `c5f8d5d0` (7 arquivos, +237/-19);
build Release x64; GPU NVIDIA GeForce RTX 3060 (driver 617.14); Windows 10 (26220);
vsync OFF; janela fixa 1280×720; sem `time.sleep()`; 120 frames de warm-up + 600 medidos,
3 repetições por trajeto. Player standalone (`blenderplayer.exe`), modo custom render loop
(`vp_render=0`).

### Ferramenta entregue (`areatest_tmp/perf_gpu/`)

| Arquivo | Papel |
|---|---|
| `bench_tick.py` | Controlador embarcado na cena: trajetos still/translate/rotate/rottrans/deocc/teleport por frame de lógica, JSONL por frame, término determinístico com `logic.endGame()` |
| `setup_bench_scene.py` | Injeta o controlador na câmera ativa de qualquer .blend, fixa `use_frame_rate=False` e `vsync='OFF'`, grava sidecar `*.eevee.json` com as opções EEVEE |
| `run_bench.py` | Driver: executa trajeto×repetição no player/embedded, grava fingerprint (commit, hash do diff, GPU/driver, flags `BGE_*`), sumariza com `analyze_bench.py` |
| `analyze_bench.py` | Sumário p50/p95/p99/máx/desvio por run (fase measure, teleportes separados) |

### Instrumentação entregue (C++, gated por env var, no-op quando desligada)

| Componente | Local | Ativação |
|---|---|---|
| `DRW_bge_cam_trace_begin_frame/stage` | `draw_context.cc` | `BGE_CAM_TRACE=<arquivo.jsonl>` |
| Estágios `frame_begin`, `kx_sg`, `ob_orig`, `ob_eval`, `rv3d`, `velocity` | `KX_KetsjiEngine.cpp`, `KX_Scene.cpp`, `eevee_instance.cc` | mesma env var; mesmo `frame_id` em todas as linhas |
| Contadores TAA separados (`cam_trigger`, `cam_reset`, `deg_trigger`, `deg_reset`) | `DRW_render.hh`, `eevee_instance.cc`, dump em `draw_context.cc` | `BGE_RENDER_PROFILE` (linha `[taa]`) |
| `--gpu-vsync <on\|off\|auto>` no blenderplayer | `GPG_ghost.cpp` | flag de linha de comando |

### Defeito encontrado e corrigido (§3.1, causa raiz)

O player standalone **ignorava a câmera ativa da cena** quando o layout salvo no .blend não
tinha o viewport em modo de câmera: `BL_DataConversion.cpp` só promove `scene->camera` a
câmera ativa do jogo se `rv3d->persp == RV3D_CAMOB`. No player isso vinha do layout salvo,
então o jogo renderizava a partir de uma `__default__cam__` **estática** derivada da posição
do rv3d — exatamente o sintoma "tick executa, posição muda no script, imagem não muda" e
`reset_camera 0.00` em movimento (o movimento não chegava ao `VelocityModule`).

Correção: `LA_PlayerLauncher::InitCamera()` agora força `rv3d->persp = RV3D_CAMOB` quando a
cena tem câmera ativa (o embedded preserva o comportamento de override de viewport de
`LA_BlenderLauncher`). Com isso o player standalone sempre renderiza de `scene->camera`.

Também corrigido nos scripts auxiliares: uso da API inexistente `KX_Scene.camera`
(`dbg_cam2.txt`) — a API é `active_camera`/`cameras`.

### Validação do aceite (§3.1)

Cadeia `KX SceneGraph → ob_orig → ob_eval → rv3d → VelocityModule` traçada com o mesmo
`frame_id` por frame. Métrica: fração de frames cuja matriz (hash FNV-1a quantizado a 1e-4)
mudou em relação ao frame anterior, por estágio:

| Trajeto (proxy ×3) | kx_sg | velocity | desalinhamento entre estágios |
|---|---:|---:|---:|
| still | 0.00/f | 0.00/f | 0 |
| translate | 0.57–0.59/f | idem | 0 |
| rotate | 0.55–0.56/f | idem | 0 |
| rottrans | 0.60–0.61/f | idem | 0 |
| deocc | 0.01/f | idem | 0 |
| teleport | 0.00/f (6 transições, todas com mudança de hash nos 5 estágios) | idem | 0 |

| Trajeto (City Street ×3) | kx_sg | velocity | desalinhamento |
|---|---:|---:|---:|
| still | 0.00/f | 0.00/f | 0 |
| translate | 0.93–0.94/f | idem | 0 |
| rotate | 0.95–0.97/f | idem | 0 |
| rottrans | 0.95–0.96/f | idem | 0 |
| deocc / teleport | 0.01–0.02/f | idem | 0 |

A razão <1.00 em trajetos contínuos é o **logic frame skipping** (a lógica roda a 60–120 Hz
mas a renderização entrega ~120 fps na proxy e ~125 fps na City Street a 1280×720): há frames
renderizados sem tick de lógica, e nesses a câmera não muda — comportamento correto.

- **Movimento Python e movimento por atuador nativo:** ambos propagam (atuador MOTION:
  0.47/f na proxy; o tick ativa o atuador em modo `BGE_BENCH_DRIVE=actuator` porque o
  controlador Python não dispara atuadores ligados sem `cont.activate()`).
- **Contadores TAA (City Street, última janela):** `cam_trigger` 0.93–0.95/f em movimento,
  **0.00/f parado**; `cam_reset` e `deg_reset` **0.00** sempre (B1 ativo); `deg_trigger` 0.00.
- **Screenshots de pontos distintos do trajeto:** translate f0120 × f0719 na City Street:
  75,6% dos pixels diferem >0.05, mean_abs_diff 0.21 — a imagem muda de verdade.
- **Repetibilidade:** p50 entre repetições varia <3% na proxy e <2% na City Street.

### Baseline de frametime (fase measure, p50/p95/p99 em ms)

Proxy scene (1280×720, 600 frames, 3 reps, melhor-mediana):

| Trajeto | p50 | p95 | p99 |
|---|---:|---:|---:|
| still | 8,39–8,52 | 10,2–12,2 | 12,3–15,7 |
| translate | 8,23–8,42 | 10,8–12,2 | 13,8–14,9 |
| rotate | 8,27–8,31 | 11,5–11,9 | 13,9–14,9 |
| rottrans | 8,44–8,55 | 11,4–11,6 | 14,3–15,4 |
| deocc | 8,42–8,51 | 11,2–11,8 | 13,8–14,9 |
| teleport | 8,35–8,39 | 11,2–12,1 | 14,3–15,2 |

City Street (1280×720, 600 frames, 3 reps):

| Trajeto | p50 | p95 | p99 | máx |
|---|---:|---:|---:|---:|
| still | 8,11–8,45 | 10,9–14,4 | 16,0–17,5 | 29,4–30,4 |
| translate | 7,69–7,83 | 15,3–15,5 | 15,9–16,6 | 24,8–25,1 |
| rotate | 8,17–8,34 | 10,6–14,0 | 16,7–17,2 | 27,3–55,4 |
| rottrans | 8,04–8,21 | 13,5–14,6 | 16,1–17,8 | 28,7–30,6 |
| deocc | 7,74–7,93 | 14,5–15,0 | 16,1–17,0 | 26,3–60,5 |
| teleport | 8,06–8,12 | 10,3–15,2 | 15,5–17,0 | 27,5–28,8 |

Observação: o p95 do caso "still" da City Street não é materialmente melhor que os trajetos
móveis — coerente com o relato anterior de que o pipeline completo re-renderiza a cada frame
no modo de jogo, convergência de TAA não reduz o trabalho por frame.

### Limitações abertas

1. **Modo embedded não fecha sozinho via `logic.endGame()`** quando executado de forma
   autônoma: o loop do launcher bloqueia o main loop do Blender e `bpy.ops.wm.quit_blender()`
   só roda depois do jogo sair. Além disso o embedded ignora vsync/geometry flags do player.
   Conforme o plano, **o player standalone é a referência** e a limitação do embedded
   permanece explicitamente aberta.
2. `blenderplayer.exe` desta máquina tem uma entrada IFEO no registro apontando para um
   debugger ausente (`C:\Program`), o que impede o lançamento por `CreateProcess`. Os runs
   usaram a cópia idêntica renomeada `blitzplayer_bench.exe` (mesmo diretório, mesmo build).
   Recomenda-se remover a chave IFEO.
3. O `dt_ms` do JSONL mede delta entre ticks de lógica (≈ frametime com frame skipping);
   buckets CPU separados (fence/acquire/submit/present) são Fase 2/3 do plano.
4. Fingerprint de GPU/driver no `fingerprint.json` veio "unknown" quando a consulta ao
   blender falhou; o trace C++ grava GPU/driver corretos no cabeçalho do camtrace.

### Decisão

**Adotar.** O gate do plano ("matrizes e imagens mudam de verdade; runs reproduzíveis") está
cumprido para o player standalone. Próximo passo recomendado: entrega 2 do plano (correção
dos readbacks Vulkan e estatísticas de sombra assíncronas, §4).
