# Plano de Performance GPU, Estabilidade e Escalabilidade — EEVEE/UPBGE

> **Status:** proposta técnica; nenhuma fase deste documento foi implementada ou medida.
> **Continuidade:** sucede `PLANO_PERFORMANCE_RASTERIZACAO.md` e
> `PLANO_PERFORMANCE_CAMERA_MOVEL.md`. As mudanças locais B1, B2, C1 e o denoiser de
> sombras permanecem preservadas e aguardam validação interativa; este plano não autoriza
> commit, revert nem reescrita dessas mudanças.
> **Escopo:** jogo com EEVEE/Vulkan, sem alterar o comportamento da viewport DCC fora de
> `DRW_game_render_loop_is_active()`.

## 1. Ponto de partida e correção do diagnóstico

A captura atual (`areatest_tmp/fase_a/city_street_profiler_atual.png`) mostra aproximadamente:

| Indicador | Valor |
|---|---:|
| Frametime | 10,97 ms / 91,2 fps |
| Rasterizer | 8,91 ms / 81% |
| GPU Latency | 1,64 ms / 14% |
| Depsgraph | 0,00 ms |
| Screen Trace / Fast GI | resolução 1:2, denoise temporal + espacial + bilateral |

O valor é alto para a meta de desempenho, mas **Rasterizer não é uma medição pura de
rasterização nem de execução GPU**. Hoje ele inclui CPU, driver, aquisição e eventual
back-pressure. Os timers `[view]` e `[deferred]` usam `std::chrono`; o próprio código os
classifica como custo de submissão CPU (`DRW_render.hh:569-594`,
`eevee_pipeline.cc:1085-1305`). Além disso, `[deferred]` está contido em `[view]` e os dois
não podem ser somados como buckets exclusivos.

Também não se deve mais interpretar `GPU Latency` como duração da GPU: ele envolve
`SwapBuffers()`, enquanto esperas de fence e `vkAcquireNextImageKHR()` podem aparecer dentro
de Rasterizer (`GHOST_ContextVK.cc:967-1059`, `KX_KetsjiEngine.cpp:292-374`).

### Fatos já estabelecidos que não serão reabertos como hipóteses

- Depsgraph e lógica não são o gargalo da City Street.
- `samples_per_frame=1`; blit/2D filters são irrelevantes na baseline anterior.
- Render-ahead já existe e está default ON nas mudanças locais; não será proposto novamente.
- O cache incremental de passes foi considerado inviável sem reescrever o draw manager.
- Culling CPU pré-sync não pagou na City Street; isso não elimina sua utilidade em outras cenas,
  mas o retira da prioridade deste caso.
- O jogo já usa `PassMain`, instancing, comandos indiretos e frustum culling GPU.
- Já existem LOD discreto KX com histerese, tile classification/compaction de raytracing,
  reutilização temporal e cache/paginação de sombras. Nenhum será apresentado como recurso novo.

### Nova incógnita principal

Ainda não existe uma decomposição confiável do custo GPU entre sombras, GBuffer, screen trace,
Fast GI, denoisers, volumes e postFX. Portanto, **nenhuma otimização de shader deve ser
priorizada apenas pelo número de instruções aparente**. A ordem final será decidida por
timestamps GPU e capturas de uma ferramenta de fabricante.

## 2. Objetivos e critérios globais

As metas abaixo são objetivos de produto, não previsões de ganho.

### 2.1 City Street, mesma qualidade e mesmo trajeto

Depois de validar a baseline interativa:

- reduzir o p50 de Rasterizer e do GPU total em pelo menos **30%**, sem apenas deslocar a espera
  para outro bucket — uma baseline de 8,91 ms implicaria aproximadamente 6,24 ms;
- atingir p95 de frametime **≤ 10 ms**, sem vsync e sem pacing artificial;
- meta ambiciosa: Rasterizer p50 **≤ 5 ms** e frametime p95 **≤ 8,33 ms**, se o hardware e a
  qualidade visual permitirem;
- p99 **≤ 16,67 ms** em regime estacionário e nenhum stall recorrente acima de 33 ms após
  warm-up.

Nenhuma dessas metas será declarada atingida com médias acumuladas ou com câmera que não tenha
sido comprovadamente propagada ao EEVEE.

### 2.2 Estabilidade e escalabilidade

- Soak de pelo menos 3.600 frames por configuração, sem crash, corrupção, crescimento
  monotônico de memória ou perda progressiva de fps.
