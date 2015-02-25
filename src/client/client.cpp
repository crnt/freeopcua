/******************************************************************************
 *   Copyright (C) 2014-2014 by Sintef Raufoss Manufacturing                  *
 *   olivier.roulet@gmail.com                  *
 *                                          *
 *   This library is free software; you can redistribute it and/or modify     *
 *   it under the terms of the GNU Lesser General Public License as          *
 *   published by the Free Software Foundation; version 3 of the License.     *
 *                                          *
 *   This library is distributed in the hope that it will be useful,          *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Lesser General Public License for more details.              *
 *                                          *
 *   You should have received a copy of the GNU Lesser General Public License *
 *   along with this library; if not, write to the                  *
 *   Free Software Foundation, Inc.,                          *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.              *
 ******************************************************************************/

#include <opc/ua/client/client.h>

#include <opc/common/uri_facade.h>
#include <opc/ua/client/remote_connection.h>
#include <opc/ua/services/services.h>
#include <opc/ua/node.h>
#include <opc/ua/protocol/string_utils.h>


namespace OpcUa
{

  void KeepAliveThread::Start(Services::SharedPtr server, Node node, Duration period)
  {
    Server = server;
    NodeToRead = node;
    Period = period;
    Running = true;
    StopRequest = false;
    Thread = std::thread([this] { this->Run(); });
  }


  void KeepAliveThread::Run()
  {
    if (Debug)  { std::cout << "KeepAliveThread | Starting." << std::endl; }
    while ( ! StopRequest )
    {
      if (Debug)  { std::cout << "KeepAliveThread | Sleeping for: " << (int64_t) ( Period * 0.7 )<< std::endl; }
      std::unique_lock<std::mutex> lock(Mutex);
      std::cv_status status = Condition.wait_for(lock, std::chrono::milliseconds( (int64_t) ( Period * 0.7) )); 
      if (status == std::cv_status::no_timeout ) 
      {
        break;
      }
      if (Debug)  { std::cout << "KeepAliveThread | renewing secure channel " << std::endl; }
      OpenSecureChannelParameters params;
      params.ClientProtocolVersion = 0;
      params.RequestType = STR_RENEW;
      params.SecurityMode = MSM_NONE;
      params.ClientNonce = std::vector<uint8_t>(1, 0);
      params.RequestLifeTime = Period;
      OpenSecureChannelResponse response = Server->OpenSecureChannel(params);
      if ( (response.ChannelSecurityToken.RevisedLifetime < Period) && (response.ChannelSecurityToken.RevisedLifetime > 0) )
      {
        Period = response.ChannelSecurityToken.RevisedLifetime;
      }

      if (Debug)  { std::cout << "KeepAliveThread | read a variable from address space to keep session open " << std::endl; }
      NodeToRead.GetValue();
    }
    Running = false;
  }

  void KeepAliveThread::Stop()
  {
    if (Debug)  { std::cout << "KeepAliveThread | Stopping." << std::endl; }
    StopRequest = true;
    Condition.notify_all();
  }

  void KeepAliveThread::Join()
  {
    if (Debug)  { std::cout << "KeepAliveThread | Joining." << std::endl; }
    if ( ! Running )
    {
      if (Debug)  { std::cout << "KeepAliveThread | Thread was not running..." << std::endl; }
      return;
    }
    Thread.join();
    if (Debug)  { std::cout << "KeepAliveThread | Join successfull." << std::endl; }
  }

  std::vector<EndpointDescription> UaClient::GetServerEndpoints(const std::string& endpoint)
  {
    const Common::Uri serverUri(endpoint);
    OpcUa::IOChannel::SharedPtr channel = OpcUa::Connect(serverUri.Host(), serverUri.Port());
    
    OpcUa::SecureConnectionParams params;
    params.EndpointUrl = endpoint;
    params.SecurePolicy = "http://opcfoundation.org/UA/SecurityPolicy#None";

    Server = OpcUa::CreateBinaryClient(channel, params, Debug);

    OpenSecureChannel();
    std::vector<EndpointDescription> endpoints = UaClient::GetServerEndpoints();
    CloseSecureChannel();

    Server.reset(); //close channel

    return endpoints;
  }

