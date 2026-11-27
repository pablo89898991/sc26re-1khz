/* Slow return to centre for one stick axis ("Direct Lean"), after the filter_axis() of
 * u/z50_Jumper's UNC THUMBS (Python, damping left out): an own C version for the firmware.
 *
 * p: the stick now, pp: the stick the pass before, v: the output the pass before (all -1..1),
 * dt: seconds since, ret_s: seconds for the output to come back from full deflection,
 * mult: how much faster it catches up while countersteering (>= 1).
 * - Travel of the stick away from its centre moves the output by the same amount (also when it
 *   crosses over: countersteering), travel towards the centre does not.
 * - Then the output moves at a constant speed: back to the centre (1 / ret_s per second), but
 *   never inside a stick held on the same side; out to a stick held further (mult times as fast);
 *   and towards the centre at mult times the speed while the stick is on the other side. */
#ifndef LEAN_H
#define LEAN_H

static inline float lean_clamp(float x)
{
	return x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
}

static inline float lean_filter(float p, float pp, float v, float dt, float ret_s, float mult)
{
	float d, step, fast;

	p = lean_clamp(p);
	pp = lean_clamp(pp);
	v = lean_clamp(v);
	if(!(dt > 0.0f))
	{
		dt = 0.0f;
	}
	if(!(ret_s > 0.0f))
	{
		return p;
	}
	/* travel away from the stick's centre, on each side */
	d = fmaxf(0.0f, fmaxf(0.0f, p) - fmaxf(0.0f, pp)) + fminf(0.0f, fminf(0.0f, p) - fminf(0.0f, pp));
	v = lean_clamp(v + d);
	step = dt / ret_s;
	fast = step * fmaxf(1.0f, mult);
	if(p > 0.0f && v >= 0.0f && p > v)
	{
		v = fminf(p, v + fast);
	}
	else if(p < 0.0f && v <= 0.0f && p < v)
	{
		v = fmaxf(p, v - fast);
	}
	else
	{
		float boundary = 0.0f, in = p * v < 0.0f ? fast : step;

		if(v > 0.0f && p > 0.0f)
		{
			boundary = fminf(v, p);
		}
		else if(v < 0.0f && p < 0.0f)
		{
			boundary = fmaxf(v, p);
		}
		if(v > boundary)
		{
			v = fmaxf(boundary, v - in);
		}
		else if(v < boundary)
		{
			v = fminf(boundary, v + in);
		}
	}
	return lean_clamp(v);
}

#endif