- Cada otimização de escalabilidade deve demonstrar pelo menos **25% de melhoria no cenário de
  estresse para o qual foi projetada**, sem regressão superior a 5% na cena 1×.
- Reportar custo por 1.000 objetos, por milhão de triângulos, por draw/grupo e por shadow view;
  não prometer que 4× mais geometria terá o mesmo custo sem LOD/culling efetivo.
- Mudanças semanticamente neutras devem preservar a imagem em testes determinísticos; mudanças
  de qualidade exigem A/B visual explícito e aprovação, não apenas uma métrica numérica.

### 2.3 Regra de aceite de cada patch

Um patch só entra no caminho default quando:

1. tem baseline e A/B no mesmo binário, cena, resolução e trajeto;
2. melhora o passe alvo em pelo menos 10% ou o frame total em pelo menos 0,3 ms, salvo correções
   de erro/stall cuja justificativa seja outra;
3. não degrada p95/p99, estabilidade temporal ou sombras/reflexos além do critério aprovado;
4. mantém fallback de A/B durante o desenvolvimento e não altera viewport DCC indevidamente;
5. passa nos testes de regressão EEVEE existentes e nos cenários de borda aplicáveis.

## 3. Fase 0 — Harness confiável e baseline interativa

**Prioridade:** bloqueadora. **Esforço estimado:** 1–3 dias.

### 3.1 Fechar a limitação da câmera no player embutido

Não assumir que a causa já é conhecida. Instrumentar toda a cadeia, com o mesmo `frame_id`:

1. transform `KX_Camera`/SceneGraph e `DIRTY_RENDER`;
2. `ob_orig->object_to_world` após `TagForTransformUpdate()`;
3. `ob_eval->object_to_world` após `TagForTransformUpdateEvaluated()`;
4. `rv3d->viewmat` depois de `ED_view3d_update_viewmat()`;
5. `VelocityModule::camera_steps[PREVIOUS/CURRENT]` e `camera_has_motion()`;
6. identidade da câmera efetivamente renderizada, `UseViewportRender()` e modo embedded/player.

Controles obrigatórios:

- movimento Python e movimento por atuador nativo, separadamente;
- player standalone e embedded, sem misturar resultados entre os modos;
- verificação da ativação do controlador/atuador — o log `dbg_act.txt` atual mostra o tick
  executando sem deslocamento da câmera;
- correção dos scripts que consultam `KX_Scene.camera`: a API atual expõe `active_camera` e
  `cameras` (`KX_Scene.cpp:3319-3347,3476-3479`).

Separar também os contadores de TAA: hoje `n_taa_reset_camera` conta o **gatilho detectado**,
mesmo quando B1 suprime o reset (`eevee_instance.cc:423-430`). Registrar
`camera_motion_trigger`, `camera_reset_applied`, `depsgraph_reset_trigger` e
`depsgraph_reset_applied`.

**Aceite:** em movimento, KX, `ob_eval`, `rv3d` e Velocity mudam no mesmo frame; screenshots de
pontos distintos do trajeto são diferentes; o gatilho de câmera é 1,00 por frame e, com B1 ativo,
o reset efetivamente aplicado permanece 0. Parado, todos ficam estáveis. Se o embedded continuar
inválido, o player standalone vira a referência e a limitação permanece explicitamente aberta.

### 3.2 Trajeto determinístico e configuração reproduzível

Criar um controlador de benchmark em `areatest_tmp/perf_gpu/` que:

- execute trajetos gravados por frame de lógica: parado, translação, rotação, rotação+translação,
  desoclusão e teleporte;
- não use `time.sleep()` nem FPS acumulado;
- desligue vsync, fixe janela/resolução e informe overscan;
- use 120 frames de warm-up e pelo menos 600 frames medidos por repetição, três repetições;
- exporte JSONL por frame e um sumário com p50/p95/p99, máximo, desvio e amostras válidas;
- registre hash do commit + hash do diff local, GPU/driver, sistema, build, cena, resolução,
  flags `BGE_*` e todas as opções EEVEE relevantes.

Warm-up, compilação de shaders, resize e teleporte serão etiquetados e analisados separadamente.

### 3.3 Matriz mínima de cenas

