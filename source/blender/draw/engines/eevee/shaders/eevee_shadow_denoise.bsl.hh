/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shadow denoising pipeline (UPBGE).
 *
 * Denoises the shadow sampling noise present in the deferred direct radiance buffers.
 * The pipeline is independent from the TAA/film accumulation and runs right after the deferred
 * light evaluation, before the combine pass:
 *
 * SVGF-lite pipeline (Schied et al., 2017), two passes:
 *
 * - temporal_main: Reprojects the previous frame accumulation using per-pixel velocity
 *   (object + camera motion) and the reconstructed scene depth. History taps are validated
 *   individually (bounds + stored depth), which provides disocclusion detection. The reprojected
 *   history is clamped to the current frame 3x3 neighborhood to avoid ghosting, then blended
 *   with the new noisy radiance using an adaptive factor driven by the accumulated history
 *   length. Alongside the radiance, the temporal pass accumulates the first and second
 *   luminance moments (m1, m2) and the history length: the accumulated variance is the natural
 *   noise gate for the spatial filter. While the history is still short (len < 4), the moments
 *   are augmented with a spatial 3x3 estimate from the current frame so the filter is active
 *   from the first frames. Two persistent ping-pong history texture arrays are kept (one layer
 *   per closure bin): radiance + view depth, and moments + history length.
 *
 * - atrous_main: Variance-guided a-trous filter. Four iterations with strides 1/2/4/8
 *   (footprint ~30px, enough for the low-frequency penumbra grain), each a 3x3 kernel with
 *   edge-stopping weights: planar depth + normal (geometric discontinuities) and an
 *   exponential luminance weight scaled by the local standard deviation
 *   (w = exp(-|lum_c - lum_s| / (sigma + eps))). Where the variance is ~0 (full light, umbra,
 *   clean highlights) the filter is inert by construction; where it is high (penumbra) it
 *   filters at the matching scale. Clean pixels (sigma below epsilon) keep their value without
 *   iterating. The iterations ping-pong through scratch buffers and the last one encodes the
 *   result back into the direct radiance buffers that the combine pass reads.
 *
 * The whole pipeline is only enabled when Scene.eevee.shadow_use_denoise is set and the SPFD
 * soft shadow path is disabled.
 */

#pragma once

#include "draw_view.bsl.hh"
#include "eevee_closure.bsl.hh"
#include "eevee_colorspace_lib.bsl.hh"
#include "eevee_defines.hh"
#include "eevee_filter.bsl.hh"
#include "eevee_gbuffer_read.bsl.hh"
#include "eevee_reverse_z_lib.bsl.hh"
#include "eevee_velocity.bsl.hh"
#include "gpu_shader_math_base.bsl.hh"
#include "gpu_shader_math_safe.bsl.hh"
#include "gpu_shader_shared_exponent.bsl.hh"
#include "gpu_shader_utildefines.bsl.hh"

