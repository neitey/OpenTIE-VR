/* Voice commands: the speech recognizer; see voice_core.h. Ported from
 * OpenXWA-VR (src/xwa_app/voice_core.c). */

#include "tie_app/quest/voice_core.h"

#include "third_party/sherpa-onnx/c-api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_stdinc.h>

#if defined(_WIN32)
#define XV_NO_DLOPEN 1
#else
#include <dlfcn.h>
#endif

/* ---- recognizer -------------------------------------------------------- */

/* A non-streaming model: push-to-talk hands it the whole utterance, which it
 * hears far better than a streaming model hears the start of a short
 * command. */
typedef const SherpaOnnxOfflineRecognizer* (*PfnCreateRecognizer)(const SherpaOnnxOfflineRecognizerConfig*);
typedef void (*PfnDestroyRecognizer)(const SherpaOnnxOfflineRecognizer*);
typedef const SherpaOnnxOfflineStream* (*PfnCreateStream)(const SherpaOnnxOfflineRecognizer*);
typedef void (*PfnDestroyStream)(const SherpaOnnxOfflineStream*);
typedef void (*PfnAcceptWaveform)(const SherpaOnnxOfflineStream*, int32_t, const float*, int32_t);
typedef void (*PfnDecode)(const SherpaOnnxOfflineRecognizer*, const SherpaOnnxOfflineStream*);
typedef const SherpaOnnxOfflineRecognizerResult* (*PfnGetResult)(const SherpaOnnxOfflineStream*);
typedef void (*PfnDestroyResult)(const SherpaOnnxOfflineRecognizerResult*);

static struct {
	void* lib;
	PfnCreateRecognizer create_recognizer;
	PfnDestroyRecognizer destroy_recognizer;
	PfnCreateStream create_stream;
	PfnDestroyStream destroy_stream;
	PfnAcceptWaveform accept;
	PfnDecode decode;
	PfnGetResult get_result;
	PfnDestroyResult destroy_result;
	const SherpaOnnxOfflineRecognizer* recognizer;
} s_xv;

int TieVoice_Ready(void) { return s_xv.recognizer != NULL; }

int TieVoice_Load(const char* lib, const char* model_dir, int threads, char* error, int error_size) {
	if (s_xv.recognizer) {
		return 1;
	}
#if defined(XV_NO_DLOPEN)
	(void)lib;
	(void)model_dir;
	(void)threads;
	snprintf(error, (size_t)error_size, "voice commands are not available on this platform");
	return 0;
#else
	if (!s_xv.lib) {
		s_xv.lib = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
		if (!s_xv.lib) {
			snprintf(error, (size_t)error_size, "cannot load %s: %s", lib, dlerror());
			return 0;
		}
	}
#define XV_SYM(field, name)                                                                                        \
	do {                                                                                                         \
		*(void**)(&s_xv.field) = dlsym(s_xv.lib, name);                                                          \
		if (!s_xv.field) {                                                                                       \
			snprintf(error, (size_t)error_size, "%s: missing %s", lib, name);                                    \
			return 0;                                                                                            \
		}                                                                                                        \
	} while (0)
	XV_SYM(create_recognizer, "SherpaOnnxCreateOfflineRecognizer");
	XV_SYM(destroy_recognizer, "SherpaOnnxDestroyOfflineRecognizer");
	XV_SYM(create_stream, "SherpaOnnxCreateOfflineStream");
	XV_SYM(destroy_stream, "SherpaOnnxDestroyOfflineStream");
	XV_SYM(accept, "SherpaOnnxAcceptWaveformOffline");
	XV_SYM(decode, "SherpaOnnxDecodeOfflineStream");
	XV_SYM(get_result, "SherpaOnnxGetOfflineStreamResult");
	XV_SYM(destroy_result, "SherpaOnnxDestroyOfflineRecognizerResult");
#undef XV_SYM

	char encoder[1024], decoder[1024], joiner[1024], tokens[1024];
	snprintf(encoder, sizeof encoder, "%s/encoder.onnx", model_dir);
	snprintf(decoder, sizeof decoder, "%s/decoder.onnx", model_dir);
	snprintf(joiner, sizeof joiner, "%s/joiner.onnx", model_dir);
	snprintf(tokens, sizeof tokens, "%s/tokens.txt", model_dir);
	SherpaOnnxOfflineRecognizerConfig config;
	memset(&config, 0, sizeof config);
	config.feat_config.sample_rate = 16000;
	config.feat_config.feature_dim = 80;
	config.model_config.transducer.encoder = encoder;
	config.model_config.transducer.decoder = decoder;
	config.model_config.transducer.joiner = joiner;
	config.model_config.tokens = tokens;
	config.model_config.num_threads = threads > 0 ? threads : 1;
	config.model_config.provider = "cpu";
	config.model_config.debug = SDL_getenv("XWA_VOICE_DEBUG") ? 1 : 0;
	config.decoding_method = "greedy_search";
	config.max_active_paths = 4;
	s_xv.recognizer = s_xv.create_recognizer(&config);
	if (!s_xv.recognizer) {
		snprintf(error, (size_t)error_size, "the speech model in %s did not load", model_dir);
		return 0;
	}
	return 1;
#endif
}

void TieVoice_Recognize(const float* samples, int count, char* out, int size) {
	if (!out || size <= 0) {
		return;
	}
	out[0] = '\0';
	if (!s_xv.recognizer || !samples || count <= 0) {
		return;
	}
	const SherpaOnnxOfflineStream* stream = s_xv.create_stream(s_xv.recognizer);
	if (!stream) {
		return;
	}
	/* Silence either side, as the sherpa-onnx tools pad. */
	static const float pad[4800] = { 0 };
	s_xv.accept(stream, 16000, pad, (int32_t)(sizeof pad / sizeof pad[0]));
	s_xv.accept(stream, 16000, samples, count);
	s_xv.accept(stream, 16000, pad, (int32_t)(sizeof pad / sizeof pad[0]));
	s_xv.decode(s_xv.recognizer, stream);
	const SherpaOnnxOfflineRecognizerResult* r = s_xv.get_result(stream);
	if (r) {
		int n = 0;
		for (const char* c = r->text; c && *c && n + 1 < size; ++c) {
			out[n++] = (char)tolower((unsigned char)*c);
		}
		out[n] = '\0';
		s_xv.destroy_result(r);
	}
	s_xv.destroy_stream(stream);
}

void TieVoice_Unload(void) {
	if (s_xv.recognizer) {
		s_xv.destroy_recognizer(s_xv.recognizer);
		s_xv.recognizer = NULL;
	}
	/* The library stays loaded (onnxruntime does not like being unloaded). */
}
