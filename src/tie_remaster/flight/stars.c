/* Direction-space renderer for TIE's original small-pixel starfield. */

#include "tie_remaster/flight/stars.h"

#include "tie_remaster/flight/renderer_internal.h"
#include "tie_remaster/gpu_debug.h"
#include "tie_remaster/scene2d/srgb_math.h"

#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/scene3d.h"
#include "tie_remaster/flight/render_math.h"

#include <math.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct TieFlightStarInstance {
	float axis_and_value[4];
} TieFlightStarInstance;

typedef struct TieFlightStarsVertexUniforms {
	float view_proj[16];
	float pixel_to_clip[2];
	float half_size_px[2];
	uint32_t style;
	float _pad_style[3];
	float colors[4][4];
} TieFlightStarsVertexUniforms;

#define TIE_FLIGHT_TIE98_MAX_STARS 3072

_Static_assert(sizeof(TieFlightStarInstance) == 16, "star instance must match StructuredBuffer<float4>");
_Static_assert(sizeof(TieFlightStarsVertexUniforms) == 160,
			   "star uniforms must match the HLSL cbuffer layout");

struct TieFlightStars {
	AeronShader* vs;
	AeronShader* ps;
	AeronGraphicsPipeline* pipeline;
	AeronSampleCount pipeline_samples;
	AeronTextureFormat rt_format;
	AeronBuffer* instances;
	TieFlightStarInstance cached[TIE_FLIGHT_TIE98_MAX_STARS];
	TieFlightStarInstance staging[TIE_FLIGHT_TIE98_MAX_STARS];
	uint8_t tie98_position_index[TIE_FLIGHT_TIE98_MAX_STARS];
	float tie98_intensity[TIE_FLIGHT_TIE98_MAX_STARS];
	uint16_t cached_count;
	uint16_t pending_count;
	TieFlightStarfieldStyle cached_style;
	TieFlightStarfieldStyle pending_style;
	bool cache_valid;
	AeronTexture* dot; /* VR star sprites (TieFlightStars_SubmitSprites) */
};

typedef struct TieFlightTie98StarRng {
	uint16_t seed;
	uint16_t value;
} TieFlightTie98StarRng;

/* Private copy of TIE's 16-bit LFSR. Star generation must not advance the
 * simulation RNG because this style can also be selected with the TIE95 engine. */
static uint16_t TieFlightStars_FlightTie98StarRandom(TieFlightTie98StarRng* rng) {
	uint16_t value = rng->value;
	for (int bit = 0; bit < 16; ++bit) {
		const uint16_t xor_bits = (rng->seed >> 8) ^ (uint16_t)(2u * (rng->seed & 0xFFu));
		const uint16_t carry = (xor_bits & 0x80u) != 0;
		value = (uint16_t)((value << 1) | (rng->seed >> 15));
		rng->seed = (uint16_t)(rng->seed * 2u + carry);
	}
	rng->value = value;
	return value;
}

static void TieFlightStars_InitTie98Table(TieFlightStars* stars) {
	TieFlightTie98StarRng rng = { .seed = 0x2357u, .value = 0 };
	for (int i = 0; i < TIE_FLIGHT_TIE98_MAX_STARS; ++i) {
		const uint8_t brightness = (uint8_t)((TieFlightStars_FlightTie98StarRandom(&rng) & 0xFu) + 8u);
		stars->tie98_intensity[i] = TieScene2dSrgb_ToLinear((float)brightness / 31.0f);
	}
	for (int i = 0; i < TIE_FLIGHT_TIE98_MAX_STARS; ++i) {
		uint8_t index;
		do {
			index = (uint8_t)(TieFlightStars_FlightTie98StarRandom(&rng) & 0x7Fu);
		} while (index > 124u);
		stars->tie98_position_index[i] = index;
	}
}

static int TieFlightStars_Tie95GridSize(uint16_t star_count) {
	for (int grid_size = 1; grid_size <= 16; ++grid_size) {
		if (3 * grid_size * grid_size == star_count)
			return grid_size;
	}
	return 0;
}

