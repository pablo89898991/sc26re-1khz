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
int main(void)
{
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
	printf("worst %.4f deg\n", worst);
	return worst < 0.05 ? 0 : 1;
}
