RACK_DIR ?= ../Rack-SDK

SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += $(wildcard LICENSE*) res README.md SETUP.md BUILD_AND_RUN.md docs

include $(RACK_DIR)/plugin.mk
