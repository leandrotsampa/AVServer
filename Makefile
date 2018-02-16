CC = $(CROSS_COMPILER)gcc
INCLUDES = -I. -Iinclude
LIBS = -Llib -L../lib/.libs \
	   -lc -lpthread -lrt -lstdc++ -ldl -lfuse3 -ljpeg6b -lhi_common -lhi_sample_common \
	   -lpng -lhigo -lhigoadp -lhi_msp -lhi_resampler -lz -lfreetype -lhi_subtitle -lhi_so -lhi_ttx -lhi_cc
OUT = AVServer

default: $(OUT)

$(OUT): clean
	$(CC) -Wall -D_FILE_OFFSET_BITS=64 $(INCLUDES) $(LIBS) -o $(OUT) $(filter-out test_av.c, $(wildcard *.c))

clean:
	rm -f $(OUT)