  std::vector<EndpointDescription> UaClient::GetServerEndpoints()
  {
    EndpointsFilter filter;
    filter.EndpointURL = Endpoint.EndpointURL;
    filter.ProfileUries.push_back("http://opcfoundation.org/UA-Profile/Transport/uatcp-uasc-uabinary");
    filter.LocaleIDs.push_back("http://opcfoundation.org/UA-Profile/Transport/uatcp-uasc-uabinary");
    std::vector<EndpointDescription> endpoints =  Server->Endpoints()->GetEndpoints(filter);
    
    return endpoints;
  }

  EndpointDescription UaClient::SelectEndpoint(const std::string& endpoint)
  {
    std::vector<EndpointDescription> endpoints = GetServerEndpoints(endpoint);
    if (Debug)  { std::cout << "UaClient | Going through server endpoints and selected one we support" << std::endl; }
    for ( EndpointDescription ed : endpoints)
    {
      if (Debug)  { std::cout << "UaClient | Examining endpoint: " << ed.EndpointURL << " with security: " << ed.SecurityPolicyURI <<  std::endl; }
      if ( ed.SecurityPolicyURI == "http://opcfoundation.org/UA/SecurityPolicy#None")
      {
        if (Debug)  { std::cout << "UaClient | Security policy is OK, now looking at user token" <<  std::endl; }
        if (ed.UserIdentifyTokens.empty() )
        {
          if (Debug)  { std::cout << "UaClient | Server does not use user token, OK" <<  std::endl; }
          return ed;
        }
        for (  UserTokenPolicy token : ed.UserIdentifyTokens)
        {
          if (token.TokenType == UserIdentifyTokenType::ANONYMOUS )
          {
            if (Debug)  { std::cout << "UaClient | Endpoint selected " <<  std::endl; }
            return ed;
          }
        }
      }
    }
    throw std::runtime_error("No supported endpoints found on server");
  }

  void UaClient::Connect(const std::string& endpoint)
  {
    EndpointDescription endpointdesc = SelectEndpoint(endpoint);
    endpointdesc.EndpointURL = endpoint; //force the use of the enpoint the user wants, seems like servers often send wrong hostname
    Connect(endpointdesc);
  }
   
  void UaClient::Connect(const EndpointDescription& endpoint)
  {
    Endpoint = endpoint;
    const Common::Uri serverUri(Endpoint.EndpointURL);
    OpcUa::IOChannel::SharedPtr channel = OpcUa::Connect(serverUri.Host(), serverUri.Port());

    OpcUa::SecureConnectionParams params;
    params.EndpointUrl = Endpoint.EndpointURL;
    params.SecurePolicy = "http://opcfoundation.org/UA/SecurityPolicy#None";

    Server = OpcUa::CreateBinaryClient(channel, params, Debug);

    OpenSecureChannel();


    if (Debug)  { std::cout << "UaClient | Creating session " <<  std::endl; }
    OpcUa::RemoteSessionParameters session;
    session.ClientDescription.URI = ApplicationUri;
    session.ClientDescription.ProductURI = ProductUri;
    session.ClientDescription.Name = LocalizedText(SessionName);
    session.ClientDescription.Type = OpcUa::ApplicationType::CLIENT;
    session.SessionName = SessionName;
    session.EndpointURL = endpoint.EndpointURL;
    session.Timeout = DefaultTimeout;
    session.ServerURI = endpoint.ServerDescription.URI;

    CreateSessionResponse response = Server->CreateSession(session);
    CheckStatusCode(response.Header.ServiceResult);
    ActivateSessionResponse aresponse = Server->ActivateSession();
    CheckStatusCode(aresponse.Header.ServiceResult);

    if (response.Session.RevisedSessionTimeout > 0 && response.Session.RevisedSessionTimeout < DefaultTimeout  )
    {
      DefaultTimeout = response.Session.RevisedSessionTimeout;
    }
    KeepAlive.Start(Server, Node(Server, ObjectID::Server_ServerStatus_State), DefaultTimeout);
  }

