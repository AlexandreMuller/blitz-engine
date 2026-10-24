# Plano: denoising de sombras orientado a SVGF (EEVEE / Blitz-Engine)

Data: 2026-11 (v2 — reescrito para estrutura SVGF-ready)
Status: **Fase 1 implementada** (2026-10-17) — validação A/B headless passou
(cenário penumbra: −37.4% de energia de alta frequência no frame 1 espacial;
highlight especular: gradiente 1.004× e pico 1.000× vs. OFF). Fases 2 e 3 pendentes.

## 1. Sintomas relatados

1. **Desempenho anormal**: o denoiser consome GPU de forma desproporcional.
2. **Blur indevido**: superfícies de baixa rugosidade têm o highlight especular reduzido e
   levemente borrado, mesmo longe de qualquer sombra — o filtro é aplicado em todo o buffer,
   não só na penumbra onde existe ruído.

## 2. Diagnóstico (verificado no código)

O denoise hoje é aplicado em **três** estágios, todos full-screen e **sem nenhuma máscara de
ruído**:

| # | Local | O que faz | Custo |
|---|-------|-----------|-------|
| 1 | `eevee_shadow_denoise.bsl.hh` → `temporal_main` | Reprojeta e acumula histórico de **todos** os pixels/bins | Dispatch full-res, ~13 fetches por bin |
| 2 | `eevee_shadow_denoise.bsl.hh` → `bilateral_main` | Bilateral 3×3 sobre o radiance acumulado, em **todos** os pixels com closure | Dispatch full-res, 8 vizinhos × (depth + gbuffer + history) por bin |
| 3 | `eevee_deferred_combine.bsl.hh` → `load_radiance_direct_denoised` | **Outro** bilateral 3×3 no combine quando `use_shadow_denoise` | 8 vizinhos × (HiZ + gbuffer + radiance) por closure, em todo fragment |

Consequências:

- **Tripla filtragem** — o estágio 3 deveria ter sido removido na rewrite v2, mas está no tree
  (provável perda durante merge). Principal culpado do blur de highlights.
- **Buffer errado para filtrar sem máscara**: `direct_radiance` carrega luz direta *completa*
  (difusa + especular); um bilateral sem gate de ruído é um low-pass sobre bordas de highlight.
- **Custo fixo full-screen**: os passes rodam na tela inteira mesmo sem nenhuma sombra jittered.
- **Método limitado**: o 3×3 bilateral não resolve ruído de baixa frequência de penumbras
  largas (granulação — pendência antiga) e não adapta raio ao ruído.

## 3. Arquitetura alvo: SVGF-lite

Padrão da indústria para 1 spp em tempo real (Schied et al., 2017). O pipeline final terá
**dois** passes (em vez dos três atuais), ambos guiados por variância:

```
light eval → [temporal: reproject + accumulate radiance + MOMENTOS (m1, m2)] →
             [à-trous: 4 iterações, strides 1/2/4/8, peso ∝ exp(-|Δlum| / (σ·k + ε))] →
             combine (sem filtro — lê o radiance já denoised)
```

Princípio central: **a variância acumulada é o gate natural** — onde σ ≈ 0 (luz plena, umbra,
highlight limpo) o filtro é inerte por construção; onde σ é alto (penumbra) ele filtra na
escala certa. Isso elimina o blur de especular *dentro* do próprio algoritmo, sem heurística
extra.

### Mudanças estruturais necessárias

- **Textura de histórico**: hoje `RGBA16F` (rgb = radiance, a = depth). SVGF precisa também de
  momentos → passar para **duas texturas ping-pong**:
  - `history_radiance`: RGB = radiance acumulado, A = view depth (como hoje);
  - `history_moments`: RG = momentos (m1, m2 da luminância), B = comprimento do histórico
    (history length), A livre/reservado.
  - Custo: +1 textura array RGBA16F por ping-pong. Alternativa mais barata: R11G11 + packed —
    avaliar na implementação; começar com RGBA16F pela simplicidade.
- **Pass espacial único**: `bilateral_main` vira `atrous_main` — 4 iterações separadas
  (sub-passes com barrier entre elas, ping-pong entre o buffer de radiance e um scratch),
  strides 1/2/4/8, pesos edge-stopping por profundidade planar + normal + **luminância guiada
  por σ** (SVGF: `w_lum = exp(-|lum_c - lum_s| / (k·σ + ε))`).
- **Combine**: volta a apenas ler `direct_radiance` (sem `use_shadow_denoise`, sem HiZ/filters
  se ficarem dead).

## 4. Fases

### Fase 1 — SVGF-lite funcional (qualidade + estrutura, sem máscara ainda)

1. **Remover o bilateral do combine** (`load_radiance_direct_denoised`, chamada em
   `combine_frag`, specialization `use_shadow_denoise`). Auditar includes/usos de `filters::`
   e `HiZ` no combine após a remoção.
