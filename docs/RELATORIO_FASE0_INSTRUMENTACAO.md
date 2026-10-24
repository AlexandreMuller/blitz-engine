# Relatório Fase 0 — Instrumentação do Render Loop + Resultados

> Cena: `samples/City Street.blend` (415 objetos: 390 mesh, 18 light, 4 font, 2 empty, 1 camera).
> Binário: `build_windows_Release_x64_vc17_Release\bin\Release\blenderplayer.exe` (Release x64).
> Metodologia: 600 frames por run, auto-exit via script `__main__` com pacing de 20 ms
> (`time.sleep` no loop Python — necessário porque o loop apertado não avança o relógio real
> e `NextFrame()` retorna `frames==0` sem renderizar). Perfil medido com `BGE_RENDER_PROFILE=<arquivo>`
> (dump próprio; o stdout do player não é capturável de forma confiável). Janela: média de 60 frames.
> Variação entre runs repetidas: 5–7% (dentro do aceite ±10%).

## 1. Ativação da instrumentação

- `BGE_RENDER_PROFILE=1` → dump para stdout a cada 60 frames.
- `BGE_RENDER_PROFILE=<caminho>` → dump para arquivo (recomendado no player).
- `BGE_RENDER_PROFILE_INTERVAL=N` → muda a janela (padrão 60).
- Overhead quando desligado: um `bool` por timer (RAII com destino dummy).

> **Extensão (Fase 3):** o dump agora inclui o breakdown interno do pipeline:
> `[view]` por estágio de `ShadingView::render()` e `[deferred]` por sub-etapa de
> `DeferredLayer::render()`.

## 2. Breakdown do frame (regime estacionário, última janela de 60 frames)

### Câmera parada (estático)

| Etapa | ms |
|---|---|
| KX tag loop (2 loops fundidos no código, medidos separados) | 0.34 + 0.17 |
| Depsgraph eval (lazy) | **0.000** |
| DRW+EEVEE sync total | 2.43 |
| — sync init+begin (engines) | 0.70 |
| — objects+extract (foreach+extração) | 1.46 |
| — EEVEE `object_sync` (dentro do anterior) | 1.10 |
| — sync end | 0.27 |
| Draw (submit do pipeline EEVEE) | 0.52 |
| 2D filter + blit | 0.01 |
| **Total CPU medido no render** | **~3.5** |

### Câmera em movimento (benchmark injeta deslocamento da câmera por frame)

| Etapa | ms (run 1 / run 2) |
|---|---|
| KX tag loops | 0.46 / 0.46 |
| Depsgraph eval | 0.000 / 0.000 |
| DRW+EEVEE sync | 1.90 / 2.04 |
| — objects+extract | 1.45 / 1.58 |
| — EEVEE `object_sync` | 1.04 / 1.11 |
| Draw (`render_sample` main_view) | 2.86–3.11 |
| 2D filter + blit | 0.01 |

Note: com câmera parada o TAA converge (`sampling.finished_viewport()`) e o pipeline pesado
não roda — por isso draw 0.52 ms. Com movimento, `main_view.render()` re-renderiza a cada
frame: +2.9 ms de submit.

## 3. Contadores por frame