  void UaClient::OpenSecureChannel()
  {
    OpenSecureChannelParameters channelparams;
    channelparams.ClientProtocolVersion = 0;
    channelparams.RequestType = STR_ISSUE;
    channelparams.SecurityMode = MSM_NONE;
    channelparams.ClientNonce = std::vector<uint8_t>(1, 0);
    channelparams.RequestLifeTime = DefaultTimeout;
    const OpenSecureChannelResponse& response = Server->OpenSecureChannel(channelparams);

    CheckStatusCode(response.Header.ServiceResult);
    
    SecureChannelId = response.ChannelSecurityToken.SecureChannelID;
    if ( response.ChannelSecurityToken.RevisedLifetime > 0 )
    {
      DefaultTimeout = response.ChannelSecurityToken.RevisedLifetime;
    }
  }

  void UaClient::CloseSecureChannel()
  {
      Server->CloseSecureChannel(SecureChannelId);
  }

  UaClient::~UaClient()
  {
    Disconnect();//Do not leave any thread or connectino running
  } 

  void UaClient::Disconnect()
  {
    KeepAlive.Stop();
    KeepAlive.Join();

    if (  Server ) 
    {
      CloseSessionResponse response = Server->CloseSession();
      if (Debug) { std::cout << "CloseSession response is " << ToString(response.Header.ServiceResult) << std::endl; }
      CloseSecureChannel();
    }
    Server.reset(); //FIXME: check if we still need this
  }

  std::vector<std::string>  UaClient::GetServerNamespaces()
  {
    if ( ! Server ) { throw std::runtime_error("Not connected");}
    Node namespacearray(Server, ObjectID::Server_NamespaceArray);
    return namespacearray.GetValue().As<std::vector<std::string>>();;
  }

  uint32_t UaClient::GetNamespaceIndex(std::string uri)
  {
    if ( ! Server ) { throw std::runtime_error("Not connected");}
    Node namespacearray(Server, ObjectID::Server_NamespaceArray);
    std::vector<std::string> uris = namespacearray.GetValue().As<std::vector<std::string>>();;
    for ( uint32_t i=0; i<uris.size(); ++i)
    {
      if (uris[i] == uri )
      {
        return i;
      }
    }
    throw(std::runtime_error("Error namespace uri does not exists in server")); 
    //return -1;
  }


  Node UaClient::GetNode(const std::string& nodeId) const
  {
    return Node(Server, ToNodeID(nodeId));
  }

  Node UaClient::GetNode(const NodeID& nodeId) const
  {
    if ( ! Server ) { throw std::runtime_error("Not connected");}
    return Node(Server, nodeId);
  }

  Node UaClient::GetRootNode() const
  {
    if ( ! Server ) { throw std::runtime_error("Not connected");}
    return Node(Server, OpcUa::ObjectID::RootFolder);
  }

  Node UaClient::GetObjectsNode() const
  {
    if ( ! Server ) { throw std::runtime_error("Not connected");}
    return Node(Server, OpcUa::ObjectID::ObjectsFolder);
  }

  Node UaClient::GetServerNode() const
  {
    if ( ! Server ) { throw std::runtime_error("Not connected");}
    return Node(Server, OpcUa::ObjectID::Server);
  }

  std::unique_ptr<Subscription> UaClient::CreateSubscription(unsigned int period, SubscriptionHandler& callback)
  {
    SubscriptionParameters params;
    params.RequestedPublishingInterval = period;

    return std::unique_ptr<Subscription>(new Subscription (Server, params, callback, Debug));
  }

} // namespace OpcUa