namespace eevee::shadow::denoise {

#define SHADOW_DENOISE_GROUP_SIZE 8

/* Rec. 709 luma coefficients. */
float luminance(float3 c)
{
  return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

/* From top left in clockwise order. Same convention as the ray-tracing denoiser. */
float4 bilinear_weights_from_subpixel_coord(float2 co)
{
  float4 weights;
  weights.x = (1.0f - co.x) * co.y;
  weights.y = co.x * co.y;
  weights.z = co.x * (1.0f - co.y);
  weights.w = (1.0f - co.x) * (1.0f - co.y);
  return weights;
}

struct DenoiseTemporal {
  [[specialization_constant(1)]] int closure_count;

  [[sampler(0)]] usampler2D direct_radiance_1_tx;
  [[sampler(1)]] usampler2D direct_radiance_2_tx;
  [[sampler(2)]] usampler2D direct_radiance_3_tx;
  [[sampler(3)]] sampler2DDepth depth_tx;
  [[sampler(4)]] sampler2D vector_tx;
  [[sampler(5)]] sampler2DArray history_tx;
  /* Moments history: R = m1 (luminance), G = m2 (luminance squared), B = history length.
   * A history length of 0 tags invalidated history (never written or no surface data). */
  [[sampler(6)]] sampler2DArray history_moments_tx;

  [[image(0, write, SFLOAT_16_16_16_16)]] image2DArray out_history_img;
  [[image(1, write, SFLOAT_16_16_16_16)]] image2DArray out_history_moments_img;

  [[resource_table]] CameraVelocity camera;

  float3 load_radiance_direct(int2 texel, int bin) const
  {
    uint data = 0u;
    switch (bin) {
      case 0:
        data = texelFetch(direct_radiance_1_tx, texel, 0).r;
        break;
      case 1:
        data = texelFetch(direct_radiance_2_tx, texel, 0).r;
        break;
      case 2:
        data = texelFetch(direct_radiance_3_tx, texel, 0).r;
        break;
      default:
        break;
    }
    return rgb9e5_decode(data);
  }

  /**
   * Sample one history tap. Returns accumulated radiance in rgb and sample weight in a.
   * A tap is rejected (weight 0) if it is out of bounds, was invalidated, or fails the depth
   * consistency check (disocclusion detection).
   */
  float4 history_tap(int2 tap_texel, int bin, float tap_weight, float scene_z) const
  {
    if (tap_weight <= 0.0f) {
      return float4(0.0f);
    }
    if (!in_texture_range(tap_texel, history_tx)) {
      return float4(0.0f);
    }
    float4 tap = texelFetch(history_tx, int3(tap_texel, bin), 0);
    /* Alpha == 0 tags invalidated history (never written or no surface data). */
    if (tap.a == 0.0f) {
      return float4(0.0f);
    }
    /* Depth consistency check: reject taps belonging to a different surface (disocclusion). */
    float depth_threshold = max(0.05f * abs(scene_z), 1e-3f);
    if (abs(tap.a - scene_z) > depth_threshold) {
      return float4(0.0f);
    }
    return float4(tap.rgb * tap_weight, tap_weight);
  }

  /**
   * Sample one history moments tap. Returns (m1, m2, len) and (0, 0, 0) if out of bounds or
   * invalidated. Consistency (depth) validation is applied by the caller through the weight
   * of the matching radiance history tap.
   */
  float4 history_moments_tap(int2 tap_texel, int bin) const
  {
    if (!in_texture_range(tap_texel, history_moments_tx)) {
      return float4(0.0f);
    }
    float4 tap = texelFetch(history_moments_tx, int3(tap_texel, bin), 0);
    if (tap.b == 0.0f) {
      return float4(0.0f);
    }
    return tap;
  }
};

/**
 * Temporal reprojection and accumulation of the direct (shadowed) radiance.
 *
 * Dispatched at full resolution. Runs once for all closure bins (specialized by closure_count).
 *
 * Input: Noisy direct radiance, depth, velocity, persistent history.
 * Output: New history (accumulated radiance + view depth).
 */
[[compute, local_size(SHADOW_DENOISE_GROUP_SIZE, SHADOW_DENOISE_GROUP_SIZE)]]
void temporal_main([[resource_table]] DenoiseTemporal &srt,
                   [[resource_table]] const draw::View &views,
                   [[resource_table]] const gbuffer::Reader &reader,
                   [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (!in_image_range(texel, srt.out_history_img)) {
    return;
  }

  const ViewMatrices view = views.get(0);
  [[resource_table]] const CameraVelocity &cam_vel = srt.camera;

  float2 extent = float2(imageSize(srt.out_history_img).xy);
  float2 uv = (float2(texel) + 0.5f) / extent;

  float depth = reverse_z::read(texelFetch(srt.depth_tx, texel, 0).r);
  float scene_z = view.point_screen_to_view(float3(uv, depth)).z;

  /* Reprojection using per-pixel velocity (object + camera motion). UV space motion. */
  float4 motion = cam_vel.resolve(views, srt.vector_tx, texel, depth);
  float2 history_co = (float2(texel) + 0.5f) + motion.xy * extent;
  float velocity_px = length(motion.xy * extent);

  /* Bilinear history sampling setup. */
  float2 history_texel = history_co - 0.5f;
  float2 round_co = floor(history_texel);
  float2 fract_co = history_texel - round_co;
  float4 bilinear_weights = bilinear_weights_from_subpixel_coord(fract_co);
  int2 base_texel = int2(round_co);

  for (int bin = 0; bin < 3; bin++) [[unroll]] {
    /* NOTE: Do not use [[static_branch]] here. The unroll + static_branch combination is
     * mishandled by the shader translator and eats the loop body. The condition is a
     * specialization constant, so the GLSL compiler folds it statically anyway. */
    if (bin < srt.closure_count) {
      int3 out_texel = int3(texel, bin);

      ClosureUndetermined closure = reader.read_bin(texel, uchar(bin));
      if (closure.type == CLOSURE_NONE_ID) {
        /* No surface data for this bin: invalidate the history so stale data is never reused. */
        imageStore(srt.out_history_img, out_texel, float4(0.0f));
        imageStore(srt.out_history_moments_img, out_texel, float4(0.0f));
      }
      else {
        float3 in_radiance = colorspace::safe_color(srt.load_radiance_direct(texel, bin));

        /* Current frame 3x3 neighborhood statistics for history clamping (anti-ghosting).
         * The same samples seed a spatial luminance-moments estimate used while the temporal
         * history is still short. */
        float3 clamp_min = in_radiance;
        float3 clamp_max = in_radiance;
        float neighbor_m1 = luminance(in_radiance);
        float neighbor_m2 = neighbor_m1 * neighbor_m1;
        float neighbor_count = 1.0f;
        for (int y = -1; y <= 1; y++) {
          for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) {
              continue;
            }
            int2 sample_texel = texel + int2(x, y);
            if (!in_texture_range(sample_texel, srt.direct_radiance_1_tx)) {
              continue;
            }
            /* Reject neighbors without surface data for this bin. */
            if (reader.read_bin(sample_texel, uchar(bin)).type == CLOSURE_NONE_ID) {
              continue;
            }
            float3 radiance = srt.load_radiance_direct(sample_texel, bin);
            clamp_min = min(clamp_min, radiance);
            clamp_max = max(clamp_max, radiance);
            float radiance_lum = luminance(radiance);
            neighbor_m1 += radiance_lum;
            neighbor_m2 += radiance_lum * radiance_lum;
            neighbor_count += 1.0f;
          }
        }
        /* Slight expansion to avoid clipping valid reprojected history. */
        float3 pad = (clamp_max - clamp_min) * 0.25f + 1e-4f;
        clamp_min -= pad;
        clamp_max += pad;

        /* Gather and validate the 4 bilinear history taps (disocclusion detection). */
        float4 h_01 = srt.history_tap(base_texel + int2(0, 1), bin, bilinear_weights.x, scene_z);
        float4 h_11 = srt.history_tap(base_texel + int2(1, 1), bin, bilinear_weights.y, scene_z);
        float4 h_10 = srt.history_tap(base_texel + int2(1, 0), bin, bilinear_weights.z, scene_z);
        float4 h_00 = srt.history_tap(base_texel + int2(0, 0), bin, bilinear_weights.w, scene_z);
        float4 history = h_01 + h_11 + h_10 + h_00;

        /* Moments taps, weighted by the matching radiance tap validation. */
        float4 moments = srt.history_moments_tap(base_texel + int2(0, 1), bin) * h_01.a;
        moments += srt.history_moments_tap(base_texel + int2(1, 1), bin) * h_11.a;
        moments += srt.history_moments_tap(base_texel + int2(1, 0), bin) * h_10.a;
        moments += srt.history_moments_tap(base_texel + int2(0, 0), bin) * h_00.a;

        float cur_lum = luminance(in_radiance);
        float cur_m1 = cur_lum;
        float cur_m2 = cur_lum * cur_lum;

        float len = 1.0f;
        float3 out_radiance = in_radiance;
        float out_m1 = cur_m1;
        float out_m2 = cur_m2;

        if (history.w > 1e-3f) {
          float3 history_radiance = clamp(history.rgb / history.w, clamp_min, clamp_max);
          /* Adaptive accumulation driven by the history length: the longer the validated
           * history, the higher its weight. Capped at 64 frames and reduced on fast motion
           * to limit smearing. */
          len = min(moments.b / history.w + 1.0f, 64.0f);
          float blend = min(1.0f - 1.0f / len, 0.9f);
          blend *= 1.0f - saturate(velocity_px * 0.02f);
          out_radiance = mix(in_radiance, history_radiance, blend);
          out_m1 = mix(cur_m1, moments.r / history.w, blend);
          out_m2 = mix(cur_m2, moments.g / history.w, blend);
        }

        /* While the temporal history is still short, augment the moments with the spatial
         * estimate so the variance (and thus the spatial filter) is meaningful right after
         * boot or disocclusion. */
        if (len < 4.0f) {
          float neighbor_count_inv = safe_rcp(neighbor_count);
          float spatial_weight = (4.0f - len) * 0.25f;
          out_m1 = mix(out_m1, neighbor_m1 * neighbor_count_inv, spatial_weight);
          out_m2 = mix(out_m2, neighbor_m2 * neighbor_count_inv, spatial_weight);
        }

        /* Feedback for the next frame. Alpha stores the view depth for disocclusion checks. */
        imageStore(srt.out_history_img, out_texel, float4(out_radiance, scene_z));
        /* Moments feedback: m1, m2 and the history length (0 would invalidate the tap). */
        imageStore(srt.out_history_moments_img, out_texel, float4(out_m1, out_m2, len, 0.0f));
      }
    }
  }
}

struct DenoiseAtrous {
  [[specialization_constant(1)]] int closure_count;
  /* Sampling stride of this iteration: 1, 2, 4 or 8 (footprint ~30px after 4 iterations). */
  [[specialization_constant(2)]] int atrous_stride;
  /* Last iteration: encode the result into the direct radiance buffers instead of the scratch. */
  [[specialization_constant(3)]] bool atrous_final;

