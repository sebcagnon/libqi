/*
*  Author(s):
*  - Cedric Gestes <gestes@aldebaran-robotics.com>
*  - Chris  Kilner <ckilner@aldebaran-robotics.com>
*
*  Copyright (C) 2010-2012 Aldebaran Robotics
*/

#include <qimessaging/details/value.hpp>
#include <cassert>

namespace qi {
  namespace detail {

  void Value::clear() {
    switch(type) {
    case String:
      delete data.str;
      break;
    case List:
      delete data.list;
      break;
    case Map:
      delete data.map;
      break;
    default:
      break;
    }
    data.ptr = 0;
    type = Invalid;
  }
  
  Value::~Value()
  {
    clear();
  }
  }
}

