CXX = g++
CXXFLAGS = -Wall -O2 -std=c++17 -D_GLIBCXX_USE_CXX11_ABI=1 \
           -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS \
           -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 \
           -DOS_LINUX -DHAVE_USB -DHAVE_LIBUSB10

# Directories
SRC_DIR = src
INC_DIR = include

# MIDAS
MIDAS_INC = $(MIDASSYS)/include
MIDAS_MXML_INC = $(MIDASSYS)/mxml
MIDAS_MJSON_INC = $(MIDASSYS)/mjson
MIDAS_LIB_DIR = $(MIDASSYS)/lib
MIDAS_LIB = $(MIDAS_LIB_DIR)/libmidas.a
MFE_LIB = $(MIDAS_LIB_DIR)/mfe.o

# DRS4 pre-compiled objects (from original DRS4 build)
DRS_C_OBJ = musbstd.o mxml.o strlcpy.o
DRS_CPP_OBJ = DRS.o averager.o

# Libraries
LIBS = -L$(MIDAS_LIB_DIR) -lmidas $(MIDAS_LIB) $(MFE_LIB) \
       -lusb-1.0 -lpthread -lzmq -lrt

# Includes
INCLUDES = -I$(INC_DIR) -I$(MIDAS_INC) -I$(MIDAS_MXML_INC) -I$(MIDAS_MJSON_INC)

# Sources and targets
FE_SRCS = $(SRC_DIR)/drs_frontend.cxx $(SRC_DIR)/drs_frontend_class.cxx
FE_OBJS = $(FE_SRCS:$(SRC_DIR)/%.cxx=%.o)
TARGET = drs_frontend

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(FE_OBJS) $(DRS_CPP_OBJ) $(DRS_C_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(FE_OBJS) $(DRS_CPP_OBJ) $(DRS_C_OBJ) $(LIBS)

%.o: $(SRC_DIR)/%.cxx
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# DRS.o from our patched DRS.cpp (no wxMutex)
DRS.o: $(SRC_DIR)/DRS.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(FE_OBJS) DRS.o $(TARGET)