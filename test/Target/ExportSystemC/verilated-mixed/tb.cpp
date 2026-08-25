#include "Top.h"

#include <cassert>
#include <systemc.h>

int sc_main(int, char **) {
  sc_signal<sc_uint<32>> x;
  sc_signal<sc_uint<32>> y;
  Top top("top");
  top.x(x);
  top.y(y);

  x.write(41);
  sc_start(1, SC_NS);
  assert(y.read() == 42);

  x.write(99);
  sc_start(1, SC_NS);
  assert(y.read() == 100);
  return 0;
}