| Cena | Pergunta que responde |
|---|---|
| City Street real | baseline e regressão principal |
| Reflexos/Fast GI intenso | custo de screen trace e denoisers |
| Sol + casters dinâmicos | tiles/páginas/views de sombra e popping |
| Mesma mesh repetida × meshes únicas | eficiência de instancing e fragmentação de grupos |
| Poucos high-poly × muitos low-poly | vertex/triangle-bound × submission/sync-bound |
| Muitos materiais/estados | custo de pipeline e batching |
| Objetos fora do frustum × visíveis porém ocluídos | frustum atual × necessidade real de occlusion |
| LOD com transições | steady-state, popping e hitches de cache |

## 4. Fase 1 — Eliminar esperas ocultas e falsos readbacks assíncronos

**Prioridade:** imediata após a instrumentação mínima. **Esforço:** 1–3 dias.

### 4.1 Corrigir `read_if_ready()` no Vulkan

`VKStorageBuffer::read_if_ready()` e `VKVertexBuffer::read_if_ready()` são documentados como
não bloqueantes, mas chamam `device.wait_for_timeline()` antes de ler
(`vk_storage_buffer.cc:188-251`, `vk_vertex_buffer.cc:49-103`).

Implementação necessária:

- consultar `VKDevice::submission_finished_timeline_get()` e só ler quando o valor concluído
  cobrir a timeline pendente (`vk_device.hh:426-442`);
- se ainda não estiver pronto, retornar `false` sem wait, sem novo flush e sem agendar outra cópia
  sobre o mesmo staging/mapped buffer;
- só limpar `read_if_ready_timeline_`/`read_if_ready_in_use_` após consumir um resultado válido;
- preservar invalidation de memória mapeada e o fallback dos demais backends;
- adicionar teste que mantenha a GPU ocupada, faça polls sucessivos e confirme dados corretos,
  ausência de wait e ausência de cópias concorrentes.

### 4.2 Remover o readback síncrono de estatísticas no jogo

`ShadowModule::init()` executa `statistics_buf_.current().read()` em viewport
(`eevee_shadow.cc:672-695`), e o read Vulkan espera a timeline (`vk_buffer.cc:198-207`).

- Adicionar `StorageCommon::read_if_ready()` ao wrapper DRW.
- No game render loop Vulkan, trocar o read síncrono por poll atrasado e manter a última estatística
  válida; até haver dado, não emitir warning baseado em valor obsoleto.
- Unificar o poll com o `async_flush_to_host()` já existente no render de sombras, evitando cópia e
  flush duplicados por frame.
- Preservar o comportamento síncrono de F12/light bake e oferecer
  `BGE_SHADOW_STATS_LEGACY_READ=1` durante o A/B.
- Não remover o read de `shadow_update_finished()` usado por image render/bake: viewport já sai
  após uma iteração (`eevee_shadow.cc:1224-1273`), portanto são caminhos diferentes.

**Aceite:** nenhum `wait_for_timeline()` no caminho denominado non-blocking; shadows e warnings
corretos; nenhum race/ASAN/Vulkan validation error; A/B de frametime e p95/p99 documentado. Se o
ganho for pequeno, a correção de contrato ainda permanece válida, mas sem promessa de speedup.

## 5. Fase 2 — Telemetria GPU Vulkan assíncrona e contadores de trabalho

**Prioridade:** bloqueadora para otimizações maiores. **Esforço:** 4–8 dias.

### 5.1 Componentes propostos

| Componente proposto | Responsabilidade | Local provável |
|---|---|---|
| `GPUTimestampScope` | API genérica opt-in, no-op quando desligada | `gpu/intern/gpu_timestamp.*` |
| `VKWriteTimestampNode` | gravar timestamp na ordem final do render graph | `gpu/vulkan/render_graph/` |
| `VKGpuTimestampRing` | query pools, slots pendentes e poll sem wait | `gpu/vulkan/vk_timestamp.*` |
| `DRWGameGpuProfile` | escopos EEVEE, identidade e metadados do frame | `DRW_render.hh`, `draw_context.cc` |
| `BGEProfileJsonlWriter` | amostras raw, sumários e config fingerprint | gameengine/launcher ou ferramenta local |

Nomes são propostos, não classes existentes.

### 5.2 Requisitos de implementação

- Duas queries por escopo, `timestampPeriod` e `timestampValidBits` validados.
- Ring fixo de slots com `frame_id`, `camera_pass` e `sample`; consultar com
  `VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`, **sem**
  `VK_QUERY_RESULT_WAIT_BIT`.
