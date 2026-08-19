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
PROF_SRCS  := test_profile.c ../App/Src/ocpp_profile.c

BINS := test_pilot test_json test_time test_ws test_profile

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

test_profile: $(PROF_SRCS)
	$(CC) $(CFLAGS) -o $@ $(PROF_SRCS) $(LDLIBS)

run: $(BINS)
	@fail=0; for t in $(BINS); do ./$$t || fail=1; echo; done; exit $$fail

# Compile-check every application source against the HAL and FreeRTOS stubs.
# _DEFAULT_SOURCE because -std=c11 sets __STRICT_ANSI__, which hides POSIX
# symbols such as S_IFCHR on glibc; newlib on the target exposes them anyway.
# This does not link a firmware image -- it catches type errors, missing
# declarations and HAL misuse without needing the CubeMX tree on the host.
APP_SRCS := $(wildcard ../App/Src/*.c)

.PHONY: check
check:
	@fail=0; for f in $(APP_SRCS); do \
	  $(CC) $(CFLAGS) -D_DEFAULT_SOURCE -DEVSE_COMPILE_CHECK=1 -fsyntax-only $$f || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then echo "compile-check: all sources OK"; fi; \
	exit $$fail

clean:
	rm -f $(BINS)
