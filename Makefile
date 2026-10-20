ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -Wno-narrowing
DEPS = pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon
SRC = $(wildcard src/*.cpp)

all: hyprzoom.so

hyprzoom.so: $(SRC) $(wildcard src/*.hpp)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $(SRC) -o $@ `pkg-config --cflags $(DEPS)`

clean:
	rm -f hyprzoom.so

.PHONY: all clean
