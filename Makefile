CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lrabbitmq -lcjson

TARGET  = rmq_bridge
SRC     = rmq_bridge.c

# Static build output from build_static.sh
STATIC_BUILD_DIR = _static_build/install

.PHONY: all clean static static-clean

# --- Dynamic (system libraries) ------------------------------------
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# --- Static (portable, Alpine) ------------------------------------
# Requires: ./build_static.sh to have been run first, OR run `make static`
# which invokes it automatically.
static:
	@if [ ! -d "$(STATIC_BUILD_DIR)" ]; then \
		echo "==> Running build_static.sh to build static libraries..."; \
		./build_static.sh; \
	else \
		echo "==> Static libs found, compiling rmq_bridge..."; \
		$(CC) $(CFLAGS) -static -no-pie \
			-I$(STATIC_BUILD_DIR)/include \
			-o $(TARGET) $(SRC) \
			-L$(STATIC_BUILD_DIR)/lib -L$(STATIC_BUILD_DIR)/lib64 \
			-lrabbitmq -lcjson -lpthread; \
		strip $(TARGET); \
		echo "==> Static binary: ./$(TARGET)"; \
	fi

# --- Clean ---------------------------------------------------------
clean:
	rm -f $(TARGET)

static-clean:
	rm -rf _static_build
	rm -f $(TARGET)
