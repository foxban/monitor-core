#include <stdio.h>
#include <stdlib.h>
#include <apr_strings.h>
#include "apr_network_io.h"
#include "apr_arch_networkio.h"

#include "apr_net.h"

#include <sys/ioctl.h>
#include <net/if.h>

#ifdef SOLARIS2
#include <sys/sockio.h>  /* for SIOCGIFADDR */
#endif

/* This function is copied directly from the 
 * apr_sockaddr_ip_get() function and modified to take a static
 * buffer instead of needing to malloc memory from a pool */
APR_DECLARE(apr_status_t) apr_sockaddr_ip_buffer_get(char *addr, int len,
                                         apr_sockaddr_t *sockaddr)
{
  if(!sockaddr || !addr || len < sockaddr->addr_str_len)
    {
      return APR_EINVAL;
    }
  /* this function doesn't malloc memory from the sockaddr pool...
   * old code...
   *addr = apr_palloc(sockaddr->pool, sockaddr->addr_str_len);
   */
    apr_inet_ntop(sockaddr->family,
                  sockaddr->ipaddr_ptr,
                  addr,
                  sockaddr->addr_str_len);
#if APR_HAVE_IPV6
    if (sockaddr->family == AF_INET6 &&
        IN6_IS_ADDR_V4MAPPED((struct in6_addr *)sockaddr->ipaddr_ptr)) {
        /* This is an IPv4-mapped IPv6 address; drop the leading
         * part of the address string so we're left with the familiar
         * IPv4 format.
         */

        /* use memmove since the memory areas overlap */
        memmove( addr, addr+7, strlen(addr+7) + 1);/* +1 for \0 */
	
	/* old code
        *addr += strlen("::ffff:");
	*/
    }
#endif
    return APR_SUCCESS;
}

static apr_socket_t *
create_net_client(apr_pool_t *context, int type, char *ipaddr, apr_port_t port)
{
  apr_sockaddr_t *remotesa = NULL;
  apr_socket_t *sock = NULL;
  apr_status_t status;
  int family = APR_UNSPEC;

  status = apr_sockaddr_info_get(&remotesa, ipaddr, APR_UNSPEC, port, 0, context);
  if(status!= APR_SUCCESS)
    {
      return NULL;
    }
  family = remotesa->sa.sin.sin_family;

  /* Created the socket */
  status = apr_socket_create(&sock, family, type, context);
  if(status != APR_SUCCESS)
    {
      return NULL;
    }

  /* Connect the socket to the address */
  status = apr_connect(sock, remotesa);
  if(status != APR_SUCCESS)
    {
      apr_socket_close(sock);
      return NULL;
    }

  return sock;
}

apr_socket_t *
create_udp_client(apr_pool_t *context, char *ipaddr, apr_port_t port)
{
  return create_net_client(context, SOCK_DGRAM, ipaddr, port);
}

static apr_socket_t *
create_net_server(apr_pool_t *context, int type, apr_port_t port, char *bind)
{
  apr_sockaddr_t *localsa = NULL;
  apr_socket_t *sock = NULL;
  apr_status_t stat;
  int family = APR_UNSPEC;

  if(bind)
    {
      stat = apr_sockaddr_info_get(&localsa, bind, APR_UNSPEC, port, 0, context);
      if (stat != APR_SUCCESS)
        return NULL;

      family = localsa->sa.sin.sin_family;
    }

  stat = apr_socket_create(&sock, family, type, context);
  if( stat != APR_SUCCESS )
    return NULL;

  /* Setup to be non-blocking */
  stat = apr_setsocketopt(sock, APR_SO_NONBLOCK, 1);
  if (stat != APR_SUCCESS)
    {
      apr_socket_close(sock);
      return NULL;
    }

  stat = apr_setsocketopt(sock, APR_SO_REUSEADDR, 1);
  if (stat != APR_SUCCESS)
    {
      apr_socket_close(sock);
      return NULL;
    }

  if(!localsa)
    {
      apr_socket_addr_get(&localsa, APR_LOCAL, sock);
      apr_sockaddr_port_set(localsa, port);
    }

  stat = apr_bind(sock, localsa);
  if( stat != APR_SUCCESS)
    {
       apr_socket_close(sock);
       /*
       fprintf(stderr, "Could not bind: %s\n", apr_strerror(stat, buf, sizeof buf));
       */
       return NULL;
    }

  return sock;
}

apr_socket_t *
create_udp_server(apr_pool_t *context, apr_port_t port, char *bind)
{
  return create_net_server(context, SOCK_DGRAM, port, bind);
}

apr_socket_t *
create_tcp_server(apr_pool_t *context, apr_port_t port, char *bind, char *interface)
{
  apr_socket_t *sock = create_net_server(context, SOCK_STREAM, port, bind);
  if(!sock)
    {
      return NULL;
    }
  if(apr_listen(sock,5) != APR_SUCCESS) 
    {
      return NULL;
    }
  return sock;
}

