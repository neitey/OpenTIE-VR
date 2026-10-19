/* KTX2 (Basis Universal: UASTC or ETC1S, optionally Zstandard
 * supercompressed) to RGBA8, for the textures of the headset runtime's
 * controller models (third_party/basisu). */
#include "tie_remaster/vr/vr_ktx2_decode.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "basisu_transcoder.h"

extern "C" uint8_t* TieKtx2_DecodeRgba(const void* data, size_t size, int* width, int* height) {
	static std::once_flag s_init;
	std::call_once(s_init, [] { basist::basisu_transcoder_init(); });
	if (!data || size < 12 || size > 0xffffffffu || !width || !height) {
		return nullptr;
	}
	basist::ktx2_transcoder ktx2;
	if (!ktx2.init(data, (uint32_t)size) || !ktx2.start_transcoding()) {
		return nullptr;
	}
	const uint32_t w = ktx2.get_width(), h = ktx2.get_height();
	if (w == 0 || h == 0 || w > 4096 || h > 4096) {
		return nullptr;
	}
	uint8_t* rgba = static_cast<uint8_t*>(std::malloc((size_t)w * h * 4));
	if (!rgba) {
		return nullptr;
	}
	/* The top level, first layer and face, as RGBA32 (output in pixels). */
	if (!ktx2.transcode_image_level(0, 0, 0, rgba, w * h, basist::transcoder_texture_format::cTFRGBA32)) {
		std::free(rgba);
		return nullptr;
	}
	*width = (int)w;
	*height = (int)h;
	return rgba;
}