- `kx_objects 398` | `iter 401` | `populated 401` (3 duplis além dos objetos diretos)
- `samples 1` | `passes 1`
- **`samples_per_frame=1` → Objetivo 1.1 do plano é NO-OP** (incerteza #1 respondida)
- `[frustum] in 143 | out 258 (64.3% cullable)` móvel; `in 102 | out 299 (74.6%)` estático
- **`[lights] eevee_synced 0`** — as 18 luzes AREA da cena **não passam pelo sync do EEVEE**
  (não estão no DEG_OBJECT_ITER do depsgraph de jogo; ver §6)

## 4. Top-3 fatias (câmera móvel, caso do profile de referência)

1. **Submit do pipeline EEVEE** (`main_view.render()`): ~2.9 ms — shadows + prepass + deferred
   + volume + film re-submetidos todo frame.
2. **Sync por objeto** (EEVEE `object_sync` + extração): ~1.0–1.5 ms — 401 objetos, sem skip.
3. **Loops de tagging do KX_Scene** (`TagForTransformUpdate` + `TagForTransformUpdateEvaluated`):
   ~0.5 ms.

Depsgraph eval: **0.00 ms** — a avaliação lazy do fork está funcionando (baseline
`BGE_FORCE_DEPSGRAPH=1` disponível para A/B).

## 5. Respostas às incertezas do plano (§6 do PLANO)

1. **`samples_per_frame` efetivo = 1** → Objetivo 1.1 é no-op. Cada sample re-syncaria a cena
   inteira, mas com 1 sample o problema não existe nesta cena.
2. **CPU sync vs enfileiramento de extração**: `object_sync` (EEVEE por objeto) ≈ 1.0 ms do
   total de 1.5 ms de objects+extract — o skip por objeto (2a) teria potencial, PORÉM:
3. **`manager->begin_sync()` destrói tudo por frame** (RESPOSTA CRÍTICA): `gbuffer_ps_.init()`,
   `prepass_.init()`, `resource_len_ = 0` e os passes são reconstruídos a cada frame.
   **2a (skip-unchanged) e 2b (cache de passes) são inviáveis sem reescrever o draw manager** —
   exatamente o que o plano proíbe (§4). Não implementadas.
4. **TAA em jogo**: com câmera parada o sampling converge e o pipeline fica em
   `finished_viewport()` (draw 0.5 ms). Com movimento, re-renderiza todo frame (incerteza
   confirmada: acumulação quase não acontece em cena em movimento).
5. **Blit final**: 2D filter + blit = 0.01 ms — irrelevante. Otimização do blit direto (Fase 3)
   não pagaria.

## 6. Culling pré-sync (Fase 2c) — implementado, medido, resultado honesto

Implementado atrás de `BGE_GAME_FRUSTUM_CULL=1` em `draw_context.cc`:

- Teste AABB×6-planos (`planes_from_projmat`) no predicate `should_draw_object`.
- Margem de expansão do frustum = **2 × alcance máximo de influência** (pior caso
  caster-do-outro-lado-da-luz), com a fórmula exata do EEVEE
  (`sqrt(power / light_threshold)` para luzes sem custom attenuation, `att_dist` com,
  `cascade_max_dist` para sol do depsgraph).
- **World sun** (placeholder criado por `world.sync()` quando `sun_threshold > 0`): margem
  conservadora de 150 m (configurável `BGE_GAME_FRUSTUM_CULL_WORLD_MARGIN`).
- Light probes presentes → culling desativado (`-1`).
- Override manual: `BGE_GAME_FRUSTUM_CULL_EXPAND=<m>`.

Resultados (câmera móvel):

| Config | populated | sync | draw |
|---|---|---|---|
| OFF | 401 | 2.04 | 3.50 |
| ON, expand 300 m (seguro, world sun) | 401 | 1.99 | 3.41 |
| ON, expand 50 m (forçado, inseguro) | 401 | 1.96 | 3.40 |

- Screenshot A/B com expand seguro: **max delta = 1/255 (idêntico)**.
- **64–75% dos objetos estão fora do frustum, mas com margens de sombra seguras nada é
  culado nesta cena**: o ambiente urbano denso mantém todos os objetos a <50 m do frustum
  expandido. O potencial não se converte em ganho aqui.
- Conclusão: a sub-etapa 2c **não paga para a City Street**. A infraestrutura fica disponível
  (opt-in) para cenas esparsas/fechadas com luzes de alcance limitado e sem sol.

## 7. Correções de crashes encontrados no caminho (pré-requisito de qualquer benchmark)

1. **Crash no startup do blenderplayer** (`style_get_dpi()` ← `U.uistyles` vazio ←
   `asset_shelf::region_prefsizey` durante `WM_check`): as UI styles só eram inicializadas
   DEPOIS do primeiro `WM_check`. Fix: inicializar `ui::init()/reinit_font()` antes do primeiro
   `WM_check` em `GPG_ghost.cpp` (idempotente — o bloco original após `WM_check` foi removido).
2. **Crash no primeiro frame com shadow denoise ligado**: `DeferredLayer::end_sync` chamava
   `direct_radiance_txs_.ensure_layer_views()` numa `TextureFromPool` ainda não adquirida
   (só adquirida em `render()`). Fix: pular o denoise no primeiro frame
   (`denoise_synced_`, `gpu_texture() != nullptr`) — semanticamente correto (sem histórico).
3. **Crash no exit com `python_main` configurado**: `MEM_delete(pythonCode.data())` liberava
   o buffer interno de um `std::string` com o allocador do Blender. Fix: cópia em
   `KX_GetPythonCode` com `MEM_delete` do buffer temporário; `LA_Launcher` não libera mais.

## 8. Outras mudanças de código (Fase 1)

- `KX_Scene::RenderAfterCameraSetup`: hoist das flags `GAME_USE_INTERACTIVE_DYNAPAINT/RIGIDBODY`
  para fora do loop de tagging (micro; `TagBlenderPhysicsObject` só é chamado quando alguma
  das opções está ligada — nas cenas comuns custa zero).
- Fusão completa dos dois loops de tagging **não aplicada**: o depsgraph lazy do fork torna a
  segunda passada quase gratuita quando nada está sujo (tag_eval 0.17 ms inclui o trabalho
  real de cópia de matrizes para `ob_eval`), e a fusão condicional complicaria a ordem
  de passes multi-câmera sem ganho mensurável.

## 9. Reprodução

```powershell
# gerar os .blend de benchmark (scripts com auto-exit + pacing)
blender.exe -b "samples\City Street.blend" --python <gen_bench.py> -- static   # ou moving

# benchmark com profile
$env:BGE_RENDER_PROFILE = "K:\Blitz-Engine\blitz-engine\profile.log"
blenderplayer.exe <bench.blend>

# A/B culling
$env:BGE_GAME_FRUSTUM_CULL = '1'          # on; expand seguro calculado
$env:BGE_GAME_FRUSTUM_CULL_EXPAND = '50'  # override manual (m)
$env:BGE_GAME_FRUSTUM_CULL_WORLD_MARGIN = '150'
```

## 10. Extensão Fase 3 — breakdown interno do pipeline (câmera móvel)

Com o `[view]` + `[deferred]` no dump, o frame móvel de ~3.1 ms de `main_view.render()`
decompõe em:

| Estágio | ms | natureza |
|---|---|---|
| `deferred` (97% do main_view) | 3.05 | soma dos submits CPU |
| — `setup` (prepass clear/submit) | 0.60 | CPU |
| — **`shadows` (hiz + `ShadowModule::render`)** | **1.29** | **GPU-drain** |
| — `gbuffer` (bind + submit gbuffer_ps) | 0.71 | CPU |
| — `eval_light` (acquire + eval_light + subsurface) | 0.35 | CPU |
| — `denoise` | 0.00 | off |
| — `combine` | 0.01 | CPU |
| `acquire` / `lights/hiz` / `vol_pre` / `volume` / `ao` / `fwd+dbg` / `postfx+film` | <0.05 | — |

**Conclusão decisiva da Fase 3:** a maior fatia (shadows, 1.29 ms) é **GPU-drain**, não CPU.
`ShadowModule::render` roda um loop de passes com `GPU_flush()` — a CPU espera a GPU
processar sombras. A cena usa **`sun_threshold=10`** → **world sun com sombra global** (as 18
luzes AREA da cena **não** chegam ao EEVEE; a iluminação é céu + sol de shader). Sombras de sol
envolvem **todo o ambiente** (não há culling por frustum), por isso o culling pré-sync (2c)
não ajuda nesta cena.

Como o caso móvel já é **GPU-bound** (shadows + gbuffer GPU), reduzir CPU de sync não melhora o
fps. O headroom real está em:
1. **Pipelining / pacing (Fase 4)** — sobrepor CPU do frame N+1 com GPU do frame N; hoje o
   frame é serial `[sync CPU] → [submit GPU] → [drain/shadows]`.
2. Reduzir qualidade de sombra (fora do escopo — trade-off de qualidade, não de código).

Fase 3 (HiZ condicional / volume / motion blur no-op): volume 0.0 ms, probes 0.004 ms, blit
0.01 ms, postfx 0.01 ms — **já são no-op ou insignificantes** nesta cena; nada a otimizar.

## 11. Extensão Fase 4 — render-ahead (experimental, `BGE_RENDER_AHEAD=1`)

O dump agora também imprime `[latency] GPU_latency + fps` (a categoria `tc_latency` do profiling
engine = espera no `SwapBuffers`).

**4a implementado** em `KX_KetsjiEngine`: o `SwapBuffers()` bloqueante (v-sync/GPU-wait) é
transferido de `EndFrame()` para o início do próximo `BeginFrame()` (`SwapBuffersNow`), com
pendente resolvido no destrutor (apresenta o último frame). Overlaps CPU do frame N+1 com GPU
do frame N. Opt-in via env var (risco: +1 frame de input latency, tearing em compositor).

Resultado A/B (câmera móvel, 600 frames, pacing 20 ms — que *minimiza* o ganho porque impõe
um gap CPU↔GPU):

| | GPU_latency | fps |
|---|---|---|
| OFF | 2.23 ms | 34.3 |
| ON | **0.42 ms** | **36.7** |

GPU_latency **-81%**, fps **+7%** mesmo com pacing artificial; no loop C++ real (CPU-bound,
sem o gap de 20 ms) o overlappable é maior. Screenshot A/B OFF vs ON: **max delta 4/255**
(equivalente, sem tearing/corrupção).

**Conclusão Fase 4:** o render-ahead é o único ganho estrutural confirmado no cenário GPU-bound,
e já está implementado atrás de flag. 4b (thread de sync) e 4c (batching Vulkan) ficam como
trabalho futuro de maior risco/infra.
