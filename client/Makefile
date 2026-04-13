CXX = g++
CXXFLAGS = -O2 -std=c++17 -pthread
LIBS = `sdl2-config --cflags --libs` -lSDL2_image

all:
	$(CXX) main.cpp -o cdu $(CXXFLAGS) $(LIBS)