static uint16_t TieFlightStars_StageTie98(TieFlightStars* stars, uint16_t tie95_star_count) {
	const int tie95_grid_size = TieFlightStars_Tie95GridSize(tie95_star_count);
	if (tie95_grid_size == 0)
		return 0;
	const int detail_step = (16 + tie95_grid_size - 1) / tie95_grid_size;
	const int grid_size = 32 / detail_step;
	const float grid_step = 64.0f / (float)grid_size;
	static const int inner_axis[3] = { 0, 0, 1 };
	static const int outer_axis[3] = { 1, 2, 2 };
	uint16_t star_index = 0;

	for (int lobe = 0; lobe < 3; ++lobe) {
		for (int row = 0; row < grid_size; ++row) {
			for (int column = 0; column < grid_size; ++column, ++star_index) {
				const int position_index = stars->tie98_position_index[star_index];
				float axis[3] = {
					(float)(-32 + position_index / 25 - 2),
					(float)(-32 + (position_index % 25) / 5 - 2),
					(float)(-32 + position_index % 5 - 2),
				};
				axis[inner_axis[lobe]] += grid_step * (float)column;
				axis[outer_axis[lobe]] += grid_step * (float)row;
				TieFlightStarInstance* dest = &stars->staging[star_index];
				dest->axis_and_value[0] = axis[0];
				dest->axis_and_value[1] = axis[1];
				dest->axis_and_value[2] = axis[2];
				dest->axis_and_value[3] = stars->tie98_intensity[star_index];
			}
		}
	}
	return star_index;
}

static bool TieFlightStars_EnsurePipeline(TieFlightStars* stars, AeronSampleCount sample_count) {
	if (stars->pipeline && stars->pipeline_samples == sample_count)
		return true;
	if (stars->pipeline) {
		Aeron_DestroyGraphicsPipeline(stars->pipeline);
		stars->pipeline = NULL;
	}

	/* Analytic coverage is premultiplied before PMA-over blending. A
	 * fully covered center therefore retains palette[252..255] exactly. */
	AeronColorTargetStateDesc color_target = {
			.format = stars->rt_format,
			.blend = {
					.enabled = 1,
					.src_color = AERON_BLEND_ONE,
					.dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
					.color_op = AERON_BLEND_OP_ADD,
					.src_alpha = AERON_BLEND_ONE,
					.dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
					.alpha_op = AERON_BLEND_OP_ADD,
			},
	};
	stars->pipeline = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc){
			.vertex_shader = stars->vs,
			.fragment_shader = stars->ps,
			.primitive_type = AERON_PRIMITIVE_TRIANGLES,
			.cull_mode = AERON_CULL_NONE,
			.depth_format = AERON_TEXTURE_FORMAT_D32_FLOAT,
			.depth = {
					.depth_test = 1,
					.depth_write = 0,
					.compare = AERON_COMPARE_GREATER_EQUAL,
			},
			.color_target_count = 1,
			.color_targets = &color_target,
			.sample_count = sample_count,
	});
	stars->pipeline_samples = stars->pipeline ? sample_count : 0;
	return stars->pipeline != NULL;
}

TieFlightStars* TieFlightStars_Create(AeronTextureFormat rt_format) {
	TieFlightStars* stars = calloc(1, sizeof *stars);
	if (!stars)
		return NULL;
	stars->rt_format = rt_format;
	stars->vs = TieFlightRenderer_CompileShader("flight_stars.vert", AERON_SHADER_STAGE_VERTEX, 0, 1, 1);
	stars->ps = TieFlightRenderer_CompileShader("flight_stars.frag", AERON_SHADER_STAGE_FRAGMENT, 0, 0, 0);
	stars->instances = Aeron_CreateBuffer(&(AeronBufferDesc) {
		.size = sizeof stars->cached,
		.usage = AERON_BUFFER_USAGE_STORAGE,
		.memory_usage = AERON_MEMORY_USAGE_GPU_ONLY,
		.debug_name = "tie.flight.stars.instances",
	});
	if (!stars->vs || !stars->ps || !stars->instances) {
		Aeron_LogError("tie.flight", "starfield GPU resource creation failed");
		TieFlightStars_Destroy(stars);
		return NULL;
	}
	TieFlightStars_InitTie98Table(stars);
	return stars;
}

