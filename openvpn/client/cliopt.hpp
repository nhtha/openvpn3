#ifndef OPENVPN_CLIENT_CLIOPT_H
#define OPENVPN_CLIENT_CLIOPT_H

#include <string>

#include <openvpn/common/platform.hpp>
#include <openvpn/common/options.hpp>
#include <openvpn/frame/frame_init.hpp>
#include <openvpn/pki/epkibase.hpp>

#include <openvpn/transport/socket_protect.hpp>
#include <openvpn/transport/client/udpcli.hpp>
#include <openvpn/transport/client/tcpcli.hpp>

#include <openvpn/client/cliproto.hpp>

#if defined(USE_TUN_BUILDER)
#include <openvpn/tun/builder/client.hpp>
#elif defined(OPENVPN_PLATFORM_LINUX) && !defined(OPENVPN_FORCE_TUN_NULL)
#include <openvpn/tun/linux/client/tuncli.hpp>
#elif defined(OPENVPN_PLATFORM_MAC) && !defined(OPENVPN_FORCE_TUN_NULL)
#include <openvpn/tun/mac/client/tuncli.hpp>
#else
#include <openvpn/tun/client/tunnull.hpp>
#endif

#ifdef USE_OPENSSL
#include <openvpn/openssl/crypto/api.hpp>
#include <openvpn/openssl/ssl/sslctx.hpp>
#include <openvpn/openssl/util/rand.hpp>
#endif

#ifdef USE_APPLE_SSL
#include <openvpn/applecrypto/crypto/api.hpp>
#include <openvpn/applecrypto/ssl/sslctx.hpp>
#include <openvpn/applecrypto/util/rand.hpp>
#endif

#ifdef USE_POLARSSL
#include <openvpn/polarssl/crypto/api.hpp>
#include <openvpn/polarssl/ssl/sslctx.hpp>
#include <openvpn/polarssl/util/rand.hpp>
#endif

using namespace openvpn;

namespace openvpn {

  class ClientOptions : public RC<thread_unsafe_refcount>
  {
  public:
    typedef boost::intrusive_ptr<ClientOptions> Ptr;

#if defined(USE_POLARSSL)
    typedef PolarSSLCryptoAPI ClientCryptoAPI;
    typedef PolarSSLContext ClientSSLAPI;
    typedef PolarSSLRandom RandomAPI;
#elif defined(USE_APPLE_SSL)
    typedef AppleCryptoAPI ClientCryptoAPI;
    typedef AppleSSLContext ClientSSLAPI;
    typedef AppleRandom RandomAPI;
#elif defined(USE_OPENSSL)
    typedef OpenSSLCryptoAPI ClientCryptoAPI;
    typedef OpenSSLContext ClientSSLAPI;
    typedef OpenSSLRandom RandomAPI;
#else
#error no SSL library defined
#endif
    typedef ClientProto::Session<RandomAPI, ClientCryptoAPI, ClientSSLAPI> Client;

    struct Config {
      Config()
      {
	external_pki = NULL;
	socket_protect = NULL;
	conn_timeout = 0;
#if defined(USE_TUN_BUILDER)
	builder = NULL;
#endif
      }

      std::string server_override;
      Protocol proto_override;
      int conn_timeout;
      SessionStats::Ptr cli_stats;
      ClientEvent::Queue::Ptr cli_events;

      // callbacks -- must remain in scope for lifetime of ClientOptions object
      ExternalPKIBase* external_pki;
      SocketProtect* socket_protect;
#if defined(USE_TUN_BUILDER)
      TunBuilderBase* builder;
#endif
    };

