/*
** Author(s):
**  - Cedric GESTES <gestes@aldebaran-robotics.com>
**
** Copyright (C) 2010 Cedric GESTES
*/

#include <alcommon-ng/transport/zeromq/zmqclient.hpp>
#include <alcommon-ng/transport/shm/client/result_handler.hpp>
#include <sstream>
#include <boost/interprocess/streams/bufferstream.hpp>
#include <alfile/alfilesystem.h>
#include <allog.h>

namespace AL {
  namespace Transport {

    /// <summary> Constructor. </summary>
    /// <param name="serverAddress"> The server address. </param>
    ZMQClient::ZMQClient(const std::string &serverAddress)
      : Client(serverAddress),
      context(1),
      socket(context, ZMQ_REQ)
    {
      connect();
    }

    /// <summary> Connects to the server </summary>
    void ZMQClient::connect()
    {
      alsdebug << "ZMQClient::connect " << _serverAddress;
      socket.connect(_serverAddress.c_str());
    }

    /// <summary> Sends. </summary>
    /// <param name="tosend"> The data to send. </param>
    /// <param name="result"> [in,out] The result. </param>
    void ZMQClient::send(const std::string &tosend, std::string &result)
    {
      // TODO optimise this
      // Could we copy from the serialized stream without calling
      // stream.str() before sending to this method?
      //TODO: could we avoid more copy?
      zmq::message_t msg(tosend.size());
      //TODO?
      memcpy(msg.data(), tosend.data(), tosend.size());
      socket.send(msg);
      socket.recv(&msg);
      // TODO optimize this
      // boost could serialize from msg.data() and size,
      // without making a string
      result.assign((char *)msg.data(), msg.size());
    }

  }
}