- Ring cheio: descartar profiling do novo frame e incrementar `gpu_dropped`; nunca bloquear.
- Não resetar/reutilizar query pool pendente e não resetar por flush de render graph.
- O timestamp node precisa respeitar a ordem final do scheduler; testar explicitamente que o
  scheduler não o move para o lado errado de dispatches/rendering scopes.
- Resultado publicado contra o `frame_id` de origem, não contra o frame que fez o poll.
- GPU total vem de um escopo total ou first→last; nunca da soma de escopos aninhados.
- `BGE_RENDER_PROFILE` continua produzindo o texto atual; `BGE_GPU_PROFILE=<arquivo.jsonl>`
  ativa a nova telemetria. Nenhum `GPU_finish()` no profiler.

### 5.3 Escopos GPU mínimos

- Frame total, prepass, HiZ, GBuffer, eval light, forward, volume, postFX/film e blit.
- Shadows: tagging/setup, caster update, página/tile update e raster de shadow views.
- Raytrace: classify/compact, generate, screen trace e spatial/temporal/bilateral por bin ativo.
- Fast GI: schedule, setup, scan, denoise e resolve.
- Shadow Denoising: temporal e cada iteração à-trous.

### 5.4 Buckets CPU separados

`frame_slot_fence_wait`, `swapchain_acquire`, `KX tag`, `DRW/EEVEE sync`, `EEVEE record`,
`render_graph build/record`, `enqueue`, `wait_for_submission`, `readback`, `present_submit` e
`SwapBuffers`. `DRW_submission_start/end()` não será rotulado como `vkQueueSubmit`: ele apenas
protege a região e chama `GPU_render_begin/end()`.

### 5.5 Contadores de trabalho

- draws/dispatches submetidos, grupos, protótipos, batches únicos, instâncias e mudanças de estado;
- bins de closure ativos, tiles classificados/compactados e dispatches indiretos vazios;
- páginas/tiles/views de sombra solicitados, selecionados, renderizados e descartados;
- triângulos estimados por primitive/index count e shadow view, sem tratar draw indireto como
  visibilidade final;
- trocas de LOD e hitches de derived cache.

Contadores pós-culling devem ser agregados GPU-side ou lidos com atraso; `submit_debug()` não
será usado por frame em produção.

**Aceite:** overhead do profiler ligado ≤5% no p50, zero wait introduzido, drops/incomplete
registrados, ranking de passes coerente com Nsight/RGP/RenderDoc e relatório parado×móvel que
explique pelo menos 80% do delta medido.

## 6. Fase 3 — Quick wins de baixo risco após o profile

**Esforço:** 2–5 dias, selecionados pelos dados da Fase 2.

### 3.1 Usar o número real de bins de closure

Há um TODO fixando três bins (`eevee_raytrace.cc:516-517,574-576`). Derivar a contagem de
`to_gbuffer_bin_count(active_closures)` e dimensionar texturas/recursos corretamente.

- Continuar chamando `trace(i, false, ...)` para bins inativos, pois esse caminho libera históricos
  e fornece recursos dummy (`eevee_raytrace.cc:634-649`).
- Medir clears, compactações e submissões eliminadas; não prometer redução equivalente de marcha,
  pois tiles vazios já são compactados.
- A/B com um, dois e três bins, refraction e históricos persistentes.

### 3.2 Presets antes de novos algoritmos

Criar uma tabela de presets por cena que apenas combine knobs existentes: Screen Trace 1:2/1:4,
Fast GI rays/steps/resolution, Bilateral ON/OFF, shadow pool/rays/steps/resolution e B2 ON/OFF.
Alterar um eixo por vez na caracterização.

**Atenção:** desligar Fast GI pode deslocar trabalho para screen tracing via
`trace_max_roughness`; o custo total, não o custo isolado do passe, decide.

### 3.3 Controles independentes, somente onde o profile indicar cauda

- `screen_trace_max_steps`, default compatível 255, testando 64/128;
- `shadow_denoise_atrous_iterations`, default 4; ao encurtar, o último passe precisa assumir
  `atrous_final`;
- presets SPFD de direções/sub-radii, pois `Max Penumbra` limita raio mas não reduz o atual caminho
  de até 240 taps;
- especialização Fast GI AO-only somente para cenas que realmente usam AO-only; a City Street da
  captura usa Global Illumination e não justifica priorizar esse ramo.

