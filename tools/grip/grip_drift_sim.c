/* Grip drift tracking (olympus.c grip_drift_update) on made-up readings. Regenerate the
 * function from olympus.c before running: see the scenarios in main(). gcc -O1 grip_drift_sim.c */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CLAMP(v,l,h) MIN(MAX(v,l),h)
#define ARG_UNUSED(x) (void)(x)
static int64_t now_ms; static int64_t k_uptime_get(void){return now_ms;}
enum {SC26RE_CFG_GRIP_DRIFT}; static int cfg=60; static int sc26re_cfg_get(int id){return cfg;}
struct olympus_pad_state { bool grip_touched; int32_t grip_rest_x16; int32_t grip_floor; int64_t grip_band_since_ms; int64_t grip_rest_at_ms; };
#define GRIP_REST_UP_SHIFT 12      /* rest level moves (reading - rest) / 4096 per ms up: half way in ~2.8 s */

static int32_t grip_drift_update(struct olympus_pad_state *state, int32_t value,
                                 int32_t touch, int32_t release)
{
	int32_t cap_pct = sc26re_cfg_get(SC26RE_CFG_GRIP_DRIFT);
	int64_t now = k_uptime_get();
	int32_t cap_x16, rest, dt;

	if(cap_pct <= 0)
	{
		state->grip_rest_x16 = 0;
		return 0;
	}
	cap_x16 = (touch * cap_pct / 100) * 16;
	dt = (int32_t)CLAMP(now - state->grip_rest_at_ms, 0, 100);
	state->grip_rest_at_ms = now;
	rest = state->grip_rest_x16 / 16;

	if(state->grip_floor == 0 && !state->grip_touched)
	{
		/* the first reading is the hands-off level to follow from */
		state->grip_rest_x16 = MAX(value, 0) * 16;
	}
	if(!state->grip_touched)
	{
		int32_t target_x16 = MAX(value, 0) * 16;

		if(target_x16 < state->grip_rest_x16)
		{
			state->grip_rest_x16 = target_x16;
		}
		else if(target_x16 - state->grip_rest_x16 < touch * 16 / 4)
		{
			/* only small, slow changes are drift; a hand coming closer jumps far above the
			 * rest level and must not be learned */
			int32_t step = ((target_x16 - state->grip_rest_x16) * dt) >> GRIP_REST_UP_SHIFT;

			state->grip_rest_x16 += MAX(step, target_x16 > state->grip_rest_x16 ? 1 : 0);
		}
		state->grip_band_since_ms = 0;
	}
	/* v51.4 test 3: no "stuck for 8 s" release any more. A light real grip reads between the release
	 * and the touch level too, so the grip turned off under the hand after 8 s (and Steam then
	 * moved the trigger haptics from the grips to the trackpads). */
	/* v51.4 test 5: the thresholds move only by how far the hands-off level has risen above the
	 * lowest it was. The hands-off reading need not be near 0: on battery it is x1.5, and a
	 * controller can rest at 500-700. Taking the whole rest level off the reading (test 2-4)
	 * then left a real hand below the touch level: the grips never turned on, and Steam moved
	 * the trigger haptics to the trackpads. */
	if(!state->grip_touched && (state->grip_floor == 0 || value + 1 < state->grip_floor))
	{
		state->grip_floor = MAX(value, 0) + 1;
	}
	ARG_UNUSED(release);
	ARG_UNUSED(rest);
	return CLAMP(state->grip_rest_x16 / 16 - (state->grip_floor - 1), 0, cap_x16 / 16);
}

static struct olympus_pad_state st;
static int touch=900, rel=720;
static bool step(int v){ int32_t rest=grip_drift_update(&st,v,touch,rel); int32_t a=v-rest; st.grip_touched = st.grip_touched ? a>=rel : a>touch; return st.grip_touched; }
int main(){
 /* 1: drift 0->800 over 5 min, a 2 s grab (1000 on top of drift) every 20 s */
 int stuck=0, missed=0; for(now_ms=0; now_ms<300000; now_ms+=4){ int drift=(int)(800*now_ms/300000); bool hand=(now_ms%20000)<2000; bool on=step(drift+(hand?1000:0)); if(!hand && (now_ms%20000)>3000 && on) stuck++; if(hand && (now_ms%20000)>200 && !on) missed++; }
 printf("drift test: stuck samples %d, missed-grab samples %d, rest %d\n", stuck, missed, st.grip_rest_x16/16);
 /* 2: steady hand 60 s at 1000 */
 st=(struct olympus_pad_state){0}; int off=0; for(now_ms=0; now_ms<60000; now_ms+=4){ bool on=step(1000); if(now_ms>100 && !on) off++; } printf("hold 60 s: off samples %d\n", off);
 /* 3 (v51.4 test 3: must stay on now): hand at 1000 but light grip 850 (between release and touch) held 60 s: released after 8 s? */
 st=(struct olympus_pad_state){0}; for(now_ms=0; now_ms<200; now_ms+=4) step(1000); int offl=-1; for(; now_ms<60000; now_ms+=4){ if(!step(850) && offl<0) offl=(int)now_ms; } printf("light 850 after grab: off at %d ms\n", offl);
 /* 4: hand approaching slowly: 0 -> 1000 over 1 s */
 st=(struct olympus_pad_state){0}; int onat=-1; for(now_ms=0; now_ms<3000; now_ms+=4){ int v= now_ms<1000? (int)(now_ms) : 1000; if(step(v)&&onat<0) onat=(int)now_ms; } printf("slow approach 1 s: on at %d ms, rest %d\n", onat, st.grip_rest_x16/16);
 /* 6: drift jumps: +100 steps */ st=(struct olympus_pad_state){0}; int on6=0; for(now_ms=0; now_ms<120000; now_ms+=4){ int v=(int)(now_ms/20000)*120; if(step(v)) on6++; } printf("drift in 120-steps to 600: on samples %d rest %d\n", on6, st.grip_rest_x16/16);
 /* 5: wireless x1.5 at rest: 500*1.5=750 from the start */
 st=(struct olympus_pad_state){0}; int on5=0; for(now_ms=0; now_ms<30000; now_ms+=4){ if(step(750)) on5++; } printf("rest 750 from boot: on samples %d\n", on5);
 /* 7 (test 5): hands-off 600 from boot (battery x1.5), a hand reads 1100: must turn on */
 st=(struct olympus_pad_state){0}; int on7=-1; for(now_ms=0; now_ms<60000; now_ms+=4){ bool hand=now_ms>=30000; if(step(hand?1100:600) && on7<0) on7=(int)now_ms; } printf("rest 600, hand 1100 at 30 s: on at %d ms\n", on7);
 /* 8: hands-off 600 drifting to 1100 over 5 min, grabs +500 every 20 s */
 st=(struct olympus_pad_state){0}; int stuck8=0, missed8=0; for(now_ms=0; now_ms<300000; now_ms+=4){ int base=600+(int)(500*now_ms/300000); bool hand=(now_ms%20000)<2000 && now_ms>5000; bool on=step(base+(hand?500:0)); if(!hand && (now_ms%20000)>3000 && on) stuck8++; if(hand && (now_ms%20000)>200 && !on) missed8++; } printf("rest 600->1100 drift with grabs: stuck %d, missed %d\n", stuck8, missed8);
 return 0; }