2. **Temporal com momentos** (`temporal_main`):
   - acumular m1/m2 da luminância junto com a radiância (mesmo blend adaptativo por
     `history length`: `blend = min(1 - 1/len, 0.9)`, cap 64 frames — design v2);
   - **fallback espacial 3×3 de variância enquanto len < 4** (sem isso σ=0 no frame 1 e o
     filtro fica inerte no boot);
   - rejeição de histórico por profundidade/bounds já existe — manter.
3. **`atrous_main` substituindo `bilateral_main`**:
   - 4 iterações, strides 1/2/4/8 (footprint ~30 px — resolve a granulação de penumbra larga);
   - kernel 3×3 por iteração (9 taps × 4 = 36 taps/pixel, mas **um único estágio** — ainda
     mais barato que os ~27 taps × overheads dos 3 estágios atuais);
   - peso de luminância exponencial sobre |Δ| (o gaussiano sobre Δ² rejeitava os dois clusters
     do ruído binário — lição da v2);
   - edge-stopping por profundidade planar + normal (já existe, reusar);
   - escreve o resultado final de volta nos `direct_radiance` buffers (como o bilateral faz
     hoje) — o combine não muda de interface.
4. **Early-out por variância no à-trous**: se σ do pixel < ε (região limpa), escrever o valor
   original sem iterar. Primeira camada de proteção do especular antes da máscara da Fase 2.

**Critério de saída da Fase 1**: highlight nítido igual ao baseline OFF; penumbra com ruído
visivelmente reduzido (meta: ≥ 50% de redução de variância em stills, superior aos 16.6% da
v1); custo do `t_df_denoise` ≤ custo atual.

### Fase 2 — Máscara de penumbra (o ganho real de desempenho)

5. **Máscara de "sombra jittered" na origem**: no light/shadow eval (`eevee_shadow_tracing`),
   marcar pixels cuja sombra usou amostragem jittered (raio > 0). Spike inicial: localizar o
   ponto exato da decisão (`light.jitter`, `max_kernel_radius`). Destino recomendado: buffer
   R8 `shadow_noise_mask_tx`; alternativa: bit livre em stencil/gbuffer.
6. **Consumo da máscara**: `temporal_main` → pixels limpos custam 1 fetch + 1 store (pula
   reprojeção); `atrous_main` → `mask==0` faz skip do store (original permanece). Vizinhos
   mascarados continuam legíveis como taps — o gate é só sobre o pixel central.
7. **Compactação por tile (opcional)**: reduzir a máscara para tiles 8×8/16×16 (atomicOr) e
   dispatch só dos tiles ativos no à-trous. Cena sem sombras → custo ≈ 0.

### Fase 3 — Qualidade da penumbra

8. **Footprint adaptativo ao tamanho da penumbra**: escalar strides/filtro pelo `kernel_radius`
   ou pela σ acumulada (o à-trous já dá a base multi-escala; a adaptação escolhe quantas
   iterações aplicar por região).
9. **Transição suave umbra→penumbra→luz** estilo Cycles (banding entre níveis discretos de
   cinza) — avaliar após Fases 1–2, pois a tripla filtragem atual mascara o comportamento real.

## 5. Validação

- **A/B headless**: reusar `test_shadow_denoise_ab.py` (128–256 px, 1 shadow sample, OFF/ON →
  PNGs). **Adicionar cena nova**: plano roughness ~0.05 + luz pontual → métrica de nitidez do
  highlight (gradiente médio na região do highlight) além da variância da penumbra. Métricas
  via numpy (scipy indisponível no runtime).
- **Perfil GPU**: timer `profile.t_df_denoise` já existe (`eevee_pipeline.cc`) — comparar ms
  antes/depois, em cena com e sem sombras.
- **Viewport/game**: ganho temporal real (ghosting, acumulação) só com câmera/objeto em
  movimento no binário — headless tem seed fixa (limitação conhecida).
- Indicar o path completo do `blender.exe` recém-linkado ao usuário para o teste interativo.

## 6. Armadilhas conhecidas (deste projeto)

- **Não usar `[[static_branch]]`** dentro de loops `[[unroll]]` — o shader_tool come o corpo.
- **`sample` é token reservado** no toolchain de shaders.
- Iteração rápida: buildar só `eevee_cpp_shaders_bsl.vcxproj` (erros em segundos); build full
  só depois de 0 erros de shader.
- Scripts Python de UI não são recopiados em build incremental (copiar para
  `bin\Release\5.3\scripts\...` ou rodar INSTALL).
- `unset GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0 GIT_CONFIG_KEY_1 GIT_CONFIG_VALUE_1`
  antes de qualquer build no Git Bash.
- Ping-pong de texturas: cuidar o `shadow_history_index_` swap e a invalidação (alpha 0) ao
  recriar — agora para **duas** texturas.

## 7. Ordem de execução

1. Fase 1 (itens 1–4) — 1–2 sessões, validação A/B + métricas de highlight e variância.
2. Fase 2 (itens 5–7) — spike da máscara no shadow tracing, depois integração.
3. Fase 3 (itens 8–9) — depois de medir o comportamento real pós-Fase 1/2.
