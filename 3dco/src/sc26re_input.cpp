#include "sc26re_input.h"

#include <math.h>
#include <string.h>

// sc26re/raw (56 bytes): buttons (le32 at 0), 12 x le16 at 4 (triggers L R, left stick x y,
// right stick x y, left pad x y pressure, right pad x y pressure), accel (3 x le16 at 34), the
// gyro integrated since power-on as a quaternion (w x y z, le16 x 32767 at 40), the reply's
// time (le32 at 52, us). Buttons are the pad report's bits (Valve's Triton layout).
enum
{
	B_A = 0x00000001, B_B = 0x00000002, B_X = 0x00000004, B_Y = 0x00000008,
	B_QAM = 0x00000010, B_R3 = 0x00000020, B_VIEW = 0x00000040, B_R4 = 0x00000080,
	B_R5 = 0x00000100, B_R = 0x00000200, B_DOWN = 0x00000400, B_RIGHT = 0x00000800,
	B_LEFT = 0x00001000, B_UP = 0x00002000, B_MENU = 0x00004000, B_L3 = 0x00008000,
	B_STEAM = 0x00010000, B_L4 = 0x00020000, B_L5 = 0x00040000, B_L = 0x00080000,
	B_RSTICK_TOUCH = 0x00100000, B_RPAD_TOUCH = 0x00200000, B_RPAD_CLICK = 0x00400000,
	B_LSTICK_TOUCH = 0x01000000, B_LPAD_TOUCH = 0x02000000, B_LPAD_CLICK = 0x04000000,
	B_RGRIP = 0x10000000, B_LGRIP = 0x20000000,
};

#define RAW_PATH "sc26re/raw"
#define RAW_SIZE 56
#define REPORT_BYTES 64 // feature report 1: id + 63 bytes
#define OP_READ 0xED

struct sc26re_input
{
	SDL_hid_device *dev;
	SDL_Thread *thread;
	SDL_Mutex *lock;
	SDL_AtomicInt run;

	// the newest state (under lock)
	Uint32 buttons;
	Sint16 v[12];
	Uint64 fresh_ns; // when it came (SDL_GetTicksNS)
	float rot[3];    // turn not yet taken, SDL gyro frame, radians
	bool rot_new;
	int rate, count;
	Uint64 rate_ns;
};

static Uint8 path_tag(const char *p)
{
	unsigned t = 0xA5;
	for (; *p; p++) {
		t = ((t << 1) ^ (t >> 7) ^ (Uint8)*p) & 255;
	}
	return (Uint8)t;
}

static Sint16 le16(const Uint8 *b) { return (Sint16)(b[0] | (b[1] << 8)); }
static Uint32 le32(const Uint8 *b) { return (Uint32)b[0] | ((Uint32)b[1] << 8) | ((Uint32)b[2] << 16) | ((Uint32)b[3] << 24); }

// One read of a settings path: the request, then the reply (it takes a few ms over USB, the Puck
// or Bluetooth; asked too early the controller hands out the previous reply). Another program
// (Steam, the remapper page) may use the channel too: a reply with another path's tag is
// theirs, step aside and ask again. Returns the payload length, 0 for "this firmware has no such
// path" (an empty answer, three times), -1 for no answer.
static int read_path(SDL_hid_device *dev, const char *path, Uint8 *out, int cap)
{
	const Uint8 tag = path_tag(path);
	const size_t n = strlen(path);
	int empty = 0;

	for (int tries = 0; tries < 4; tries++) {
		Uint8 buf[REPORT_BYTES] = { 1 };
		buf[1] = OP_READ;
		buf[2] = (Uint8)(n + 1);
		memcpy(&buf[3], path, n);
		if (SDL_hid_send_feature_report(dev, buf, sizeof(buf)) < 0) {
			SDL_Delay(20);
			continue;
		}
		for (int k = 0; k < 6; k++) {
			Uint8 r[REPORT_BYTES] = { 1 };
			const int got = SDL_hid_get_feature_report(dev, r, sizeof(r));
			const int o = got < 3 ? -1 : r[1] == OP_READ ? 1 : (r[0] == OP_READ ? 0 : -1);
			if (o >= 0) {
				const int len = SDL_min((int)r[o + 1], got - o - 2);
				if (len <= 0) {
					if (++empty >= 3) {
						return 0;
					}
					break;
				}
				empty = 0;
				if (r[o + 2] == tag) {
					const int m = SDL_min(len - 1, cap);
					memcpy(out, &r[o + 3], (size_t)m);
					return m;
				}
				SDL_Delay(30 + (SDL_rand(20)));
				break;
			}
			SDL_Delay(2 + k * 4);
		}
	}
	return -1;
}