Esses controles finais devem ser RNA por cena, não uma coleção permanente de variáveis de ambiente.

## 7. Fase 4 — Otimizações dirigidas pelo passe dominante

**Esforço:** 1–3 semanas por ramo efetivamente justificado. Não executar todos por padrão.

### 4A. Sombras e evolução de B2

B2 atual apenas congela `grid_offset` em sincronizações ímpares; não há fila, aging, orçamento nem
classificação de casters (`eevee_shadow.cc:27-75`). Sua ativação default depende do A/B interativo.

Evolução incremental:

1. usar `frame_id` apresentado/renderizado, não paridade global de `begin_sync()`;
2. manter cobertura lógica do clipmap atualizada; orçar render de páginas, não aumentar o atraso do
   grid indiscriminadamente;
3. separar motivos `COVERAGE_NEW`, `DYNAMIC` e `LOD_QUALITY` em metadata sidecar;
4. distinguir **dirty** de **elegível neste frame**, com filas por prioridade e aging/max_age;
5. orçar páginas e views separadamente, com override crítico para teleporte/invalidação em massa;
6. reutilizar stale somente quando a cobertura for compatível; tile recém-inserida não pode ler
   depth de outra região;
7. prefetch na direção do movimento e fallback coarse válido quando necessário.

Criar `ShadowUpdateBudget` dentro do `ShadowModule`, sem renderer paralelo. A infraestrutura atual
de páginas/cache será aproveitada; não será duplicada.

**Aceite:** redução de pelo menos 25% no p95 do custo de sombras do cenário móvel, sem tile sem
cobertura, caster dinâmico starving, popping recorrente ou fallback inválido. Qualidade integral,
budget estrito e zero popping não são garantidos simultaneamente em teleportes; o catch-up crítico
é obrigatório.

### 4B. Raytracing, Fast GI e denoisers

Se forem dominantes nos timestamps:

- reduzir custo fixo por bins antes de alterar o algoritmo de marcha;
- ajustar steps/taps com presets e validar contact reflection, misses, off-screen fallback,
  leaking, halos, shimmer e ghosting;
- compactar o Shadow Denoising customizado por tiles somente com halo correto para strides 1/2/4/8
  e preservação do histórico;
- não mascarar Fast GI setup apenas pela tile receptora sem considerar o footprint das regiões
  fontes;
- não propor cache cross-frame de HiZ antes de modelar jitter, câmera, refraction e objetos
  dinâmicos.

**Aceite:** ganho no GPU total, não apenas deslocamento de custo para outro passe; qualidade
aprovada em sequências de 300 frames e nos testes de desoclusão/refraction.

### 4C. Geometria, LOD e instancing

Se GBuffer/shadow raster/vertex work ou submissão de geometria forem dominantes:

1. **LOD discreto KX já existente:** gerar níveis offline com Decimate/Subsurf estático, configurar
   distância + histerese e preservar materiais/layouts compatíveis. Nunca executar Decimate por
   frame. Estender `KX_LodManager` apenas com telemetria e depuração; não criar outro sistema LOD.
2. **Instancing já existente:** medir razão objetos→protótipos→grupos→draws. Corrigir primeiro
   evaluated meshes/batches divergentes, custom vertex ranges e estados incompatíveis. Para
   repetição grande, avaliar duplis/collection instances/Geometry Nodes que preservem batches.
3. **Ferramenta de authoring:** script para gerar LODs, validar bounds, materiais, light/shadow
   flags e estimar triângulos por nível; cache de transição deve ser aquecido no benchmark.
4. **Sombras incluídas:** contar triângulos de câmera e de cada shadow view; reduzir só a câmera
   não caracteriza o ganho total.

**Aceite:** melhoria ≥25% no estresse-alvo, sem regressão >5% em 1×, sem popping/hitches
recorrentes e sem perda de batching que anule o ganho de triângulos.

### 4D. CPU de submissão/Vulkan

Somente se record CPU estiver alto com GPU ociosa:

- reduzir submissões/estados/flushes identificados pelo trace;
- investigar agregação multi-draw depois de resolver Batch/VAO por grupo — trocar `count=1` por
  `count>1` não é suficiente;
- não aumentar profundidade de render-ahead para esconder GPU saturada, pois isso adiciona
  latência e não reduz trabalho.

