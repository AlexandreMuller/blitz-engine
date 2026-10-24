# Plano de Otimização de Performance — Rasterização EEVEE/UPBGE (Blitz-Engine)

> **STATUS DA EXECUÇÃO (2026-10-16):** Fase 0 concluída (instrumentação + relatório em
> `docs/RELATORIO_FASE0_INSTRUMENTACAO.md`). Fase 1: 1.1 auditado (NO-OP, `samples_per_frame=1`),
> 1.2 parcial (hoist de flags), 1.3 não paga (foreach já filtra; custo é a iteração).
> Fase 2a/2b **inviáveis** (passes destruídos por frame — incerteza #3 do §6 respondida).
> 2c implementada com segurança (opt-in `BGE_GAME_FRUSTUM_CULL=1`) porém **sem ganho na City Street**
> (64–75% fora do frustum, mas todos a <50 m com margens de sombra seguras). Três crashes reais
> corrigidos no caminho (startup `style_get_dpi`, denoise first-frame, exit `python_main`).
> **Fase 3 concluída (2026-10-16):** breakdown `[view]`+`[deferred]` no dump. `main_view.render()`
> (3.1 ms móvel) = 97% deferred; dentro dele: **shadows 1.29 ms (GPU-drain, world sun global)**,
> gbuffer 0.71, setup 0.60, eval_light 0.35; denoise/combine/volume/ao/postfx ≈ 0. O caso móvel é
> **GPU-bound** (não CPU) — culling/sync não melhora fps. Headroom real = Fase 4 (pipelining/pacing)
> ou qualidade de sombra (fora do escopo). Itens originais da Fase 3 já eram no-op na cena.
> **Fase 4a implementada (2026-10-16):** render-ahead experimental (`BGE_RENDER_AHEAD=1`) — SwapBuffers
> bloqueante movido de `EndFrame()` para o início do próximo `BeginFrame()`. GPU_latency 2.23→0.42 ms
> (-81%), fps +7% mesmo com pacing artificial; screenshot A/B equivalente (max delta 4/255). 4b/4c
> (thread de sync / batching Vulkan) ficam como trabalho futuro de maior risco.
> Baseado na exploração da árvore-fonte em `K:\Blitz-Engine\blitz-engine` (UPBGE 5.3.0a).
> Profile de referência: cena street, ~200+ objetos, Vulkan — Frametime 11.38 ms (88.5 fps);
> Physics 0.09 / Logic 0.05 / Animations 0.06 / **Depsgraph 0.05** / SceneGraph 0.02 /
> **Rasterizer 9.40 ms (83%)** / GPU Latency 1.66 ms.

---

## 1. Diagnóstico: onde o tempo vai hoje

### 1.1 Correção da tese inicial (achado importante)

A tese "o EEVEE espera a avaliação do depsgraph de TODA a cena a cada frame"
**já está majoritariamente resolvida neste fork**. O código mostra:

- `KX_KetsjiEngine::NextFrame()` já tem avaliação lazy do depsgraph:
  `MarkDepsgraphDirty()`/`IsDepsgraphDirty()`, com env var `BGE_FORCE_DEPSGRAPH`
  para A/B de benchmark (`KX_KetsjiEngine.cpp:496-507`).
- `KX_GameObject::TagForTransformUpdate()` só marca `ID_RECALC_TRANSFORM` quando
  `m_requiresDepsgraphUpdate` é verdadeiro (modificadores, geometry nodes,
  constraints, shape keys, drivers, armatures); objetos de "puro movimento"
  (física/SceneGraph) copiam a matriz direto para `ob_eval` em
  `TagForTransformUpdateEvaluated()` sem avaliar o depsgraph
  (`KX_GameObject.cpp:386-416`).
- `KX_Scene::RenderAfterCameraSetup()` só chama `UpdateDepsgraph()` →
  `BKE_scene_graph_update_tagged()` quando sujo (`KX_Scene.cpp:955-963`).

O profile confirma: **Depsgraph = 0.05 ms**. O custo real está em outro lugar.

### 1.2 O que compõe os 9.40 ms do bucket `tc_rasterizer`

O bucket `tc_rasterizer` engloba todo `KX_KetsjiEngine::Render()` → `RenderCamera()` →
`KX_Scene::RenderAfterCameraSetup()` (`KX_KetsjiEngine.cpp:771-873, 1074-1105`), que contém:

1. **Dois loops completos sobre TODOS os objetos por passagem de câmera**
   (`KX_Scene.cpp:929-970`): `TagForTransformUpdate` (com `TagBlenderPhysicsObject`,
   `BKE_object_apply_mat4`, queries de partículas) e depois
   `TagForTransformUpdateEvaluated`. O(N × passes de câmera).
2. **Loop `samples_per_frame`**: `KX_Scene.cpp:1033-1044` chama
   `DRW_game_render_loop()` **N vezes por frame**, e cada chamada re-faz o sync
   completo (ver 1.3). Se `samples_per_frame > 1`, o custo de sync é multiplicado.
3. **Sync DRW full-scene**: `DRW_game_render_loop()` (`draw_context.cc:3158-3229`) →
   `engines_init_and_sync()` (`draw_context.cc:1333-1350`) → `foreach_obref_in_scene()`
   que itera **todos os objetos visíveis via `DEG_OBJECT_ITER`** a cada chamada
   (`draw_context.cc:1074-1223`), e para cada um `drw_engines_cache_populate()`
   (`draw_context.cc:1277-1309`).
4. **Sync EEVEE full-scene**: `Instance::begin_sync()` sincroniza **incondicionalmente
   ~17 módulos** por frame (`eevee_instance.cc:367-411`), e
   `Instance::object_sync()` → `SyncModule::sync_mesh()` roda **por objeto, todo
   frame**, sem "skip unchanged" (`eevee_sync.cc:225-288`): `velocity.step_object_sync()`,
   `material_array_get()`, `DRW_cache_object_surface_material_get()`, attach de passes.
5. **Extração de geometria**: `drw_batch_cache_generate_requested()` via `ExtractionGraph`
   a cada sync (`draw_context.cc:1305-1328`) — há cache por `last_update_geometry`,
   mas a validação e o enfileiramento rodam sempre.
6. **Pipeline EEVEE por viewport**: `render_sample()` (`eevee_instance.cc:559-605`) →
   `main_view.render()` → Prepass + HiZ + Shadows + Deferred/Forward + volume +
   motion blur + film (`eevee_pipeline.cc`).
7. **2D filters + blits**: `Render2DFilters()` e `DrawFrameBuffer` com cópia fullscreen
   por passagem de câmera (`KX_Scene.cpp:1046-1079`).

### 1.3 Mecanismos incrementais existentes

| Mecanismo | Estado | Local |
|---|---|---|
| Lazy depsgraph eval | ✅ implementado (fork) | `KX_KetsjiEngine.cpp:227-240`, `KX_Scene.cpp:955-963` |
| Tag granular por objeto (`m_requiresDepsgraphUpdate`) | ✅ implementado (fork) | `KX_GameObject.cpp:392-416` |
| Cache de batches por geometria (`last_update_geometry`) | ✅ upstream | `eevee_sync.cc:45`, `draw_cache_extract_mesh.cc` |
| Shadow tile update (`do_full_update_`, `update_tag_fb_`) | ✅ upstream (mas re-render por frame) | `eevee_shadow.cc:578-980, 1316` |
| Culling por fingerprint (`Manager::ensure_visibility`) | ✅ upstream (mas re-sync invalida) | `draw_manager.cc:238-258` |
| `depsgraph_last_update_` vs `DEG_get_update_count()` | ✅ existe, só usado p/ TAA reset | `eevee_instance.cc:177, 510` |
| Sync EEVEE "objeto não mudou → pular sync_mesh" | ❌ **não existe** | — |
| Sync DRW incremental entre samples/passes de câmera | ❌ **não existe** | — |

### 1.4 Sequenciamento logic vs render (sem pipelining)

`LA_Launcher::EngineNextFrame()` é estritamente sequencial: `NextFrame()` (lógica +
física substep + interpolação) → `RenderEngine()` → eventos (`LA_Launcher.cpp:501-538`).
Não há sobreposição entre CPU da lógica e submissão GPU do frame anterior.

---

## 2. Plano por fases

### FASE 0 — Instrumentação e confirmação do perfil (risco baixo, pré-requisito de tudo)

**Motivação:** os 9.40 ms estão distribuídos entre sync DRW, extração, pipeline EEVEE,
filtros 2D e blits, mas a fatia de cada um é desconhecida. O fork já tem
`last_sync_time_` em `DRWContext::engines_init_and_sync()` (`draw_context.cc:1335,1349`),
logging de perf a cada 60 frames em `NextFrame()` e `BGE_FORCE_DEPSGRAPH` — falta
granularidade dentro do render loop.

**Escopo:** `draw_context.cc` (`DRW_game_render_loop`), `eevee_instance.cc`
(`begin_sync`/`end_sync`/`render_sample`), `KX_Scene.cpp` (`RenderAfterCameraSetup`),
`KX_KetsjiEngine.cpp` (`Render`).

**Abordagem:**
1. Temporizadores scoped (`BLI_time_now_seconds` / `std::chrono`, já usado no fork)
   ao redor de: `acquire_data`, `engines_init_and_sync` (breakdown por engine),
   `engines_draw_scene`, `release_data`, loop de objetos em `RenderAfterCameraSetup`,
   `Render2DFilters`, e dentro de `render_sample()` (shadows, prepass, deferred,
   volume, motion blur, film).
2. Contadores: objetos iterados no `DEG_OBJECT_ITER`, objetos efetivamente syncados,
   batches extraídos vs. reutilizados, draw calls, samples por frame efetivo.
3. Script headless de benchmark: cena street via blender.exe/player headless, câmera
   em trajetória fixa, N frames, dump dos timers. Baselines: `BGE_FORCE_DEPSGRAPH=1`
   vs. `0`; `samples_per_frame=1` vs. atual.

**Aceite:** relatório com breakdown de ms por etapa; top-3 fatias; reproduzível (±10%).

**Riscos:** overhead de instrumentação desprezível; zero divergência upstream se
logging condicional.

---

### FASE 1 — Quick wins no loop do game engine (risco baixo, benefício imediato)

**Objetivo 1.1 — Auditar e corrigir `samples_per_frame`**
- *Motivação:* cada sample re-executa `DRW_game_render_loop()` completo, incluindo
  sync full-scene (`KX_Scene.cpp:1036-1044`). `Instance::init()` já desabilitou
  `sampling.reset()` em playback para permitir acumulação (`eevee_instance.cc:206-209`),
  então samples subsequentes deveriam apenas re-renderizar com jitter, sem re-sync.
- *Escopo:* `KX_Scene.cpp` (loop de samples), `DRW_game_render_loop`, `eevee_instance.cc`.
- *Abordagem:* separar "sync" de "draw sample": sincronizar uma vez, renderizar N
  samples com `sampling.step()` + `film.update_sample_table()` entre elas
  (`render_sample()` já é reentrante — `eevee_instance.cc:559-605`). Fallback via env var.
- *Aceite:* com `samples_per_frame=4`, frametime ≤ (1 sample) + 20%; screenshot A/B equivalente.
- *Risco:* TAA/ghosting; validar com A/B e cena animada.

**Objetivo 1.2 — Fundir os loops de objetos em `RenderAfterCameraSetup`**
- *Motivação:* dois loops O(N) completos por pass de câmera (`KX_Scene.cpp:929-970`)
  com chamadas pesadas (`BKE_object_apply_mat4`, alocação de `std::vector` de filhos
  em `KX_GameObject.cpp:372-384`, query de partículas por objeto).
- *Abordagem:* fusão em um único loop; hoisting da query de partículas (cache por
  frame); evitar alocação do vector de filhos quando vazio; pular
  `TagBlenderPhysicsObject` para objetos sem physics controller.
- *Aceite:* −0.2 a −0.5 ms na cena de 200+ objetos; sem mudança comportamental
  (screenshot byte-equal em cena com parent/dupli/partículas).

**Objetivo 1.3 — Eliminar trabalho redundante por pass de câmera**
- *Motivação:* câmeras custom viewports e overlay repetem o pipeline inteiro
  (`KX_KetsjiEngine.cpp:803-822`), com iteração completa mesmo para poucos objetos overlay.
- *Abordagem:* cachear o conjunto overlay/não-overlay por frame (depende só de
  `gameflag`); invalidação por tag.
- *Aceite:* overlay pass com custo proporcional ao número de objetos overlay.

---

### FASE 2 — Sync EEVEE/DRW incremental por objeto (risco médio-alto, maior benefício estrutural)

**Motivação:** `Instance::object_sync()` e `SyncModule::sync_mesh()` rodam para todo
objeto visível todo frame (`eevee_instance.cc:413-469`, `eevee_sync.cc:225-288`),
mesmo quando objeto, materiais e câmera não mudaram. Na cena street (objetos
majoritariamente estáticos), a maior parte desse trabalho é redundante. Provável
maior fatia dos 9.40 ms.

**Escopo:** `eevee_instance.cc`, `eevee_sync.cc`, `eevee_velocity.cc`, `draw_context.cc`
(`foreach_obref_in_scene`/`drw_engines_cache_populate`), `draw_manager.cc`.

**Abordagem (sub-etapas, cada uma testável):**
1. **2a. Chave de mudança por objeto:** usar `ob->runtime->last_update_transform`/
   `last_update_geometry` (já mantidos pelo depsgraph) + `DEG_get_update_count()`
   para decidir skip. Cuidados obrigatórios:
   - **velocity/motion blur precisa de re-sync quando a transformada muda** (objetos
     em movimento nunca podem ser pulados);
   - **TAA reset depende de mudanças** (`sampling.reset()` em `eevee_instance.cc:177-179`);
   - objetos com `GPU_MATFLAG_SCENE_TIME` nunca podem ser pulados.
2. **2b. Cache de passes por objeto:** os `PassMain::Sub` attachments de um objeto
   estático podem persistir entre frames se o `draw::Manager` não for resetado —
   investigar se `manager->begin_sync()` pode operar incremental (maior ponto de
   divergência upstream; prototipar atrás de flag de jogo).
3. **2c. Culling antes do sync:** `foreach_obref_in_scene` já filtra visibilidade
   (`DEG_iterator_object_is_visible`, `BKE_object_visibility`) mas não frustum.
   Adicionar teste de bounds da câmera ativa antes de `object_sync` — **exceto
   casters de sombra** e objetos com sombra volumétrica; usar
   `ShadowModule::compute_visibility` como referência (`eevee_shadow.cc:1276`).
4. **2d. Shadows:** garantir que `do_full_update_` só é setado quando luzes
   relevantes mudam (`eevee_shadow.cc:578-634`), e que o tile-update
   (`update_tag_fb_`) cobre objetos que entraram em cena.

**Aceite:**
- Cena street, câmera parada: sync time (medido na Fase 0) reduz ≥ 60%;
  frametime ≤ 6 ms (≥ 150 fps).
- Cena com todos os objetos animados: regressão ≤ 5% (skip é no-op quando tudo muda).
- Screenshots A/B dentro do limiar em câmera parada e móvel; sem flicker de
  TAA/sombra em 300 frames consecutivos.

**Riscos:** flickering temporal, motion blur quebrado, shadow popping, divergência
upstream em `draw_context.cc`. Mitigação: toggle runtime por sub-etapa, A/B headless,
`BGE_FORCE_DEPSGRAPH=1` como baseline permanente.

---

### FASE 3 — Otimização do pipeline de render por frame (risco médio)

**Motivação:** mesmo com sync incremental, `render_sample()` dispara shadows +
prepass + HiZ + deferred/forward + volume + motion blur + film todo frame
(`eevee_instance.cc:559-605`, `eevee_pipeline.cc`).

**Abordagem:**
1. **HiZ:** update só quando depth mudar (câmera+cena paradas → skip).
2. **Volume/irradiance probes:** confirmar que não re-renderizam em game a menos que
   `update_probes_this_sample_` (`eevee_instance.cc:531-551`).
3. **Motion blur:** se desabilitado na cena, garantir zero custo (early-out; medir).
4. **Light linking / cryptomatte:** custo zero quando desabilitados em jogo.
5. **Film/display:** avaliar blit direto sem framebuffer intermediário quando não há
   2D filters ativos (`KX_Scene.cpp:1046-1079`).

**Aceite:** cada pass < 0.1 ms em no-op; pipeline estático (câmera+objetos parados)
≤ 1.5 ms CPU-side.

**Risco:** médio — passes são shared com viewport Blender; mudanças condicionais a
contexto de jogo.

---

### FASE 4 — Latência GPU e pipelining do loop (risco alto, benefício estrutural)

**Motivação:** GPU Latency = 1.66 ms é swap/wait; lógica+física são minúsculas —
o frame é serial: [sync/extract CPU] → [submit GPU] → [swap wait]. Sem paralelismo
entre CPU do frame N e GPU do frame N−1.

**Abordagem:**
1. **4a. Frame pacing:** mover `SwapBuffers()`/`EndFrame` para o início do próximo
   frame (render-ahead de 1 frame), sobrepondo CPU do logic step com GPU do frame
   anterior. Pontos: `KX_KetsjiEngine::EndFrame()` (`KX_KetsjiEngine.cpp:294-337`),
   `LA_Launcher::EngineNextFrame()`.
2. **4b. Thread de sync:** a extração (`ExtractionGraph`, `draw_context.cc:1311-1331`)
   já é task-graph; avaliar sync EEVEE em job thread separada (depende da Fase 2 —
   sem sync incremental, mover para thread só move o custo).
3. **4c. Vulkan submission batching:** agrupar `DRW_submission_start/end`
   (`eevee_instance.cc:588-602`), reduzir sincronizações CPU-GPU.

**Aceite:** GPU latency de 1.66 ms → < 0.5 ms sem input lag perceptível;
frametime ≤ 7 ms (≥ 140 fps) na cena street.

**Risco:** alto — race conditions, tearing, input; por último, atrás de flag experimental.

---

## 3. Quick wins vs. trabalho estrutural

**Quick wins (Fase 0–1, dias):** instrumentação; auditoria de `samples_per_frame`;
fusão de loops em `RenderAfterCameraSetup`; cache do conjunto overlay; skip de blit
intermediário sem 2D filters; documentar `BGE_FORCE_DEPSGRAPH` como baseline.

**Estrutural (Fases 2–4, semanas):** sync incremental por objeto (peça central);
culling pré-sync; render-ahead pipelining.

## 4. O que NÃO tentar

- **Reescrever/substituir o DRW/draw manager** — custo de manutenção contra upstream mata o fork.
- **Modificar o núcleo do depsgraph** (`source/blender/depsgraph/`) — eval já custa 0.05 ms; zero ganho, máximo risco.
- **Trocar o backend Vulkan** — declarado OK pelo usuário.
- **Reescrever pipelines EEVEE (deferred/forward/shadow tracing)** — passes já existem e são shared com o DCC; otimizar no-op, não reescrever.
- **Threading do logic step** — lógica custa 0.05 ms; irrelevante.
- **Mudar formatos `.bsl.hh` de shader** — conflitos de merge constantes com upstream; manter patches mínimos.

## 5. Arquivos-chave (papel no caminho do frame)

1. `source/gameengine/Launcher/LA_Launcher.cpp` — loop principal; `NextFrame()` → `Render()` sem pipelining.
2. `source/gameengine/Ketsji/KX_KetsjiEngine.cpp` — `NextFrame()` (substeps física, interpolação, lazy depsgraph, `BGE_FORCE_DEPSGRAPH`), `Render()`, `RenderCamera()`.
3. `source/gameengine/Ketsji/KX_Scene.cpp` — `RenderAfterCameraSetup()` (loops de tagging, loop `samples_per_frame`, `DRW_game_render_loop`, 2D filters, blits), `UpdateDepsgraph()`.
4. `source/gameengine/Ketsji/KX_GameObject.cpp` — `TagForTransformUpdate()`/`TagForTransformUpdateEvaluated()`; granularidade de tag por objeto.
5. `source/blender/draw/intern/draw_context.cc` — `DRW_game_render_loop()`; `foreach_obref_in_scene()` (iteração full-scene via `DEG_OBJECT_ITER`); `drw_engines_cache_populate()`; `engines_init_and_sync()`; `ExtractionGraph`.
6. `source/blender/draw/engines/eevee/eevee_instance.cc` — `begin_sync()` (~17 módulos incondicionais), `object_sync()` (sem skip-unchanged), `render_sample()`, `depsgraph_last_update_`.
7. `source/blender/draw/engines/eevee/eevee_sync.cc` — `SyncModule::sync_mesh()` (sync por objeto por frame).
8. `source/blender/draw/engines/eevee/eevee_shadow.cc` — `ShadowModule` (`do_full_update_`, tile update tags, `render()` por frame).
9. `source/blender/draw/engines/eevee/eevee_velocity.cc` — `step_object_sync()` por objeto por frame.
10. `source/blender/draw/engines/eevee/eevee_pipeline.cc` — Prepass/Deferred/Forward render, HiZ, shadow render por view.
11. `source/blender/draw/engines/eevee/eevee_sampling.cc` — `Sampling::reset()/step()`; acumulação temporal.
12. `source/blender/draw/intern/draw_manager.cc` — `Manager::compute_visibility/ensure_visibility` (culling por fingerprint), `generate_commands`.
13. `source/blender/draw/intern/draw_cache_extract_mesh.cc` — extração de geometria com cache por `last_update_geometry`.

## 6. Incertezas a confirmar na Fase 0

- **`samples_per_frame` efetivo na cena do profile é desconhecido** — se for 1, o objetivo 1.1 vira no-op; imprimir o valor primeiro.
- **Quanto do sync por objeto é CPU vs. enfileiramento de extração** — a instrumentação dirá se o skip por objeto (2a) paga seu próprio custo.
- **`manager->begin_sync()` invalida tudo ou mantém recursos?** — se destrói os `PassMain` por frame, a sub-etapa 2b exige redesign maior que o estimado.
- **Comportamento do TAA em jogo** — com `sampling.reset()` a cada mudança de depsgraph e objetos em movimento, a acumulação pode nunca convergir; confirmar visualmente.
- **Custo real do blit final** (`GPU_viewport_draw_to_screen_ex` + `DrawFrameBuffer`) — a Fase 3 dirá se o blit direto vale.
