#ifndef TIE_RUNTIME_TIMING_FLIGHT_INTEGRATION_H
#define TIE_RUNTIME_TIMING_FLIGHT_INTEGRATION_H

#include <stdint.h>

struct FlightObject;

uint16_t TieFlightIntegration_AutopilotStep(uint16_t obj_idx, unsigned int axis, int16_t rate_cap,
											int16_t pacing, uint16_t axis_scale);
void TieFlightIntegration_Move(uint16_t object_index, struct FlightObject* object);
/* High-rate step for one MOVE external-push accumulator (axis 0..2). `clamped`
 * is the accumulator clamped to the push cap; the result never overshoots
 * `accum`, and the axis remainder resets when the accumulator drains. */
int32_t TieFlightIntegration_PushStep(uint16_t object_index, unsigned int axis, int32_t accum,
									  int32_t clamped);
void TieFlightIntegration_SeparateFriendly(void);
/* Logical movement frames, independent of host ticks. */
void TieFlightIntegration_BeginFrame(void);
uint32_t TieFlightIntegration_Frame(void);
/* Where between the previous movement frame (0, world_*_prev) and the newest
 * (1, world_*) this host frame shows the world: a smoothed presentation
 * clock, so motion advances evenly per displayed frame instead of in whole
 * 4 ms simulation ticks. Call once per host tick. */
float TieFlightIntegration_PresentAlpha(void);
void TieFlightIntegration_ResetPresentation(void);

#endif