void TieFlightStars_Destroy(TieFlightStars* stars) {
	if (!stars)
		return;
	if (stars->pipeline)
		Aeron_DestroyGraphicsPipeline(stars->pipeline);
	if (stars->vs)
		Aeron_DestroyShader(stars->vs);
	if (stars->ps)
		Aeron_DestroyShader(stars->ps);
	if (stars->instances)
		Aeron_DestroyBuffer(stars->instances);
	if (stars->dot)
		Aeron_DestroyTexture(stars->dot);
	free(stars);
}

bool TieFlightStars_Prepare(TieFlightStars* stars, AeronCommandBuffer* cmd, const TieSnapshot* snapshot,
							TieFlightStarfieldStyle style) {
	if (style == TIE_FLIGHT_STARFIELD_STYLE_AUTO && snapshot) {
		style = snapshot->legacy_render_convention == TIE_FLIGHT_LEGACY_RENDER_TIE95
					? TIE_FLIGHT_STARFIELD_STYLE_TIE95
					: TIE_FLIGHT_STARFIELD_STYLE_TIE98;
	}
	if (!stars || !cmd || !snapshot || snapshot->star_count == 0 || snapshot->star_count > TIE_MAX_STARS ||
		(style != TIE_FLIGHT_STARFIELD_STYLE_TIE95 && style != TIE_FLIGHT_STARFIELD_STYLE_TIE98)) {
		if (stars)
			stars->pending_count = 0;
		return false;
	}

	uint16_t count = snapshot->star_count;
	if (style == TIE_FLIGHT_STARFIELD_STYLE_TIE98) {
		count = TieFlightStars_StageTie98(stars, snapshot->star_count);
		if (count == 0) {
			stars->pending_count = 0;
			return false;
		}
	} else {
		for (uint16_t i = 0; i < count; ++i) {
			const TieStarDirection* source = &snapshot->stars[i];
			TieFlightStarInstance* dest = &stars->staging[i];
			dest->axis_and_value[0] = source->axis[0];
			dest->axis_and_value[1] = source->axis[1];
			dest->axis_and_value[2] = source->axis[2];
			dest->axis_and_value[3] = (float)source->palette_slot;
		}
	}

	const uint32_t bytes = count * (uint32_t)sizeof stars->staging[0];
	const bool changed = !stars->cache_valid || stars->cached_count != count ||
						 stars->cached_style != style || memcmp(stars->cached, stars->staging, bytes) != 0;
	if (changed) {
		if (!Aeron_UploadBufferDataCmd(cmd, stars->instances, 0, stars->staging, bytes)) {
			stars->pending_count = 0;
			return false;
		}
		memcpy(stars->cached, stars->staging, bytes);
		stars->cached_count = count;
		stars->cached_style = style;
		stars->cache_valid = true;
	}
	stars->pending_count = count;
	stars->pending_style = style;
	return true;
}

void TieFlightStars_DrawInPass(TieFlightStars* stars, AeronCommandBuffer* cmd, AeronRenderPass* pass,
							   const TieSnapshot* snapshot, const TieFlightCamera* camera,
							   const float view_proj[16]) {
	if (!stars || !cmd || !pass || !snapshot || !camera || !view_proj || stars->pending_count == 0 ||
		snapshot->classic_w == 0 || snapshot->classic_h == 0 || camera->rt_w <= 0 || camera->rt_h <= 0)
		return;
	if (!TieFlightStars_EnsurePipeline(stars, Aeron_RenderPassGetSampleCount(pass))) {
		Aeron_CommandBufferSetFailure(cmd, "Starfield pipeline preparation failed");
		return;
	}

	TieFlightStarsVertexUniforms uniforms = { 0 };
	memcpy(uniforms.view_proj, view_proj, sizeof uniforms.view_proj);
	uniforms.pixel_to_clip[0] = 2.0f / (float)camera->rt_w;
	uniforms.pixel_to_clip[1] = 2.0f / (float)camera->rt_h;
	float reference_width = (float)snapshot->classic_w;
	float reference_height = (float)snapshot->classic_h;
	if (stars->pending_style == TIE_FLIGHT_STARFIELD_STYLE_TIE98) {
		reference_width = 640.0f;
		reference_height = 480.0f;
	}
	uniforms.half_size_px[0] = 0.5f * camera->fit_w / reference_width;
	uniforms.half_size_px[1] = 0.5f * camera->fit_h / reference_height;
	uniforms.style = stars->pending_style == TIE_FLIGHT_STARFIELD_STYLE_TIE98 ? 1u : 0u;
	for (int slot = 0; slot < 4; ++slot) {
		TieScene2dSrgb_PalToLinearRgb(snapshot->palette[252 + slot], &uniforms.colors[slot][0],
									  &uniforms.colors[slot][1], &uniforms.colors[slot][2]);
		uniforms.colors[slot][3] = 1.0f;
	}

	TIE_GPU_PUSH(cmd,
				 stars->pending_style == TIE_FLIGHT_STARFIELD_STYLE_TIE98 ? "TIE98 stars" : "TIE95 stars");
	Aeron_BindGraphicsPipeline(pass, stars->pipeline);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, stars->instances);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &uniforms, sizeof uniforms);
	Aeron_DrawInstanced(pass, 6, stars->pending_count, 0);
	TIE_GPU_POP(cmd);
}

