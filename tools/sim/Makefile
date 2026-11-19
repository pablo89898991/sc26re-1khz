SRC=../../app/src
DEPS=sim.c $(SRC)/hwtest.c $(SRC)/hwtest.h $(SRC)/remap.c $(SRC)/hwtest.c $(SRC)/sc26re_cfg.c $(SRC)/sc26re_cfg.h $(SRC)/remap.h $(SRC)/xinput_guide.c $(SRC)/gyro_mouse.c $(SRC)/gyro_mouse.h
all: libsim.so libsim_asan.so libsim_cov.so libsim_gm.so libsim_gm_asan.so
libsim.so: $(DEPS)
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/hwtest.c $(SRC)/sc26re_cfg.c $(SRC)/xinput_guide.c -lm

libsim_asan.so: $(DEPS)
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -fsanitize=address,undefined,float-cast-overflow,float-divide-by-zero -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/hwtest.c $(SRC)/sc26re_cfg.c $(SRC)/xinput_guide.c -lm

libsim_cov.so: $(DEPS)
	gcc -O0 -g -fPIC -shared --coverage -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/hwtest.c $(SRC)/sc26re_cfg.c $(SRC)/xinput_guide.c -lm

libsim_gm.so: $(DEPS)
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -DWITH_GM -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/hwtest.c $(SRC)/sc26re_cfg.c $(SRC)/gyro_mouse.c -lm

libsim_gm_asan.so: $(DEPS)
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -DWITH_GM -fsanitize=address,undefined,float-cast-overflow,float-divide-by-zero -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/hwtest.c $(SRC)/sc26re_cfg.c $(SRC)/gyro_mouse.c -lm
