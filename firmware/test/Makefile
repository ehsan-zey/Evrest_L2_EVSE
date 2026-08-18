# Host-side unit tests for the pure EVSE logic (no HAL, no FreeRTOS).
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g \
           -DEVSE_HOST_TEST=1 \
           -I../App/Inc -Istubs
LDLIBS  := -lm

PILOT_SRCS := test_pilot.c ../App/Src/cp_pilot.c ../App/Src/proximity.c stubs/stubs.c
JSON_SRCS  := test_json.c ../App/Src/json.c
TIME_SRCS  := test_time.c ../App/Src/rtc_time.c
WS_SRCS    := test_ws.c ../App/Src/ws_client.c

BINS := test_pilot test_json test_time test_ws

.PHONY: all run clean
all: $(BINS)

test_pilot: $(PILOT_SRCS)
	$(CC) $(CFLAGS) -o $@ $(PILOT_SRCS) $(LDLIBS)

test_json: $(JSON_SRCS)
	$(CC) $(CFLAGS) -o $@ $(JSON_SRCS) $(LDLIBS)

test_time: $(TIME_SRCS)
	$(CC) $(CFLAGS) -o $@ $(TIME_SRCS) $(LDLIBS)

test_ws: $(WS_SRCS)
	$(CC) $(CFLAGS) -o $@ $(WS_SRCS) $(LDLIBS)

run: $(BINS)
	@fail=0; for t in $(BINS); do ./$$t || fail=1; echo; done; exit $$fail

clean:
	rm -f $(BINS)
