// ---------------------------------------------------------------------
//
// Copyright (C) 2010 - 2017 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------


// check serialization for TableHandler

#include <deal.II/base/table_handler.h>

#include <boost/serialization/vector.hpp>

#include "serialization.h"

namespace dealii
{
  bool
  compare(const TableHandler &t1, const TableHandler &t2)
  {
    std::ostringstream o1, o2;
    t1.write_tex(o1);
    t1.write_text(o1);
    t2.write_tex(o2);
    t2.write_text(o2);

    return (o1.str() == o2.str());
  }
} // namespace dealii


void
test()
{
  TableHandler t1, t2;
  std::string  keys[4] = {"key1", "key2", "key3", "key4"};


  for (unsigned int i = 0; i < 10; ++i)
    {
      t1.add_value(keys[(0 + i) % 4], i);
      t1.add_value(keys[(1 + i) % 4], sqrt(i));
      t1.add_value(keys[(2 + i) % 4], 'a' + i);
      t1.add_value(keys[(3 + i) % 4], std::string("abc-") + "0123456789"[i]);
    }
  t1.set_tex_table_caption("This is a caption text with \\LaTeX{} symbols");

  verify(t1, t2);
}


int
main()
{
  std::ofstream logfile("output");
  deallog << std::setprecision(3);
  deallog.attach(logfile);

  test();

  deallog << "OK" << std::endl;
}
