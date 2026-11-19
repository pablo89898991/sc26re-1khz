/* the 3D overlay orientation (app/src/imu_raw.h) against double precision: gcc -I../../app/src test_imu_raw.c -lm */
#include <stdio.h>
#include <stdlib.h>
#include "imu_raw.h"
/* imu_raw.h against double precision: random rates, sample periods 0.25..50 ms */
static void dturn(double q[4], const double w[3], double dt)
{
	double a = sqrt(w[0]*w[0]+w[1]*w[1]+w[2]*w[2]) * dt, k = a > 0 ? sin(a/2) / (a/dt) : 0, d[4] = { cos(a/2), w[0]*k, w[1]*k, w[2]*k };
	double o[4] = { q[0]*d[0]-q[1]*d[1]-q[2]*d[2]-q[3]*d[3], q[0]*d[1]+q[1]*d[0]+q[2]*d[3]-q[3]*d[2], q[0]*d[2]-q[1]*d[3]+q[2]*d[0]+q[3]*d[1], q[0]*d[3]+q[1]*d[2]-q[2]*d[1]+q[3]*d[0] };
	double l = sqrt(o[0]*o[0]+o[1]*o[1]+o[2]*o[2]+o[3]*o[3]); for(int i = 0; i < 4; ++i) q[i] = o[i] / l;
}
/* the same turn just under 0.1 rad (the edge of the series) 10000 times: each turn is exact to a
 * few nanoradians, a missing series term adds up (about h^5 / 12 rad a turn: 3e-4 rad here) */
static double edge(double ax, double ay, double az, double n, int steps)
{
	double l = sqrt(ax*ax + ay*ay + az*az); ax /= l; ay /= l; az /= l;
	float r[3] = { (float)(ax * n), (float)(ay * n), (float)(az * n) };
	float q[4] = { 1, 0, 0, 0 };
	for(int i = 0; i < steps; ++i) imu_raw_orient_turn(q, r, q);
	double rn = sqrt((double)r[0]*r[0] + (double)r[1]*r[1] + (double)r[2]*r[2]), a = rn * steps / 2;
	double e[4] = { cos(a), r[0] / rn * sin(a), r[1] / rn * sin(a), r[2] / rn * sin(a) };
	/* conj(e) * q: its vector part is the error (acos of a dot product is too coarse for floats) */
	double w = e[0]*q[0] + e[1]*q[1] + e[2]*q[2] + e[3]*q[3];
	double x = e[0]*q[1] - e[1]*q[0] - e[2]*q[3] + e[3]*q[2];
	double y = e[0]*q[2] + e[1]*q[3] - e[2]*q[0] - e[3]*q[1];
	double z = e[0]*q[3] - e[1]*q[2] + e[2]*q[1] - e[3]*q[0];
	return 2 * atan2(sqrt(x*x + y*y + z*z), fabs(w));
}
int main(void)
{
	double edge_worst = 0;
	srand(3);
	for(int k = 0; k < 40; ++k)
	{
		double ax = rand() % 2001 - 1000, ay = rand() % 2001 - 1000, az = rand() % 2001 - 1000;
		double e = edge(ax, ay, az, k % 2 ? 0.0999 : 0.08 + (rand() % 1000) * 0.0000199, 10000);
		if(e > edge_worst) edge_worst = e;
	}
	srand(7);
	double worst = 0;
	for(int run = 0; run < 200; ++run)
	{
		struct imu_raw_orient o; imu_raw_orient_init(&o);
		double q[4] = { 1, 0, 0, 0 };
		int n = 1 + rand() % 3000;
		for(int i = 0; i < n; ++i)
		{
			int16_t g[3]; double w[3];
			float dt = (run % 4 == 0) ? 0.00025f + (rand() % 1000) * 0.00005f : 0.001f;
			for(int k = 0; k < 3; ++k) { g[k] = (int16_t)((rand() % 65535) - 32767); if(run % 3 == 1) g[k] /= 300; w[k] = g[k] * 0.06103 * 0.017453293; }
			imu_raw_orient_add(&o, g, dt); dturn(q, w, dt);
		}
		int16_t out[4]; imu_raw_orient_get(&o, out);
		double f[4] = { out[0] / 32767.0, out[1] / 32767.0, out[2] / 32767.0, out[3] / 32767.0 }, l = sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]+f[3]*f[3]);
		double dot = fabs((f[0]*q[0]+f[1]*q[1]+f[2]*q[2]+f[3]*q[3]) / l), err = 2 * acos(dot > 1 ? 1 : dot) * 180 / M_PI;
		if(err > worst) worst = err;
	}
	printf("edge of the series %.2g rad / 10000 turns (< 1.5e-4), worst %.4f deg (< 0.05)\n", edge_worst, worst);
	return worst < 0.05 && edge_worst < 1.5e-4 ? 0 : 1;
}
