/* host test for app/src/curve.h:
 * gcc -fsanitize=address,undefined -I app/src -o /tmp/tc tests/test_curve.c -lm && /tmp/tc */
#include <math.h>
#include <stdio.h>
#include "curve.h"

static int fails;
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while(0)

int main(void)
{
	/* the user's screenshot: close to linear */
	const uint16_t shot[8] = { 5855, 5898, 11711, 12000, 19187, 20000, 29030, 28768 };
	/* points on the diagonal: identity */
	const uint16_t diag[8] = { 6553, 6553, 13107, 13107, 19660, 19660, 26214, 26214 };
	/* unsorted input: same as sorted */
	const uint16_t uns[8] = { 19660, 26000, 6553, 1000, 26214, 30000, 13107, 9000 };
	const uint16_t srt[8] = { 6553, 1000, 13107, 9000, 19660, 26000, 26214, 30000 };
	/* anti dead zone: a step at x = 0 */
	const uint16_t step[8] = { 0, 8192, 8192, 12000, 16384, 18000, 24576, 26000 };
	/* last point at the top right x: step up to (1, 1) at the end */
	const uint16_t top[8] = { 8192, 4096, 16384, 8192, 24576, 12288, 32767, 16384 };
	/* all points at 0: a flat line jumps to 1 only past x = 0 */
	const uint16_t zero[8] = { 0 };
	/* out-of-range values are clamped */
	const uint16_t big[8] = { 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535 };

	CHECK(curve_eval(diag, 0.0f) == 0.0f);
	CHECK(curve_eval(diag, -0.5f) == 0.0f);
	for(float x = 0.01f; x <= 1.0f; x += 0.01f)
	{
		CHECK(NEAR(curve_eval(diag, x), x));
	}
	/* above 1: the last segment goes on (identity here) */
	CHECK(NEAR(curve_eval(diag, 2.5f), 2.5f));

	/* through each point exactly */
	CHECK(NEAR(curve_eval(shot, 5855.0f / 32767.0f), 5898.0f / 32767.0f));
	CHECK(NEAR(curve_eval(shot, 19187.0f / 32767.0f), 20000.0f / 32767.0f));
	CHECK(NEAR(curve_eval(shot, 1.0f), 1.0f));
	/* between points: linear */
	CHECK(NEAR(curve_eval(shot, (5855.0f + 11711.0f) / 2.0f / 32767.0f), (5898.0f + 12000.0f) / 2.0f / 32767.0f));
	/* monotonic for a monotonic curve */
	{
		float prev = 0.0f;

		for(float x = 0.0f; x <= 1.2f; x += 0.001f)
		{
			float y = curve_eval(shot, x);

			CHECK(y >= prev - 1e-6f);
			prev = y;
		}
	}
	for(float x = 0.0f; x <= 1.3f; x += 0.05f)
	{
		CHECK(NEAR(curve_eval(uns, x), curve_eval(srt, x)));
	}
	/* step at 0: just above 0 the output starts at the first point's y */
	CHECK(curve_eval(step, 0.0f) == 0.0f);
	CHECK(fabsf(curve_eval(step, 1e-5f) - 8192.0f / 32767.0f) < 1e-3f);
	/* last point at x = 1: f(1) is that point; above, its segment goes on */
	CHECK(NEAR(curve_eval(top, 1.0f), 16384.0f / 32767.0f));
	CHECK(curve_eval(top, 1.5f) > curve_eval(top, 1.0f));
	CHECK(curve_eval(top, 1.5f) < 1.0f);
	/* all points at the origin: from (0,0) straight to (1,1) */
	CHECK(NEAR(curve_eval(zero, 0.5f), 0.5f));
	/* all at the top right corner: the line from 0 to there */
	CHECK(NEAR(curve_eval(big, 0.5f), 0.5f));
	CHECK(NEAR(curve_eval(big, 1.0f), 1.0f));
	CHECK(isfinite(curve_eval(big, 3.0f)));
	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails != 0;
}
