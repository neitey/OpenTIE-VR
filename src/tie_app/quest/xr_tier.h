#ifndef TIE_APP_QUEST_XR_TIER_H
#define TIE_APP_QUEST_XR_TIER_H

/* Standalone headsets differ a lot in GPU power and memory (Quest 2 and
 * Pico 4: XR2 Gen 1, 6 / 8 GB; Quest 3 and Pico 4 Ultra: XR2 Gen 2; Steam
 * Frame: Snapdragon 8 Gen 3, 16 GB). The tier, picked from the GPU's name and
 * the RAM, sets the defaults that depend on it: the per-eye pixel budget, the
 * refresh rate and the foveation level. Settings the player made win. */
typedef enum TieXrTier {
	TIE_XR_TIER_UNKNOWN = 0, /* not a GPU we know: treated as mid */
	TIE_XR_TIER_LOW,         /* Adreno 6xx (XR2 Gen 1) or under 7 GB of RAM */
	TIE_XR_TIER_MID,         /* Adreno 7xx before 750 (XR2 Gen 2, XR2+ Gen 2) */
	TIE_XR_TIER_HIGH,        /* Adreno 750 and later (Snapdragon 8 Gen 3 on) */
} TieXrTier;

/* gpu_name: the Vulkan device name (NULL or "" if unknown); ram_mb: 0 if
 * unknown. */
TieXrTier TieXrTier_Classify(const char* gpu_name, int ram_mb);
const char* TieXrTier_Name(TieXrTier tier);
/* Per-eye pixel budget of the tier (AERON_XR_EYE_PIXELS). */
long TieXrTier_EyePixels(TieXrTier tier);

/* Probes the GPU (a throwaway Vulkan instance) and the RAM, picks the tier
 * (XWA_XR_TIER=low|mid|high overrides it) and sets the environment defaults
 * the player has not set. Call before Aeron_Init. Returns the tier. */
TieXrTier TieXrTier_ApplyDefaults(void);

#endif