  /* Input radiance. RGB = radiance, A = variance (only valid from the second iteration on;
   * on the first iteration the variance comes from the moments texture). */
  [[sampler(0)]] sampler2DArray in_radiance_tx;
  /* Moments history (first iteration only): R = m1, G = m2, B = history length. */
  [[sampler(1)]] sampler2DArray in_moments_tx;
  [[sampler(2)]] sampler2DDepth depth_tx;

  [[image(0, read_write, DEFERRED_RADIANCE_FORMAT)]] uimage2D direct_radiance_1_img;
  [[image(1, read_write, DEFERRED_RADIANCE_FORMAT)]] uimage2D direct_radiance_2_img;
  [[image(2, read_write, DEFERRED_RADIANCE_FORMAT)]] uimage2D direct_radiance_3_img;
  /* Scratch output for the non-final iterations (ping-ponged by the caller). */
  [[image(3, write, SFLOAT_16_16_16_16)]] image2DArray out_scratch_img;

  void store_radiance_direct(int2 texel, int bin, float3 radiance)
  {
    uint data = rgb9e5_encode(radiance);
    switch (bin) {
      case 0:
        imageStore(direct_radiance_1_img, texel, uint4(data));
        break;
      case 1:
        imageStore(direct_radiance_2_img, texel, uint4(data));
        break;
      case 2:
        imageStore(direct_radiance_3_img, texel, uint4(data));
        break;
      default:
        break;
    }
  }
};

/**
 * One iteration of the variance-guided a-trous filter over the accumulated radiance.
 *
 * Dispatched at full resolution, once per stride (1/2/4/8). Runs once for all closure bins
 * (specialized by closure_count). The final iteration writes the denoised radiance back into
 * the direct radiance buffers read by the combine pass; the others write radiance + filtered
 * variance into the scratch buffer for the next iteration.
 */
[[compute, local_size(SHADOW_DENOISE_GROUP_SIZE, SHADOW_DENOISE_GROUP_SIZE)]]
void atrous_main([[resource_table]] DenoiseAtrous &srt,
                 [[resource_table]] const draw::View &views,
                 [[resource_table]] const gbuffer::Reader &reader,
                 [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (!in_image_range(texel, srt.out_scratch_img)) {
    return;
  }

  const ViewMatrices view = views.get(0);
  float2 extent_inv = 1.0f / float2(textureSize(srt.depth_tx, 0).xy);

  for (int bin = 0; bin < 3; bin++) [[unroll]] {
    /* NOTE: Do not use [[static_branch]] here. See temporal_main. */
    if (bin < srt.closure_count) {
      ClosureUndetermined center_closure = reader.read_bin(texel, uchar(bin));
      /* NOTE: Do not use `continue` here: the unrolled loop body is straight-line code in
       * the generated GLSL and the translator rejects it. */
      if (center_closure.type != CLOSURE_NONE_ID) {
        int3 in_texel = int3(texel, bin);
        float4 center_data = texelFetch(srt.in_radiance_tx, in_texel, 0);
        float3 center_radiance = colorspace::safe_color(center_data.rgb);

        /* Variance of the accumulated signal. On the first iteration it is derived from the
         * luminance moments accumulated by the temporal pass (unbiased estimator); on later
         * iterations it is filtered along with the radiance (alpha channel). */
        float variance = 0.0f;
        if (srt.atrous_stride == 1) {
          float4 moments = texelFetch(srt.in_moments_tx, in_texel, 0);
          float len = max(moments.b, 1.0f);
          float unbiased = len / max(len - 1.0f, 1.0f);
          variance = max(moments.g - moments.r * moments.r, 0.0f) * unbiased;
        }
        else {
          variance = max(center_data.a, 0.0f);
        }

        float3 out_radiance = center_radiance;
        float out_variance = variance;

        /* Early-out: clean pixels (no accumulated variance) keep their value untouched. */
        if (variance > 1e-6f) {
          float center_depth = reverse_z::read(texelFetch(srt.depth_tx, texel, 0).r);
          float2 center_uv = (float2(texel) + 0.5f) * extent_inv;
          float3 center_P = view.point_screen_to_world(float3(center_uv, center_depth));
          float3 center_N = center_closure.N;
          float center_lum = luminance(center_radiance);
          float sigma = sqrt(variance);

          float3 accum_radiance = center_radiance;
          float accum_variance = variance;
          float weight_accum = 1.0f;

          for (int y = -1; y <= 1; y++) {
            for (int x = -1; x <= 1; x++) {
              if (x == 0 && y == 0) {
                continue;
              }
              /* Discretized 3x3 gaussian kernel: edges weight 0.2, corners 0.05, center 1. */
              float kernel_weight = (x * y == 0) ? 0.2f : 0.05f;
              int2 sample_texel = texel + int2(x, y) * srt.atrous_stride;
              if (!in_texture_range(sample_texel, srt.depth_tx)) {
                continue;
              }
              float sample_depth = reverse_z::read(texelFetch(srt.depth_tx, sample_texel, 0).r);
              if (sample_depth == 1.0f) {
                /* Background. */
                continue;
              }
              ClosureUndetermined sample_closure = reader.read_bin(sample_texel, uchar(bin));
              if (sample_closure.type == CLOSURE_NONE_ID) {
                continue;
              }
              int3 sample_layer_texel = int3(sample_texel, bin);
              float4 sample_data = texelFetch(srt.in_radiance_tx, sample_layer_texel, 0);
              float3 sample_radiance = colorspace::safe_color(sample_data.rgb);

              float2 sample_uv = (float2(sample_texel) + 0.5f) * extent_inv;
              float3 sample_P = view.point_screen_to_world(float3(sample_uv, sample_depth));

              float depth_weight = filters::planar_weight(center_N, center_P, sample_P, 10000.0f);
              float normal_weight = filters::angle_weight(center_N, sample_closure.N);
              /* Exponential luminance weight scaled by the local standard deviation (SVGF
               * phi factor k = 2): where the variance is ~0 the weight collapses and the
               * filter is inert, preserving clean highlights by construction; where the
               * variance is high, k > 1 lets the two clusters of binary shadow noise mix. */
              float sample_lum = luminance(sample_radiance);
              float lum_weight = exp(-abs(center_lum - sample_lum) / (sigma * 2.0f + 1e-4f));
              float weight = kernel_weight * depth_weight * normal_weight * lum_weight;

              float sample_variance = 0.0f;
              if (srt.atrous_stride == 1) {
                float4 sample_moments = texelFetch(srt.in_moments_tx, sample_layer_texel, 0);
                float sample_len = max(sample_moments.b, 1.0f);
                float sample_unbiased = sample_len / max(sample_len - 1.0f, 1.0f);
                sample_variance = max(sample_moments.g - sample_moments.r * sample_moments.r, 0.0f) *
                                  sample_unbiased;
              }
              else {
                sample_variance = max(sample_data.a, 0.0f);
              }

              accum_radiance += sample_radiance * weight;
              accum_variance += sample_variance * weight;
              weight_accum += weight;
            }
          }

          float weight_accum_inv = safe_rcp(weight_accum);
          out_radiance = accum_radiance * weight_accum_inv;
          out_variance = accum_variance * weight_accum_inv;
        }

        if (srt.atrous_final) {
          srt.store_radiance_direct(texel, bin, out_radiance);
        }
        else {
          imageStore(srt.out_scratch_img, in_texel, float4(out_radiance, out_variance));
        }
      }
    }
  }
}

PipelineCompute temporal(temporal_main);
PipelineCompute atrous(atrous_main);

}  // namespace eevee::shadow::denoise
