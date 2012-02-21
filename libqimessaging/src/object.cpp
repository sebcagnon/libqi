/*
** Author(s):
**  - Cedric GESTES <gestes@aldebaran-robotics.com>
**
** Copyright (C) 2012 Aldebaran Robotics
*/

#include <iostream>
#include <qimessaging/object.hpp>

namespace qi {

  Object::Object()
    : _meta(new MetaObject())
  {
    advertiseMethod("__metaobject", this, &Object::metaObject);
  }

  Object::~Object() {
    delete _meta;
  }

  MetaObject &Object::metaObject() {
    return *_meta;
  }

  MetaMethod::MetaMethod(const std::string &name, const std::string &sig, const qi::Functor *functor)
    : _name(name),
      _signature(sig),
      _functor(functor)
  {
  }

  unsigned int Object::xAdvertiseService(const std::string &name, const std::string& signature, const qi::Functor* functor) {
    MetaMethod mm(name, signature, functor);
    _meta->_methods[name] = mm;
    unsigned int idx = _meta->_methodsNumber++;
    _meta->_methodsTable.push_back(&(_meta->_methods[name]));
    return idx;
  }

  void Object::metaCall(const std::string &method, const std::string &sig, DataStream &in, DataStream &out)
  {
    MetaMethod &mm = metaObject()._methods[method];
    if (mm._functor)
      mm._functor->call(in, out);
    //TODO: log error
  }

  void Object::metaCall(unsigned int method, const std::string &sig, DataStream &in, DataStream &out)
  {
    MetaMethod *mm = metaObject()._methodsTable[method];
    if (mm->_functor)
      mm->_functor->call(in, out);
  }

  qi::DataStream &operator<<(qi::DataStream &stream, const MetaMethod &meta) {
    stream << meta._name;
    stream << meta._signature;
    return stream;
  }

  qi::DataStream &operator>>(qi::DataStream &stream, MetaMethod &meta) {
    stream >> meta._name;
    stream >> meta._signature;
    return stream;
  }

  qi::DataStream &operator<<(qi::DataStream &stream, const MetaObject &meta) {
    stream << meta._methods;
    return stream;
  }

  qi::DataStream &operator>>(qi::DataStream &stream, MetaObject &meta) {
    stream >> meta._methods;
    return stream;
  }


};
