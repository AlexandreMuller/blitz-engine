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
 * - temporal_main: Reprojects the previous frame accumulation using per-pixel velocity
 *   (object + camera motion) and the reconstructed scene depth. History taps are validated
 *   individually (bounds + stored depth), which provides disocclusion detection. The reprojected
 *   history is clamped to the current frame 3x3 neighborhood to avoid ghosting, then blended
 *   with the new noisy radiance. The accumulated radiance and the current view depth are stored
 *   in a persistent ping-pong history texture array (one layer per closure bin).
 *
 * - bilateral_main: Final separated bilateral filter (3x3) over the temporally accumulated
 *   radiance. Samples across geometric discontinuities are rejected using the surface normal
 *   (gbuffer) and the reconstructed depth. The result is encoded back into the direct radiance
 *   buffers that the combine pass reads.
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
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_shared_exponent_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

namespace eevee::shadow::denoise {

#define SHADOW_DENOISE_GROUP_SIZE 8

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

  [[image(0, write, SFLOAT_16_16_16_16)]] image2DArray out_history_img;

  [[resource_table]] srt_t<CameraVelocity> camera;

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
      }
      else {
        float3 in_radiance = colorspace::safe_color(srt.load_radiance_direct(texel, bin));

        /* Current frame 3x3 neighborhood statistics for history clamping (anti-ghosting). */
        float3 clamp_min = in_radiance;
        float3 clamp_max = in_radiance;
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
          }
        }
        /* Slight expansion to avoid clipping valid reprojected history. */
        float3 pad = (clamp_max - clamp_min) * 0.25f + 1e-4f;
        clamp_min -= pad;
        clamp_max += pad;

        /* Gather and validate the 4 bilinear history taps (disocclusion detection). */
        float4 history = srt.history_tap(base_texel + int2(0, 1), bin, bilinear_weights.x, scene_z);
        history += srt.history_tap(base_texel + int2(1, 1), bin, bilinear_weights.y, scene_z);
        history += srt.history_tap(base_texel + int2(1, 0), bin, bilinear_weights.z, scene_z);
        history += srt.history_tap(base_texel + int2(0, 0), bin, bilinear_weights.w, scene_z);

        float3 out_radiance = in_radiance;
        if (history.w > 1e-3f) {
          float3 history_radiance = clamp(history.rgb / history.w, clamp_min, clamp_max);
          /* Accumulate over ~10 frames. Reduce history weight on fast motion to limit smearing. */
          float blend = mix(0.9f, 0.25f, saturate(velocity_px * 0.02f));
          out_radiance = mix(in_radiance, history_radiance, blend);
        }

        /* Feedback for the next frame. Alpha stores the view depth for disocclusion checks. */
        imageStore(srt.out_history_img, out_texel, float4(out_radiance, scene_z));
      }
    }
  }
}

struct DenoiseBilateral {
  [[specialization_constant(1)]] int closure_count;

  [[sampler(0)]] sampler2DArray history_tx;
  [[sampler(1)]] sampler2DDepth depth_tx;

  [[image(0, read_write, DEFERRED_RADIANCE_FORMAT)]] uimage2D direct_radiance_1_img;
  [[image(1, read_write, DEFERRED_RADIANCE_FORMAT)]] uimage2D direct_radiance_2_img;
  [[image(2, read_write, DEFERRED_RADIANCE_FORMAT)]] uimage2D direct_radiance_3_img;

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
 * Final separated bilateral filter over the temporally accumulated radiance.
 *
 * Dispatched at full resolution. Runs once for all closure bins (specialized by closure_count).
 * Writes the denoised radiance back into the direct radiance buffers read by the combine pass.
 */
[[compute, local_size(SHADOW_DENOISE_GROUP_SIZE, SHADOW_DENOISE_GROUP_SIZE)]]
void bilateral_main([[resource_table]] DenoiseBilateral &srt,
                    [[resource_table]] const draw::View &views,
                    [[resource_table]] const gbuffer::Reader &reader,
                    [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (!in_image_range(texel, srt.direct_radiance_1_img)) {
    return;
  }

  const ViewMatrices view = views.get(0);
  float2 extent_inv = 1.0f / float2(textureSize(srt.depth_tx, 0).xy);

  for (int bin = 0; bin < 3; bin++) [[unroll]] {
    /* NOTE: Do not use [[static_branch]] here. See temporal_main. */
    if (bin < srt.closure_count) {
      ClosureUndetermined center_closure = reader.read_bin(texel, uchar(bin));
      if (center_closure.type != CLOSURE_NONE_ID) {
        float center_depth = reverse_z::read(texelFetch(srt.depth_tx, texel, 0).r);
        float2 center_uv = (float2(texel) + 0.5f) * extent_inv;
        float3 center_P = view.point_screen_to_world(float3(center_uv, center_depth));
        float3 center_N = center_closure.N;

        float3 in_radiance = texelFetch(srt.history_tx, int3(texel, bin), 0).rgb;

        /* 3x3 bilateral filter: gaussian spatial weight, planar depth weight and normal
         * weight reject samples across geometric discontinuities. */
        float gauss = filters::gaussian_factor(1.5f, 1.0f);
        float3 accum_radiance = colorspace::log_from_scene_linear(
            colorspace::safe_color(in_radiance));
        float weight_accum = 1.0f;

        for (int y = -1; y <= 1; y++) {
          for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) {
              continue;
            }
            int2 sample_texel = texel + int2(x, y);
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
            float2 sample_uv = (float2(sample_texel) + 0.5f) * extent_inv;
            float3 sample_P = view.point_screen_to_world(float3(sample_uv, sample_depth));

            float depth_weight = filters::planar_weight(center_N, center_P, sample_P, 10000.0f);
            float normal_weight = filters::angle_weight(center_N, sample_closure.N);
            float spatial_weight = filters::gaussian_weight(gauss,
                                                          length_squared(float2(x, y)));
            float weight = depth_weight * normal_weight * spatial_weight;

            float3 radiance = texelFetch(srt.history_tx, int3(sample_texel, bin), 0).rgb;
            accum_radiance += colorspace::log_from_scene_linear(colorspace::safe_color(radiance)) *
                              weight;
            weight_accum += weight;
          }
        }

        float3 out_radiance = colorspace::scene_linear_from_log(accum_radiance *
                                                                safe_rcp(weight_accum));
        srt.store_radiance_direct(texel, bin, out_radiance);
      }
    }
  }
}

PipelineCompute temporal(temporal_main);
PipelineCompute bilateral(bilateral_main);

}  // namespace eevee::shadow::denoise
