SRC_DIR = ./
SRCS = $(shell find $(SRC_DIR) -name '*.cpp')
TARGET = tinywebserver

CXX ?= g++
CXXFLAGS ?= -lpthread -lmysqlclient

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
endif

all : $(TARGET)

$(TARGET) : main.c $(SRCS)
	$(CXX) -o $(TARGET) $^ $(CXXFLAGS)

.PHONY: clean
clean:
	rm -rf $(TARGET)