    ClientOptions(const OptionList& opt,   // only needs to remain in scope for duration of constructor call
		  const Config& config)
      : session_iteration(0),
	socket_protect(config.socket_protect),
	cli_stats(config.cli_stats),
	cli_events(config.cli_events),
	server_poll_timeout_(10),
	server_override(config.server_override),
	proto_override(config.proto_override),
	conn_timeout_(config.conn_timeout)
    {
      // initialize RNG/PRNG
      rng.reset(new RandomAPI());
      prng.reset(new PRNG<RandomAPI, ClientCryptoAPI>("SHA1", rng, 16));

      // frame
      frame = frame_init();

      // client config
      ClientSSLAPI::Config cc;
      cc.set_external_pki_callback(config.external_pki);
      cc.frame = frame;
#ifdef OPENVPN_SSL_DEBUG
      cc.enable_debug();
#endif
#if defined(USE_POLARSSL)
      cc.rng = rng;
#endif
      cc.load(opt);
      if (!cc.mode.is_client())
	throw option_error("only client configuration supported");

      // client ProtoContext config
      cp.reset(new Client::ProtoConfig);
      cp->load(opt);
      cp->ssl_ctx.reset(new ClientSSLAPI(cc));
      cp->frame = frame;
      cp->now = &now_;
      cp->rng = rng;
      cp->prng = prng;

      // load remote list
      remote_list.reset(new RemoteList(opt));
      if (!remote_list->size())
	throw option_error("no remote option specified");

      // initialize transport layer
      if (cp->layer != Layer(Layer::OSI_LAYER_3))
	throw option_error("only layer 3 currently supported");

      // init transport config
      const std::string session_name = load_transport_config();

      // initialize tun/tap
#if defined(USE_TUN_BUILDER)
      TunBuilderClient::ClientConfig::Ptr tunconf = TunBuilderClient::ClientConfig::new_obj();
      tunconf->builder = config.builder;
      tunconf->session_name = session_name;
      tunconf->frame = frame;
      tunconf->stats = cli_stats;
#elif defined(OPENVPN_PLATFORM_LINUX) && !defined(OPENVPN_FORCE_TUN_NULL)
      TunLinux::ClientConfig::Ptr tunconf = TunLinux::ClientConfig::new_obj();
      tunconf->layer = cp->layer;
      tunconf->frame = frame;
      tunconf->stats = cli_stats;
#elif defined(OPENVPN_PLATFORM_MAC) && !defined(OPENVPN_FORCE_TUN_NULL)
      TunMac::ClientConfig::Ptr tunconf = TunMac::ClientConfig::new_obj();
      tunconf->layer = cp->layer;
      tunconf->frame = frame;
      tunconf->stats = cli_stats;
#else
      TunNull::ClientConfig::Ptr tunconf = TunNull::ClientConfig::new_obj();
      tunconf->frame = frame;
      tunconf->stats = cli_stats;
#endif
      tun_factory = tunconf;

      // server-poll-timeout
      {
	const Option *o = opt.get_ptr("server-poll-timeout");
	if (o)
	  server_poll_timeout_ = types<unsigned int>::parse(o->get(1));
      }

      // userlocked username
      {
	const Option* o = opt.get_ptr("USERNAME");
	if (o)
	  userlocked_username = o->get(1);
      }
    }

    void next()
    {
      ++session_iteration;
      load_transport_config();
    }

    Client::Config::Ptr client_config()
    {
      Client::Config::Ptr cli_config = new Client::Config;
      cli_config->proto_context_config = cp;
      cli_config->transport_factory = transport_factory;
      cli_config->tun_factory = tun_factory;
      cli_config->cli_stats = cli_stats;
      cli_config->cli_events = cli_events;
      cli_config->creds = creds;
      return cli_config;
    }

    bool need_creds() const
    {
      return !cp->autologin;
    }

    void submit_creds(const ClientCreds::Ptr& creds_arg)
    {
      // if no username is defined in creds and userlocked_username is defined
      // in profile, set the creds username to be the userlocked_username
      if (creds_arg && !creds_arg->username_defined() && !userlocked_username.empty())
	creds_arg->set_username(userlocked_username);

      creds = creds_arg;
    }

    Time::Duration server_poll_timeout() const
    {
      return Time::Duration::seconds(server_poll_timeout_);
    }

    SessionStats& stats() { return *cli_stats; }
    ClientEvent::Queue& events() { return *cli_events; }

    int conn_timeout() const { return conn_timeout_; }

    void update_now()
    {
      now_.update();
    }

  private:
    std::string load_transport_config()
    {
      // initialize remote item with current element
      const RemoteList::Item rli = remote_list->get(session_iteration, server_override, proto_override);
      cp->remote_adjust(rli);

      // initialize transport factory
      if (rli.transport_protocol.is_udp())
	{
	  UDPTransport::ClientConfig::Ptr udpconf = UDPTransport::ClientConfig::new_obj();
	  udpconf->server_host = rli.server_host;
	  udpconf->server_port = rli.server_port;
	  udpconf->frame = frame;
	  udpconf->stats = cli_stats;
	  udpconf->socket_protect = socket_protect;
	  transport_factory = udpconf;
	}
      else if (rli.transport_protocol.is_tcp())
	{
	  TCPTransport::ClientConfig::Ptr tcpconf = TCPTransport::ClientConfig::new_obj();
	  tcpconf->server_host = rli.server_host;
	  tcpconf->server_port = rli.server_port;
	  tcpconf->frame = frame;
	  tcpconf->stats = cli_stats;
	  tcpconf->socket_protect = socket_protect;
	  transport_factory = tcpconf;
	}
      else
	throw option_error("unknown transport protocol");

      return rli.server_host;
    }

    unsigned int session_iteration;

    Time now_; // current time
    RandomAPI::Ptr rng;
    PRNG<RandomAPI, ClientCryptoAPI>::Ptr prng;
    Frame::Ptr frame;
    ClientSSLAPI::Config cc;
    Client::ProtoConfig::Ptr cp;
    RemoteList::Ptr remote_list;
    TransportClientFactory::Ptr transport_factory;
    TunClientFactory::Ptr tun_factory;
    SocketProtect* socket_protect;
    SessionStats::Ptr cli_stats;
    ClientEvent::Queue::Ptr cli_events;
    ClientCreds::Ptr creds;
    unsigned int server_poll_timeout_;
    std::string server_override;
    Protocol proto_override;
    int conn_timeout_;
    std::string userlocked_username;
  };
}

#endif