static void qmul(const float a[4], const float b[4], float o[4])
{
	o[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
	o[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
	o[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
	o[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}

static int SDLCALL reader(void *data)
{
	sc26re_input *s = (sc26re_input *)data;
	float prev_q[4] = { 1, 0, 0, 0 };
	bool have_q = false;
	bool have_ts = false;
	Uint32 last_ts = 0;
	int fails = 0;

	while (SDL_GetAtomicInt(&s->run)) {
		Uint8 b[RAW_SIZE];
		const int n = read_path(s->dev, RAW_PATH, b, sizeof(b));
		if (n == 0) {
			SDL_Log("sc26re: this firmware has no native state (stock, or sc26re before v51): using SDL's pad report");
			break;
		}
		if (n < RAW_SIZE) {
			if (++fails > 50) {
				SDL_Delay(1000);
			} else {
				SDL_Delay(10);
			}
			continue;
		}
		fails = 0;

		// not newer than the last reply: the previous one again (asked too early over the Puck or
		// Bluetooth); much older: the controller started again
		const Uint32 ts = le32(&b[52]);
		const Sint32 d = have_ts ? (Sint32)(ts - last_ts) : 1;
		if (d <= 0 && d > -1000000) {
			SDL_Delay(1);
			continue;
		}
		const float dt = (!have_ts || d <= 0) ? 0.0f : d / 1e6f;
		if (d <= 0) {
			have_q = false;
		}
		last_ts = ts;
		have_ts = true;

		// the turn since the last read: the difference of the controller's own orientation
		float q[4];
		for (int i = 0; i < 4; i++) {
			q[i] = le16(&b[40 + 2 * i]) / 32767.0f;
		}
		const float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		float turn[3] = { 0, 0, 0 };
		bool turned = false;
		if (ql > 0.5f) {
			for (int i = 0; i < 4; i++) {
				q[i] /= ql;
			}
			if (have_q) {
				const float inv[4] = { prev_q[0], -prev_q[1], -prev_q[2], -prev_q[3] };
				float dq[4];
				qmul(inv, q, dq);
				if (dq[0] < 0) {
					for (int i = 0; i < 4; i++) {
						dq[i] = -dq[i];
					}
				}
				const float sn = sqrtf(dq[1] * dq[1] + dq[2] * dq[2] + dq[3] * dq[3]);
				const float ang = 2.0f * atan2f(sn, dq[0]);
				// faster than the gyro can measure (2000 deg/s): the controller started again
				if (sn > 1e-9f && ang <= (2100.0f * dt + 1.0f) * (SDL_PI_F / 180.0f)) {
					const float k = ang / sn;
					// controller axes (x, y, z) -> SDL's gyro frame (x, z, -y), as SDL's driver does
					turn[0] = dq[1] * k;
					turn[1] = dq[3] * k;
					turn[2] = -dq[2] * k;
					turned = true;
				}
			}
			memcpy(prev_q, q, sizeof(prev_q));
			have_q = true;
		}

		SDL_LockMutex(s->lock);
		s->buttons = le32(&b[0]);
		for (int i = 0; i < 12; i++) {
			s->v[i] = le16(&b[4 + 2 * i]);
		}
		s->fresh_ns = SDL_GetTicksNS();
		if (turned) {
			for (int i = 0; i < 3; i++) {
				s->rot[i] += turn[i];
			}
			s->rot_new = true;
		}
		s->count++;
		if (s->fresh_ns - s->rate_ns >= SDL_NS_PER_SECOND) {
			s->rate = s->count;
			s->count = 0;
			s->rate_ns = s->fresh_ns;
		}
		SDL_UnlockMutex(s->lock);
		SDL_Delay(1);
	}
	return 0;
}

sc26re_input *sc26re_open(SDL_Gamepad *gamepad)
{
	if (gamepad == NULL || SDL_GetGamepadVendor(gamepad) != 0x28de) {
		return NULL;
	}
	const Uint16 pid = SDL_GetGamepadProduct(gamepad);
	if (pid < 0x1302 || pid > 0x1305) { // the controller wired / Bluetooth, the Puck (Proteus, Nereid)
		return NULL;
	}
	const char *path = SDL_GetGamepadPath(gamepad);
	if (path == NULL || *path == 0) {
		return NULL;
	}
	SDL_hid_device *dev = SDL_hid_open_path(path);
	if (dev == NULL) {
		SDL_Log("sc26re: could not open %s: %s", path, SDL_GetError());
		return NULL;
	}
	sc26re_input *s = (sc26re_input *)SDL_calloc(1, sizeof(*s));
	if (s == NULL) {
		SDL_hid_close(dev);
		return NULL;
	}
	s->dev = dev;
	s->lock = SDL_CreateMutex();
	SDL_SetAtomicInt(&s->run, 1);
	s->thread = SDL_CreateThread(reader, "sc26re native input", s);
	SDL_Log("sc26re: reading the controller's native state on %s", path);
	return s;
}

void sc26re_close(sc26re_input *s)
{
	if (s == NULL) {
		return;
	}
	SDL_SetAtomicInt(&s->run, 0);
	if (s->thread) {
		SDL_WaitThread(s->thread, NULL);
	}
	SDL_hid_close(s->dev);
	SDL_DestroyMutex(s->lock);
	SDL_free(s);
}

bool sc26re_active(sc26re_input *s)
{
	if (s == NULL) {
		return false;
	}
	SDL_LockMutex(s->lock);
	const bool on = s->fresh_ns != 0 && SDL_GetTicksNS() - s->fresh_ns < SDL_NS_PER_SECOND / 4;
	SDL_UnlockMutex(s->lock);
	return on;
}

static Uint32 buttons_of(sc26re_input *s)
{
	SDL_LockMutex(s->lock);
	const Uint32 b = s->buttons;
	SDL_UnlockMutex(s->lock);
	return b;
}

bool sc26re_button(sc26re_input *s, SDL_GamepadButton button)
{
	const Uint32 b = buttons_of(s);
	// as SDL's driver and its mapping ("misc1:b11,paddle1:b12,paddle2:b13,paddle3:b14,
	// paddle4:b15,touchpad:b17,misc2:b16")
	switch (button) {
	case SDL_GAMEPAD_BUTTON_SOUTH: return (b & B_A) != 0;
	case SDL_GAMEPAD_BUTTON_EAST: return (b & B_B) != 0;
	case SDL_GAMEPAD_BUTTON_WEST: return (b & B_X) != 0;
	case SDL_GAMEPAD_BUTTON_NORTH: return (b & B_Y) != 0;
	case SDL_GAMEPAD_BUTTON_BACK: return (b & B_MENU) != 0;
	case SDL_GAMEPAD_BUTTON_GUIDE: return (b & B_STEAM) != 0;
	case SDL_GAMEPAD_BUTTON_START: return (b & B_VIEW) != 0;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: return (b & B_L3) != 0;
	case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return (b & B_R3) != 0;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return (b & B_L) != 0;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return (b & B_R) != 0;
	case SDL_GAMEPAD_BUTTON_DPAD_UP: return (b & B_UP) != 0;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return (b & B_DOWN) != 0;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return (b & B_LEFT) != 0;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return (b & B_RIGHT) != 0;
	case SDL_GAMEPAD_BUTTON_MISC1: return (b & B_QAM) != 0;
	case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return (b & B_R4) != 0;
	case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return (b & B_L4) != 0;
	case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return (b & B_R5) != 0;
	case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return (b & B_L5) != 0;
	case SDL_GAMEPAD_BUTTON_TOUCHPAD: return (b & B_LPAD_CLICK) != 0;
	case SDL_GAMEPAD_BUTTON_MISC2: return (b & B_RPAD_CLICK) != 0;
	default: return false;
	}
}

Sint16 sc26re_axis(sc26re_input *s, SDL_GamepadAxis axis)
{
	SDL_LockMutex(s->lock);
	Sint16 v[12];
	memcpy(v, s->v, sizeof(v));
	SDL_UnlockMutex(s->lock);
	const auto neg = [](Sint16 x) { return (Sint16)(x == -32768 ? 32767 : -x); };
	switch (axis) {
	case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return (Sint16)SDL_max(v[0], 0);
	case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return (Sint16)SDL_max(v[1], 0);
	case SDL_GAMEPAD_AXIS_LEFTX: return v[2];
	case SDL_GAMEPAD_AXIS_LEFTY: return neg(v[3]);
	case SDL_GAMEPAD_AXIS_RIGHTX: return v[4];
	case SDL_GAMEPAD_AXIS_RIGHTY: return neg(v[5]);
	default: return 0;
	}
}

bool sc26re_finger(sc26re_input *s, int touchpad, bool *down, float *x, float *y, float *pressure)
{
	SDL_LockMutex(s->lock);
	const Uint32 b = s->buttons;
	const Sint16 px = s->v[touchpad == 0 ? 6 : 9], py = s->v[touchpad == 0 ? 7 : 10];
	const Uint16 pr = (Uint16)s->v[touchpad == 0 ? 8 : 11];
	SDL_UnlockMutex(s->lock);
	*down = (b & (touchpad == 0 ? B_LPAD_TOUCH : B_RPAD_TOUCH)) != 0;
	*x = px / 65536.0f + 0.5f;
	*y = -(float)py / 65536.0f + 0.5f;
	*pressure = pr / 32768.0f;
	return true;
}

bool sc26re_capsense(sc26re_input *s, SDL_GamepadCapSenseType type)
{
	const Uint32 b = buttons_of(s);
	switch (type) {
	case SDL_GAMEPAD_CAPSENSE_LEFT_STICK: return (b & B_LSTICK_TOUCH) != 0;
	case SDL_GAMEPAD_CAPSENSE_RIGHT_STICK: return (b & B_RSTICK_TOUCH) != 0;
	case SDL_GAMEPAD_CAPSENSE_LEFT_GRIP: return (b & B_LGRIP) != 0;
	case SDL_GAMEPAD_CAPSENSE_RIGHT_GRIP: return (b & B_RGRIP) != 0;
	default: return false;
	}
}

bool sc26re_take_rotation(sc26re_input *s, float rot[3])
{
	SDL_LockMutex(s->lock);
	const bool n = s->rot_new;
	memcpy(rot, s->rot, sizeof(s->rot));
	memset(s->rot, 0, sizeof(s->rot));
	s->rot_new = false;
	SDL_UnlockMutex(s->lock);
	return n;
}

int sc26re_rate(sc26re_input *s)
{
	if (s == NULL) {
		return 0;
	}
	SDL_LockMutex(s->lock);
	const int r = s->rate;
	SDL_UnlockMutex(s->lock);
	return r;
}