bool TieFlightStars_PrepareCustom(TieFlightStars* stars, AeronCommandBuffer* cmd, const float (*dirs)[4],
								  uint16_t count) {
	if (!stars || !cmd || !dirs || count == 0)
		return false;
	if (count > TIE_FLIGHT_TIE98_MAX_STARS)
		count = TIE_FLIGHT_TIE98_MAX_STARS;
	for (uint16_t i = 0; i < count; ++i)
		memcpy(stars->staging[i].axis_and_value, dirs[i], sizeof stars->staging[i].axis_and_value);
	const uint32_t bytes = count * (uint32_t)sizeof stars->staging[0];
	if (!stars->cache_valid || stars->cached_count != count || memcmp(stars->cached, stars->staging, bytes) != 0) {
		if (!Aeron_UploadBufferDataCmd(cmd, stars->instances, 0, stars->staging, bytes)) {
			stars->pending_count = 0;
			return false;
		}
		memcpy(stars->cached, stars->staging, bytes);
		stars->cached_count = count;
		stars->cached_style = TIE_FLIGHT_STARFIELD_STYLE_TIE98;
		stars->cache_valid = true;
	}
	stars->pending_count = count;
	stars->pending_style = TIE_FLIGHT_STARFIELD_STYLE_TIE98;
	return true;
}

void TieFlightStars_DrawCustom(TieFlightStars* stars, AeronCommandBuffer* cmd, AeronRenderPass* pass,
							   const float view_proj[16], int rt_w, int rt_h, float half_size_px) {
	if (!stars || !cmd || !pass || !view_proj || stars->pending_count == 0 || rt_w <= 0 || rt_h <= 0)
		return;
	if (!TieFlightStars_EnsurePipeline(stars, Aeron_RenderPassGetSampleCount(pass))) {
		Aeron_CommandBufferSetFailure(cmd, "Starfield pipeline preparation failed");
		return;
	}
	TieFlightStarsVertexUniforms uniforms = { 0 };
	memcpy(uniforms.view_proj, view_proj, sizeof uniforms.view_proj);
	uniforms.pixel_to_clip[0] = 2.0f / (float)rt_w;
	uniforms.pixel_to_clip[1] = 2.0f / (float)rt_h;
	uniforms.half_size_px[0] = half_size_px;
	uniforms.half_size_px[1] = half_size_px;
	uniforms.style = 1u; /* colour = intensity (w) */
	TIE_GPU_PUSH(cmd, "VR environment stars");
	Aeron_BindGraphicsPipeline(pass, stars->pipeline);
	Aeron_BindStorageBuffer(pass, AERON_SHADER_STAGE_VERTEX, 0, stars->instances);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &uniforms, sizeof uniforms);
	Aeron_DrawInstanced(pass, 6, stars->pending_count, 0);
	TIE_GPU_POP(cmd);
}