## 8. Fase 5 — Qualidade adaptativa e orçamento de frame

**Prioridade:** somente após otimizações estáticas. **Esforço:** 3–7 dias.

Criar presets `Performance`, `Balanced` e `Quality` centralizados por cena. Um futuro
`BGERenderQualityController` poderá consumir GPU total e frametime em EMA/janela, com:

- target 60/120 fps, margem explícita e histerese;
- cooldown e passos discretos, sem oscilar resolução a cada frame;
- limites mínimos/máximos para raytrace, Fast GI, sombras e LOD;
- tratamento especial de resize, teleporte, loading e compilação de shaders;
- contabilização do custo de reset temporal/realocação ao trocar resolução;
- opt-out completo e override manual por cena.

Dynamic resolution é o último recurso, não a primeira solução: mudanças frequentes podem destruir
históricos e introduzir realocações, apagando o benefício.

**Aceite:** p95 dentro do orçamento escolhido, sem oscilação visível, sem aumento de input lag e
sem resets contínuos de TAA.

## 9. Fase 6 — Trabalho avançado, condicionado a evidência

Não faz parte da primeira entrega:

- **Occlusion GPU em duas fases:** somente se houver muito trabalho em objetos frustum-visíveis
  mas ocluídos após LOD/instancing. Exige HiZ histórico explícito, late pass para desoclusão e
  política conservadora para transparentes/dinâmicos. `HiZBuffer::back` atual não é histórico de
  frame pronto para esse uso.
- **hLOD/meshlets/mesh shaders:** ausentes no caminho atual e fora de uma otimização incremental.
- **Reescrita do draw manager, depsgraph ou renderer:** fora do escopo.
- **Threading da lógica:** sem justificativa, pois lógica/física são pequenas na baseline.

## 10. Validação obrigatória

### Performance

- Três repetições, 120 frames de warm-up e 600+ frames medidos por configuração.
- p50/p95/p99 por frame e por passe; nunca apenas média de 60 frames.
- Parado, translação, rotação, desoclusão, teleporte e soak 3.600 frames.
- Profiler OFF×ON para medir overhead; render-ahead ON como default e OFF apenas no A/B.

### Imagem e estabilidade

- B1 legado×novo: ghosting, volume, reflexos, penumbra e highlights.
- B2/orçamento: bordas de clipmap, casters dinâmicos, LOD, teleporte, pressão do pool e múltiplas
  views/câmeras.
- Raytrace/Fast GI: contato, off-screen fallback, roughness, refraction, leaking e disoclusão.
- Regressão `docs/test_shadow_denoise_ab.py`, F12 EEVEE e viewport DCC.
- Vulkan validation e testes de unidade/integração dos readbacks e query pools.

### Relatórios

Cada fase concluída deve produzir uma tabela antes/depois, config fingerprint, dados brutos e
decisão `adotar`, `manter opt-in` ou `rejeitar`. Resultados ficarão em
`docs/RESULTADOS_PERFORMANCE_GPU.md`; artefatos locais em `areatest_tmp/perf_gpu/`.

## 11. Sequência de execução recomendada

| Ordem | Entrega | Gate para avançar |
|---:|---|---|
| 0 | Validar interativamente B1/B2/C1 pendentes | baseline atual aprovada e comportamento sem regressão |
| 1 | Harness de câmera + trajeto + JSONL CPU | matrizes e imagens mudam de verdade; runs reproduzíveis |
| 2 | Correção dos readbacks e estatísticas de sombra | zero wait no non-blocking; A/B documentado |
| 3 | Timestamps GPU + waits separados + contadores | mapa de passes confiável e overhead ≤5% |
| 4 | Closure bins + presets/knobs de baixo risco | primeiros ganhos sem alterar arquitetura |
| 5 | Ramo dominante: sombras ou raytrace/denoise | meta City Street e qualidade visual |
| 6 | LOD/instancing conforme estresse de geometria | ganho no cenário-alvo e estabilidade de transições |
| 7 | Qualidade adaptativa | p95 estável sem oscilação/reset contínuo |
| 8 | Occlusion/multi-draw avançado, se ainda necessário | evidência de gargalo remanescente |

A primeira entrega recomendada é o bloco **1+2+3**: benchmark confiável, eliminação das esperas
ocultas e mapa GPU real. Ele reduz risco imediatamente e impede que as fases caras sejam guiadas
por um bucket CPU ambíguo.
