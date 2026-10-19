#ifndef TIE_APP_QUEST_VOICE_CORE_H
#define TIE_APP_QUEST_VOICE_CORE_H

/*
 * Voice commands, the speech recognizer: English, sherpa-onnx loaded at run
 * time (dlopen), a non-streaming model that hears the whole push-to-talk
 * utterance. Ported from OpenXWA-VR. The phrase -> action matcher is in
 * voice_commands.c, the VR glue (microphone, push-to-talk, feedback) in
 * vr_voice.c.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the recognizer library (`lib` as given to dlopen) and the model in
 * `model_dir` (encoder.onnx, decoder.onnx, joiner.onnx, tokens.txt: an
 * offline transducer). Slow (about a second): call it off the frame
 * thread. Returns 1 when ready; on failure `error` says why. */
int TieVoice_Load(const char* lib, const char* model_dir, int threads, char* error, int error_size);
int TieVoice_Ready(void);

/* Recognizes one utterance (16 kHz mono, -1..1) and writes what was said,
 * lower case. Takes a fraction of the utterance's length: call it off the
 * frame thread. */
void TieVoice_Recognize(const float* samples, int count, char* out, int size);
void TieVoice_Unload(void);

#ifdef __cplusplus
}
#endif

#endif