/* A round dot, coverage in rgb and alpha (premultiplied white), linear. */
static AeronTexture* TieFlightStars_DotTexture(TieFlightStars* stars, AeronCommandBuffer* cmd) {
	if (stars->dot)
		return stars->dot;
	enum { N = 16 };
	uint8_t px[N * N * 4];
	for (int j = 0; j < N; ++j) {
		for (int i = 0; i < N; ++i) {
			const float x = ((float)i + 0.5f) / (float)N * 2.0f - 1.0f;
			const float y = ((float)j + 0.5f) / (float)N * 2.0f - 1.0f;
			const float r = sqrtf(x * x + y * y);
			float c = r < 0.75f ? 1.0f : r < 1.0f ? (1.0f - r) / 0.25f : 0.0f;
			const uint8_t v = (uint8_t)(c * 255.0f + 0.5f);
			uint8_t* p = &px[(j * N + i) * 4];
			p[0] = p[1] = p[2] = p[3] = v;
		}
	}
	AeronTexture* tex = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = N,
		.height = N,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "tie.flight.star_dot",
	});
	if (tex && !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
													.texture = tex,
													.width = N,
													.height = N,
													.raw_data = px,
													.raw_size = (uint32_t)sizeof px,
												})) {
		Aeron_DestroyTexture(tex);
		tex = NULL;
	}
	stars->dot = tex;
	return tex;
}

int TieFlightStars_SubmitSprites(TieFlightStars* stars, AeronScene3D* scene, AeronCommandBuffer* cmd,
								 const TieSnapshot* snapshot, const float camera_ori[4], float half_tan,
								 int max_sprites) {
	if (!stars || !scene || !cmd || !snapshot || !camera_ori || stars->pending_count == 0 || max_sprites <= 0)
		return 0;
	AeronTexture* dot = TieFlightStars_DotTexture(stars, cmd);
	if (!dot)
		return 0;
	float colors[4][3];
	for (int slot = 0; slot < 4; ++slot)
		TieScene2dSrgb_PalToLinearRgb(snapshot->palette[252 + slot], &colors[slot][0], &colors[slot][1],
									  &colors[slot][2]);
	float m[9];
	TieRenderMath_QuaternionToMat3(camera_ori, m);
	const float right[3] = { m[0], m[1], m[2] };
	const float up[3] = { m[3], m[4], m[5] };
	const float fwd[3] = { m[6], m[7], m[8] };
	const bool tie98 = stars->pending_style == TIE_FLIGHT_STARFIELD_STYLE_TIE98;
	const float far_dist = 65536.0f;
	static const float sgx[4] = { -1.0f, -1.0f, +1.0f, +1.0f };
	static const float sgy[4] = { +1.0f, -1.0f, -1.0f, +1.0f };
	static const float cuv[4][2] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };
	int submitted = 0;
	for (uint16_t i = 0; i < stars->pending_count && submitted < max_sprites; ++i) {
		const float* a = stars->cached[i].axis_and_value;
		float n[3] = { a[0], a[1], a[2] };
		const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (len < 1e-6f)
			continue;
		float facing = (n[0] * fwd[0] + n[1] * fwd[1] + n[2] * fwd[2]) / len;
		/* The classic renderer shows a direction behind the eye at its
		 * antipode (the shader negates negative-depth clip positions). */
		const float sign = facing < 0.0f ? -1.0f : 1.0f;
		facing *= sign;
		if (facing < 0.17f) /* outside both eyes' view (~80 deg off axis) */
			continue;
		for (int k = 0; k < 3; ++k)
			n[k] *= sign / len;
		float rgb[3];
		if (tie98) {
			rgb[0] = rgb[1] = rgb[2] = a[3];
		} else {
			const int slot = (int)(a[3] + 0.5f) & 3;
			memcpy(rgb, colors[slot], sizeof rgb);
		}
		AeronSceneBillboardDesc d;
		memset(&d, 0, sizeof d);
		d.texture = dot;
		d.blend = AERON_SCENE_BILLBOARD_BLEND_ADDITIVE;
		d.stage = AERON_SCENE_BILLBOARD_STAGE_SKY;
		for (int c = 0; c < 4; ++c) {
			for (int k = 0; k < 3; ++k)
				d.corners[c][k] = far_dist * (n[k] + (right[k] * sgx[c] + up[k] * sgy[c]) * half_tan);
			d.uv[c][0] = cuv[c][0];
			d.uv[c][1] = cuv[c][1];
			d.colors[c][0] = rgb[0];
			d.colors[c][1] = rgb[1];
			d.colors[c][2] = rgb[2];
			d.colors[c][3] = 1.0f;
		}
		AeronScene_AddBillboard(scene, &d);
		++submitted;
	}
	return submitted;
}
