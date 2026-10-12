CONFIG ?= Debug
GENERATOR ?= Ninja
BUILD ?= build/$(CONFIG)
JOBS ?= 4
.PHONY: all configure test run
all: configure
	cmake --build "$(BUILD)" --config "$(CONFIG)" --parallel $(JOBS)
configure:
	cmake -S . -B "$(BUILD)" -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(CONFIG)
test: all
	ctest --test-dir "$(BUILD)" -C $(CONFIG) --output-on-failure
run: all
	"$(BUILD)/xscarlet" $(if $(FILE),"$(FILE)")
