SRC=../../app/src
libsim.so: sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/remap.h $(SRC)/xinput_guide.c
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/xinput_guide.c -lm

libsim_asan.so: sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/remap.h
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -fsanitize=address,undefined,float-cast-overflow,float-divide-by-zero -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/xinput_guide.c -lm

libsim_cov.so: sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/remap.h
	gcc -O0 -g -fPIC -shared --coverage -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/xinput_guide.c -lm

libsim_gm.so: sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/gyro_mouse.c $(SRC)/remap.h
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -DWITH_GM -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/gyro_mouse.c -lm

libsim_gm_asan.so: sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/gyro_mouse.c $(SRC)/remap.h
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -DWITH_GM -fsanitize=address,undefined,float-cast-overflow,float-divide-by-zero -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Iinc -I$(SRC) -DSC26RE_SIM -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/gyro_mouse.c -lm
