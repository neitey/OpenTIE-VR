#ifndef TIE_FLIGHT_STARS_H
#define TIE_FLIGHT_STARS_H

#include <stdbool.h>
#include <stdint.h>

#include "aeron/render.h"
#include "tie_remaster/flight/render_config.h"

typedef struct TieFlightCamera TieFlightCamera;
typedef struct TieFlightStars TieFlightStars;
typedef struct TieSnapshot TieSnapshot;

TieFlightStars* TieFlightStars_Create(AeronTextureFormat rt_format);
void TieFlightStars_Destroy(TieFlightStars* stars);

/* Uploads the direction list before the scene render pass opens. AUTO
 * selects the style of the snapshot's flight edition. */
bool TieFlightStars_Prepare(TieFlightStars* stars, AeronCommandBuffer* cmd, const TieSnapshot* snapshot,
							TieFlightStarfieldStyle style);

/* Draws one instanced rounded quad per direction inside the scene pass. */
void TieFlightStars_DrawInPass(TieFlightStars* stars, AeronCommandBuffer* cmd, AeronRenderPass* pass,
							   const TieSnapshot* snapshot, const TieFlightCamera* camera,
							   const float view_proj[16]);

/* A star field of the caller's own (the VR menu environment): `stars` are
 * directions (xyz) with a linear intensity (w), uploaded before the scene
 * pass; drawn as round dots of `half_size_px` with view_proj's rotation. */
bool TieFlightStars_PrepareCustom(TieFlightStars* stars, AeronCommandBuffer* cmd, const float (*dirs)[4],
								  uint16_t count);
void TieFlightStars_DrawCustom(TieFlightStars* stars, AeronCommandBuffer* cmd, AeronRenderPass* pass,
							   const float view_proj[16], int rt_w, int rt_h, float half_size_px);

/* OpenTIE-VR direct output (no pass hooks allowed): the stars Prepare staged
 * as additive SKY billboards (round dots of `half_tan` tangent half-size at
 * the far distance) for the directions in front of `camera_ori` (within
 * ~80 deg, antipodes as the classic renderer). Returns how many. */
struct AeronScene3D;
int TieFlightStars_SubmitSprites(TieFlightStars* stars, struct AeronScene3D* scene, AeronCommandBuffer* cmd,
								 const TieSnapshot* snapshot, const float camera_ori[4], float half_tan,
								 int max_sprites);

#endif
