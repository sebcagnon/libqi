/*
** Author(s):
**  - Herve Cuche <hcuche@aldebaran-robotics.com>
**
** Copyright (C) 2012 Aldebaran Robotics
*/

#include <qimessaging/object.hpp>
#include <qimessaging/gateway.hpp>
#include <qimessaging/session.hpp>
#include <qimessaging/transport/transport_server.hpp>
#include "src/transport/network_thread.hpp"
#include "src/transport/url.hpp"
#include <boost/bind.hpp>
#include <qi/log.hpp>

static int reqid = 500;

namespace qi
{

class GatewayPrivate : public TransportServerInterface, public TransportSocketInterface
{
public:

  typedef std::vector< std::pair<qi::Message, TransportSocket *> > PendingMessageVector;
  typedef std::map<unsigned int, PendingMessageVector>             PendingMessageMap;

  typedef std::map< int, std::pair<int, TransportSocket *> >       RequestIdMap;
  typedef std::map<TransportSocket *, RequestIdMap>                ServiceRequestIdMap;

  typedef std::map<unsigned int, qi::TransportSocket *>            ServiceSocketMap;

protected:

  void handleClientRead(TransportSocket *client, qi::Message &msg);
  void handleGatewayServiceRead(TransportSocket *master, qi::Message &msg);
  void handleServiceRead(TransportSocket *service, qi::Message &msg);
  void forwardClientMessage(TransportSocket *client,
                            TransportSocket *service,
                            qi::Message &msg);
  //ServerInterface
  virtual void newConnection();

  //SocketInterface
  virtual void onReadyRead(TransportSocket *client, qi::Message &msg);
  virtual void onWriteDone(TransportSocket *client);
  virtual void onConnected(TransportSocket *client);
  virtual void onDisconnected(TransportSocket *client);


public:
  ServiceSocketMap                             _services;
  std::vector<qi::TransportSocket *>           _clients;
  std::vector<unsigned int>                    _endpoints;
  TransportServer                              _ts;
  TransportSocket                             *_tso;
  qi::Session                                 *_session;

