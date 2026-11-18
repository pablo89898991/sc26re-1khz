SRC=../../app/src
libsim.so: sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c $(SRC)/remap.h
	gcc -O1 -g -fPIC -shared -Wall -Wno-unused-function -Iinc -I$(SRC) -include zephyr/kernel.h -o $@ sim.c $(SRC)/remap.c $(SRC)/sc26re_cfg.c -lm
