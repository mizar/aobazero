THE_OS := $(shell uname -s)
PROGRAM = aobaz

# isnan() does not work with -ffast-math

default:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=gcc CXX=g++ \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -pipe -O3 -g -march=native -std=c++14 -DNDEBUG'  \
		LDFLAGS='$(LDFLAGS) -g' \
		$(PROGRAM)

debug:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=gcc CXX=g++ \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -pipe -Og -g -std=c++14' \
		LDFLAGS='$(LDFLAGS) -g' \
		$(PROGRAM)

clang:
	@echo "Detected OS: ${THE_OS}"
	$(MAKE) CC=clang-5.0 CXX=clang++-5.0 \
		CXXFLAGS='$(CXXFLAGS) -Wall -Wextra -Wno-missing-braces -O3 -flto -march=native -std=c++14 -DNDEBUG' \
		LDFLAGS='$(LDFLAGS) -flto -fuse-linker-plugin' \
		$(PROGRAM)

DYNAMIC_LIBS = -lpthread -lz -llzma
LIBS =

ifeq ($(THE_OS),Linux)
# for Linux with OpenBLAS
	CXXFLAGS += -I/usr/include/openblas
#	CXXFLAGS += -I/usr/include/openblas -I./Eigen
	DYNAMIC_LIBS += -lboost_system -lboost_filesystem -lboost_program_options
	DYNAMIC_LIBS += -lopenblas
	DYNAMIC_LIBS += -lOpenCL
	DYNAMIC_LIBS += -lrt
endif
ifeq ($(THE_OS),Darwin)
# for macOS (comment out the Linux part)
	LIBS += -framework Accelerate
	LIBS += -framework OpenCL
#	CXXFLAGS += -I./Eigen
	CXXFLAGS += -I/System/Library/Frameworks/Accelerate.framework/Versions/Current/Headers
	DYNAMIC_LIBS += -lboost_system -lboost_filesystem -lboost_program_options
	DYNAMIC_LIBS += -lrt
endif
ifeq ($(MSYSTEM),MINGW64)
# for Windows (MSYS2 MINGW64 environment)
#  pacman -S --noconfirm mingw-w64-x86_64-boost
#  pacman -S --noconfirm mingw-w64-x86_64-openblas
#  pacman -S --noconfirm mingw-w64-x86_64-opencl-headers
#  pacman -S --noconfirm mingw-w64-x86_64-opencl-icd-git
	CXXFLAGS += -I$(shell cygpath -aw /mingw64/include/boost)
	CXXFLAGS += -I$(shell cygpath -aw /mingw64/include/CL)
	CXXFLAGS += -I$(shell cygpath -aw /mingw64/include/OpenBLAS)
	CXXFLAGS += -DUSE_WINAPI -DWIN32 -DBOOST_USE_WINDOWS_H -DWIN32_LEAN_AND_MEAN
	DYNAMIC_LIBS += -lboost_system-mt -lboost_filesystem-mt -lboost_program_options-mt
	DYNAMIC_LIBS += -lws2_32 -lwsock32
	DYNAMIC_LIBS += -lopenblas
#	DYNAMIC_LIBS += -lOpenCL
	LDFLAGS += -static -Wl,--stack,25000000
endif

# for MKL instead of OpenBLAS
#DYNAMIC_LIBS += -lmkl_rt
#CXXFLAGS += -I/opt/intel/mkl/include
#LDFLAGS  += -L/opt/intel/mkl/lib/intel64/

# CPU_ONLY = 1 for cpu version.
CPU_ONLY = 1

ifeq ($(CPU_ONLY), 1)
CXXFLAGS += -DUSE_CPU_ONLY
endif
CXXFLAGS += -I. -I../common -DMINIMUM -DTLP -DCSA_LAN -DUSI -DYSS_ZERO -DNO_LOGGING
CXXFLAGS += -fopenmp
LDFLAGS  += -fopenmp

CPPFLAGS += -MD -MP

bases  = Network Leela Utils Zobrist GTP Random SMP OpenCL OpenCLScheduler NNCache Tuner CPUPipe
ifneq ($(CPU_ONLY), 1)
bases += iobase xzi err shogibase osi nnet nnet-cpu nnet-ocl nnet-srv opencli nnet-ipc

endif
bases += $(addprefix bona/, data main io proce utility ini attack book makemove unmake time csa valid bitop iterate searchr search quiesrch evaluate swap hash root next movgenex genevasn gencap gennocap gendrop mate1ply rand learn1 learn2 evaldiff problem ponder thread sckt debug mate3 genchk phash dfpn dfpnhash ysszero yss_net)

sources = $(addsuffix .cpp, $(bases))
objects = $(addsuffix .o,   $(bases))
deps    = $(addsuffix .d,   $(bases))

-include $(deps)

%.o : %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

%.o : ../common/%.cpp
	$(CXX) $(CXXFLAGS) -DUSE_OPENBLAS -DUSE_OPENCL_AOBA $(CPPFLAGS) -c -o $@ $<

$(PROGRAM): $(objects)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS) $(DYNAMIC_LIBS)

clean:
	-$(RM) $(PROGRAM) $(objects) $(deps)

.PHONY: clean default debug clang