  //for each service socket, associated if request to a client socket. 0 mean gateway
  ServiceRequestIdMap                         _serviceToClient;
  PendingMessageMap                           _pendingMessage;
}; // !GatewayPrivate

void GatewayPrivate::newConnection()
{
  TransportSocket *socket = _ts.nextPendingConnection();
  if (!socket)
    return;
  socket->setDelegate(this);
  _clients.push_back(socket);
}

void GatewayPrivate::forwardClientMessage(TransportSocket *client, TransportSocket *service, qi::Message &msg)
{
  qi::Message   servMsg(msg);
  RequestIdMap &reqIdMap = _serviceToClient[service];

  servMsg.setId(reqid++);
  reqIdMap[servMsg.id()] = std::make_pair(msg.id(), client);
  service->send(servMsg);
}

//From Client
// C.1/ new client which ask master for a service        => return gateway endpoint, enter C.2 or C.3
// C.2/ new message from client to unknown destination   => ask master, enter S.1
// C.3/ new message from client to know services         => forward, enter S.3
void GatewayPrivate::handleClientRead(TransportSocket *client, qi::Message &msg)
{
  // C.1/ We are the Master!
  // unique case: service always return gateway endpoint
  if (msg.service() == qi::Message::ServiceDirectory && msg.function() == qi::Message::Service)
  {
    qi::Message retval;
    retval.buildReplyFrom(msg);
    qi::DataStream d(retval.buffer());
    std::vector<unsigned int> tmpEndPoint;
    tmpEndPoint.push_back(msg.service());
    for (unsigned int i = 0; i < _endpoints.size(); ++i)
      tmpEndPoint.push_back(_endpoints[i]);

    d << tmpEndPoint;

    client->send(retval);
    return;
  }

  std::map<unsigned int, qi::TransportSocket*>::iterator it;
  it = _services.find(msg.service());

  //// C.3/
  if (it != _services.end())
  {
    forwardClientMessage(client, it->second, msg);
  }
  //// C.2/
  else
  {
    //request to gateway to have the endpoint
    qi::Message masterMsg;
    qi::DataStream d(masterMsg.buffer());
    d << msg.service();

    //associate the transportSoket client = 0
    //this will allow S.1 to be handle correctly
    masterMsg.setType(qi::Message::Call);
    masterMsg.setService(qi::Message::ServiceDirectory);
    masterMsg.setPath(0);
    masterMsg.setFunction(qi::Message::Service);

    RequestIdMap &reqIdMap = _serviceToClient[_tso];
    reqIdMap[masterMsg.id()] = std::make_pair(0, (qi::TransportSocket*)NULL);

    //store the pending message
    PendingMessageMap::iterator itPending;
    itPending = _pendingMessage.find(msg.service());
    if (itPending == _pendingMessage.end())
    {
      PendingMessageVector pendingMsg;
      pendingMsg.push_back(std::make_pair(msg, client));
      _pendingMessage[msg.service()] = pendingMsg;
    }
    else
    {
      PendingMessageVector &pendingMsg = itPending->second;
      pendingMsg.push_back(std::make_pair(msg, client));
    }
    _tso->send(masterMsg);

    return;
  }
}

//Only be master request
//S1 answer from master for us
void GatewayPrivate::handleGatewayServiceRead(TransportSocket *master, qi::Message &msg)
{
  ServiceRequestIdMap::iterator it;
  // get the map of request => client
  it = _serviceToClient.find(master);

  //assert it's really from master
  if (it == _serviceToClient.end())
    return;


  std::vector<std::string>      result;
  qi::DataStream                d(msg.buffer());

  d >> result;

  qi::Url url(result[1]);

  //new socket
  qi::TransportSocket *servSocket = new qi::TransportSocket();
  servSocket->setDelegate(this);
  //call connect
  servSocket->connect(url.host(), url.port(), _session->_nthd->getEventBase());

  //TODO: serviceName = endpointIt.name();
  //unsigned int serviceName = result[0];
  unsigned int serviceName = 0;
  _services[serviceName] = servSocket;
  //   go to S.2
}


//From Service
// S.1/ new message from master from us                  => create new service, enter S.2
// S.3/ new message from service                         => forward, (end)
void GatewayPrivate::handleServiceRead(TransportSocket *service, qi::Message &msg)
{
  // get the map of request => client
  ServiceRequestIdMap::iterator it;
  it = _serviceToClient.find(service);
  // should not fail
  if (it == _serviceToClient.end())
  {
    //log error
    return;
  }

  RequestIdMap &request = it->second;
  RequestIdMap::iterator itReq;

  itReq = request.find(msg.id());
  if (itReq != request.end())
  {
    if (itReq->second.second == 0)
      handleGatewayServiceRead(service, msg);
    else
    {
      // S.3
      // id should be rewritten
      qi::Message ans(msg);
      ans.setId(itReq->second.first);
      itReq->second.second->send(ans);
    }
  }
}

void GatewayPrivate::onReadyRead(TransportSocket *client, qi::Message &msg)
{
  if (std::find(_clients.begin(), _clients.end(), client) != _clients.end())
  {
    // Client
    handleClientRead(client, msg);
  }
  else
  {
    // Server
    handleServiceRead(client, msg);
  }
}

void GatewayPrivate::onWriteDone(TransportSocket *client)
{
}

// S.2/ new service connection                           => handle pending message, jump to S.3
void GatewayPrivate::onConnected(TransportSocket *service)
{
  if (service == _tso)
    return;

  unsigned int serviceId;
  ServiceSocketMap::const_iterator it;

  //TODO: optimise?  O(log(n)) instead O(n)
  for (it = _services.begin(); it != _services.end(); ++it)
  {
    if ((TransportSocket *)it->second == service)
    {
      serviceId = it->first;
      break;
    }
  }

  if (it == _services.end())
  {
    qiLogError("Gateway", "fail baby\n");
    return;
  }

  //handle pending message
  PendingMessageVector &pmv = _pendingMessage[serviceId];
  PendingMessageVector::iterator itPending;

  for (itPending = pmv.begin(); itPending != pmv.end(); ++itPending)
  {
    TransportSocket *client = itPending->second;
    qi::Message &msg = itPending->first;
    forwardClientMessage(client, service, msg);
  }
}

void GatewayPrivate::onDisconnected(TransportSocket *client)
{
}

Gateway::Gateway()
  : _p(new GatewayPrivate())
{
}

Gateway::~Gateway()
{
  delete _p;
}

void Gateway::listen(qi::Session *session, const std::string &addr)
{
  qi::Url url(addr);
  _p->_session = session;
  _p->_tso = new qi::TransportSocket();
  _p->_tso->setDelegate(_p);
  _p->_tso->connect("127.0.0.1", 5555, _p->_session->_nthd->getEventBase());
  _p->_tso->waitForConnected();
  _p->_services[qi::Message::ServiceDirectory] = _p->_tso;
  _p->_endpoints.push_back(0);
  _p->_ts.setDelegate(_p);
  _p->_ts.start(url.host(), url.port(), session->_nthd->getEventBase());
}
} // !qi
