# Resultados — Performance GPU, Estabilidade e Escalabilidade (EEVEE/UPBGE)

> Resultados por fase do `PLANO_PERFORMANCE_GPU_ESCALABILIDADE.md`.
> Artefatos locais (JSONL brutos, traces, perfis, screenshots, fingerprints) ficam em
> `areatest_tmp/perf_gpu/out/`.

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
