# Host-side unit tests for the pure EVSE logic (no HAL, no FreeRTOS).
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g \
           -DEVSE_HOST_TEST=1 \
           -I../App/Inc -Istubs
LDLIBS  := -lm

SRCS := test_pilot.c ../App/Src/cp_pilot.c ../App/Src/proximity.c stubs/stubs.c

.PHONY: all run clean
all: test_pilot

test_pilot: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

run: test_pilot
	./test_pilot

clean:
	rm -f test_pilot
