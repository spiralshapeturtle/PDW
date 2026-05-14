
typedef struct {
   unsigned int  irq;        // IRQ number for COM port
   unsigned int  com_port;   // I/O address for COM port
} SLICER_IN_STR;

typedef struct {
   unsigned short *freqdata; // each freqdata entry is 16 bits
   unsigned char *linedata;  // each linedata entry is 8 bits
   volatile unsigned long *cpstn;  // FIX [L3]: volatile — consumer (decode.cpp) mag waarde niet hoisten
   long bufsize;             // size of freqdata and linedata buffers
} SLICER_OUT_STR;

