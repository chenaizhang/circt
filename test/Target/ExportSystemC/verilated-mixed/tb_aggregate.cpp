#include "AggregateTop.h"

#include <cassert>
#include <systemc.h>

int sc_main(int, char **) {
  sc_signal<bool> cfgEnable;
  sc_signal<sc_uint<3>> cfgMode;
  sc_signal<sc_uint<8>> pixel0;
  sc_signal<sc_uint<8>> pixel1;
  sc_signal<sc_uint<3>> result;

  AggregateTop top("top");
  top.cfg_enable(cfgEnable);
  top.cfg_mode(cfgMode);
  top.pixels_0(pixel0);
  top.pixels_1(pixel1);
  top.result(result);

  cfgEnable.write(true);
  cfgMode.write(5);
  pixel0.write(0x12);
  pixel1.write(0x34);
  sc_start(1, SC_NS);
  assert(result.read() == 5);
  return 0;
}