static int
mcast_set_ttl(apr_socket_t *socket, int val)
{
  apr_sockaddr_t *sa;

  apr_socket_addr_get(&sa, APR_LOCAL, socket);
  if(!sa)
    {
      return -1;
    }
  switch (sa->family)
    {
	case APR_INET: {
		u_char		ttl;

		ttl = val;
		return(setsockopt(socket->socketdes, IPPROTO_IP, IP_MULTICAST_TTL,
						  &ttl, sizeof(ttl)));
	}

#if	APR_HAVE_IPV6
	case APR_INET6: {
		int		hop;

		hop = val;
		return(setsockopt(socket->socketdes, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
						  &hop, sizeof(hop)));
	}
#endif

	default:
		errno = EPROTONOSUPPORT;
		return(-1);
	}
}

static apr_status_t
mcast_join( apr_pool_t *context, apr_socket_t *sock, char *mcast_channel, apr_port_t port, char *ifname )
{
  apr_status_t status;
  int rval;
  apr_sockaddr_t *sa;

  status = apr_sockaddr_info_get(&sa, mcast_channel , APR_UNSPEC, port, 0, context);
  if(status != APR_SUCCESS)
    {
      return status;
    }

  switch( sa->family ) /* (*sa)->sa.sin.sin_family */
    {
    case APR_INET:
	{
	  struct ip_mreq mreq[1];
	  struct ifreq ifreq[1];

	  /* &((*sa)->sa.sin.sin_addr */
	  memcpy(&mreq->imr_multiaddr, &(sa->sa.sin.sin_addr), 
		 sizeof mreq->imr_multiaddr);

	  memset(&ifreq,0, sizeof(ifreq));
	  if(ifname)
	    {
              memset(ifreq, 0, sizeof(struct ifreq));
              strncpy(ifreq->ifr_name, ifname, IFNAMSIZ);
              if(ioctl(sock->socketdes, SIOCGIFADDR, ifreq) == -1)
		{
		  return APR_EGENERAL;
		}
	    }
	  else
	    {
	      /* wildcard address (let the kernel decide) */
	      mreq->imr_interface.s_addr = htonl(INADDR_ANY);
	    }

	  memcpy(&mreq->imr_interface, 
		 &((struct sockaddr_in *)&ifreq->ifr_addr)->sin_addr, 
		 sizeof mreq->imr_interface);

	  rval = setsockopt(sock->socketdes, IPPROTO_IP, IP_ADD_MEMBERSHIP, 
			    mreq, sizeof mreq);
	  if(rval<0)
	    {
	      return APR_EGENERAL;
	    }
	  break;
	}
#if APR_HAVE_IPV6
    case APR_INET6:
	{
	  struct ipv6_mreq mreq[1];
	  struct ifreq ifreq[1];

	  /* &((*sa)->sa.sin6.sin6_addr)*/
          memcpy(&mreq->ipv6mr_multiaddr, &(sa->sa.sin6.sin6_addr),
		                  sizeof mreq->ipv6mr_multiaddr);

          memset(&ifreq,0, sizeof(ifreq));
	  if(ifname)
	    {
	      strncpy(ifreq->ifr_name, ifname, IFNAMSIZ);
	    }

	  if (ioctl(sock->socketdes, SIOCGIFADDR, ifreq) == -1)
	                return -1;

	  rval = setsockopt(sock->socketdes, IPPROTO_IPV6, IPV6_JOIN_GROUP, mreq, sizeof mreq);
	  break;
	}
#endif
    default:
      /* Set errno to EPROTONOSUPPORT */
      return -1;
    }

  return APR_SUCCESS;
}

apr_socket_t *
create_mcast_client(apr_pool_t *context, char *mcast_ip, apr_port_t port, int ttl)
{
  apr_socket_t *socket = create_udp_client(context, mcast_ip, port);
  if(!socket)
    {
      return NULL;
    }
  return socket;
}

apr_socket_t *
create_mcast_server(apr_pool_t *context, char *mcast_ip, apr_port_t port, char *bind, char *interface)
{
  apr_status_t status = APR_SUCCESS;
  /* NOTE: If bind is set to mcast_ip in the configuration file, then we will bind the 
   * the multicast address to the socket as well as the port and prevent any 
   * datagrams that might be delivered to this port from being processed. Otherwise,
   * packets destined to the same port (but a different multicast/unicast channel) will be
   * processed. */
  apr_socket_t *socket = create_udp_server(context, port, bind);
  if(!socket)
    {
      return NULL;
    }

  /* TODO: We can probe for a list of interfaces and perform multiple join calls for the same
   * socket to have it listen for multicast traffic on all interfaces (important for
   * multihomed machines). */
  if(interface && !apr_strnatcasecmp(interface, "ALL"))
    {
      /* for(each interface)
       * {
       *   mcast_join(...);
       * }
       */
    }
  else
    {
      status = mcast_join(context,  socket, mcast_ip, port, interface );
    }

  return status == APR_SUCCESS? socket: NULL;
}