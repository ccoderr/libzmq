/*
Copyright (c) 2007-2016 Contributors as noted in the AUTHORS file

This file is part of libzmq, the ZeroMQ core engine in C++.

libzmq is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (LGPL) as published
by the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

As a special exception, the Contributors give you permission to link
this library with independent modules to produce an executable,
regardless of the license terms of these independent modules, and to
copy and distribute the resulting executable under terms of your choice,
provided that you also meet, for each linked independent module, the
terms and conditions of the license of that module. An independent
module is a module which is not derived from or based on this library.
If you modify this library, you must extend this exception to your
version of the library.

libzmq is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "precompiled.hpp"

#if !defined ZMQ_HAVE_WINDOWS
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef ZMQ_HAVE_VXWORKS
#include <sockLib.h>
#endif
#endif

#include "udp_engine.hpp"
#include "session_base.hpp"
#include "v2_protocol.hpp"
#include "err.hpp"
#include "ip.hpp"

//  OSX uses a different name for this socket option
#ifndef IPV6_ADD_MEMBERSHIP
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#endif

zmq::udp_engine_t::udp_engine_t (const options_t &options_) :
    plugged (false),
    fd (-1),
    session (NULL),
    handle ((handle_t) NULL),
    address (NULL),
    options (options_),
    send_enabled (false),
    recv_enabled (false)
{
}

zmq::udp_engine_t::~udp_engine_t ()
{
    zmq_assert (!plugged);

    if (fd != retired_fd) {
#ifdef ZMQ_HAVE_WINDOWS
        int rc = closesocket (fd);
        wsa_assert (rc != SOCKET_ERROR);
#else
        int rc = close (fd);
        errno_assert (rc == 0);
#endif
        fd = retired_fd;
    }
}

int zmq::udp_engine_t::init (address_t *address_, bool send_, bool recv_)
{
    zmq_assert (address_);
    zmq_assert (send_ || recv_);
    send_enabled = send_;
    recv_enabled = recv_;
    address = address_;

    fd = open_socket (address->resolved.udp_addr->family (), SOCK_DGRAM,
                      IPPROTO_UDP);
    if (fd == retired_fd)
        return -1;

    unblock_socket (fd);

    return 0;
}

void zmq::udp_engine_t::plug (io_thread_t *io_thread_, session_base_t *session_)
{
    zmq_assert (!plugged);
    plugged = true;

    zmq_assert (!session);
    zmq_assert (session_);
    session = session_;

    //  Connect to I/O threads poller object.
    io_object_t::plug (io_thread_);
    handle = add_fd (fd);

    // Bind the socket to a device if applicable
    if (!options.bound_device.empty ())
        bind_to_device (fd, options.bound_device);

    if (send_enabled) {
        if (!options.raw_socket) {
            const ip_addr_t *out = address->resolved.udp_addr->target_addr ();
            out_address = out->as_sockaddr ();
            out_addrlen = out->sockaddr_len ();

            if (out->is_multicast ()) {
                int level;
                int optname;

                if (out->family () == AF_INET6) {
                    level = IPPROTO_IPV6;
                    optname = IPV6_MULTICAST_LOOP;
                } else {
                    level = IPPROTO_IP;
                    optname = IP_MULTICAST_LOOP;
                }

                int loop = options.multicast_loop;
                int rc = setsockopt (fd, level, optname, (char *) &loop,
                                     sizeof (loop));

#ifdef ZMQ_HAVE_WINDOWS
                wsa_assert (rc != SOCKET_ERROR);
#else
                errno_assert (rc == 0);
#endif
            }
        } else {
            /// XXX fixme ?
            out_address = (sockaddr *) &raw_address;
            out_addrlen = sizeof (sockaddr_in);
        }

        set_pollout (handle);
    }

    if (recv_enabled) {
        int on = 1;
        int rc =
          setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof (on));
#ifdef ZMQ_HAVE_WINDOWS
        wsa_assert (rc != SOCKET_ERROR);
#else
        errno_assert (rc == 0);
#endif

        const ip_addr_t *bind_addr = address->resolved.udp_addr->bind_addr ();
        ip_addr_t any = ip_addr_t::any (bind_addr->family ());
        const ip_addr_t *real_bind_addr;

        bool multicast = address->resolved.udp_addr->is_mcast ();

        if (multicast) {
            //  In multicast we should bind ANY and use the mreq struct to
            //  specify the interface
            any.set_port (bind_addr->port ());

            real_bind_addr = &any;
        } else {
            real_bind_addr = bind_addr;
        }

#ifdef ZMQ_HAVE_VXWORKS
        rc = bind (fd, (sockaddr *) real_bind_addr->as_sockaddr (),
                   real_bind_addr->sockaddr_len ());
#else
        rc = bind (fd, real_bind_addr->as_sockaddr (),
                   real_bind_addr->sockaddr_len ());

#endif
#ifdef ZMQ_HAVE_WINDOWS
        wsa_assert (rc != SOCKET_ERROR);
#else
        errno_assert (rc == 0);
#endif

        if (multicast) {
            const ip_addr_t *mcast_addr =
              address->resolved.udp_addr->target_addr ();

            if (mcast_addr->family () == AF_INET) {
                struct ip_mreq mreq;
                mreq.imr_multiaddr = mcast_addr->ipv4.sin_addr;
                mreq.imr_interface = bind_addr->ipv4.sin_addr;

                rc = setsockopt (fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                 (char *) &mreq, sizeof (mreq));

                errno_assert (rc == 0);
            } else if (mcast_addr->family () == AF_INET6) {
                struct ipv6_mreq mreq;
                int iface = address->resolved.udp_addr->bind_if ();

                zmq_assert (iface >= -1);

                mreq.ipv6mr_multiaddr = mcast_addr->ipv6.sin6_addr;
                mreq.ipv6mr_interface = iface;

                rc = setsockopt (fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                                 (char *) &mreq, sizeof (mreq));

                errno_assert (rc == 0);
            } else {
                //  Shouldn't happen
                abort ();
            }

#ifdef ZMQ_HAVE_WINDOWS
            wsa_assert (rc != SOCKET_ERROR);
#else
            errno_assert (rc == 0);
#endif
        }
        set_pollin (handle);

        //  Call restart output to drop all join/leave commands
        restart_output ();
    }
}

void zmq::udp_engine_t::terminate ()
{
    zmq_assert (plugged);
    plugged = false;

    rm_fd (handle);

    //  Disconnect from I/O threads poller object.
    io_object_t::unplug ();

    delete this;
}

void zmq::udp_engine_t::sockaddr_to_msg (zmq::msg_t *msg, sockaddr_in *addr)
{
    char *name = inet_ntoa (addr->sin_addr);

    char port[6];
    sprintf (port, "%d", (int) ntohs (addr->sin_port));

    int size =
      (int) strlen (name) + (int) strlen (port) + 1 + 1; //  Colon + NULL
    int rc = msg->init_size (size);
    errno_assert (rc == 0);
    msg->set_flags (msg_t::more);
    char *address = (char *) msg->data ();

    strcpy (address, name);
    strcat (address, ":");
    strcat (address, port);
}

int zmq::udp_engine_t::resolve_raw_address (char *name_, size_t length_)
{
    memset (&raw_address, 0, sizeof raw_address);

    const char *delimiter = NULL;

    // Find delimiter, cannot use memrchr as it is not supported on windows
    if (length_ != 0) {
        int chars_left = (int) length_;
        char *current_char = name_ + length_;
        do {
            if (*(--current_char) == ':') {
                delimiter = current_char;
                break;
            }
        } while (--chars_left != 0);
    }

    if (!delimiter) {
        errno = EINVAL;
        return -1;
    }

    std::string addr_str (name_, delimiter - name_);
    std::string port_str (delimiter + 1, name_ + length_ - delimiter - 1);

    //  Parse the port number (0 is not a valid port).
    uint16_t port = (uint16_t) atoi (port_str.c_str ());
    if (port == 0) {
        errno = EINVAL;
        return -1;
    }

    raw_address.sin_family = AF_INET;
    raw_address.sin_port = htons (port);
    raw_address.sin_addr.s_addr = inet_addr (addr_str.c_str ());

    if (raw_address.sin_addr.s_addr == INADDR_NONE) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

void zmq::udp_engine_t::out_event ()
{
    msg_t group_msg;
    int rc = session->pull_msg (&group_msg);
    errno_assert (rc == 0 || (rc == -1 && errno == EAGAIN));

    if (rc == 0) {
        msg_t body_msg;
        rc = session->pull_msg (&body_msg);

        size_t group_size = group_msg.size ();
        size_t body_size = body_msg.size ();
        size_t size;

        if (options.raw_socket) {
            rc = resolve_raw_address ((char *) group_msg.data (), group_size);

            //  We discard the message if address is not valid
            if (rc != 0) {
                rc = group_msg.close ();
                errno_assert (rc == 0);

                body_msg.close ();
                errno_assert (rc == 0);

                return;
            }

            size = body_size;

            memcpy (out_buffer, body_msg.data (), body_size);
        } else {
            size = group_size + body_size + 1;

            // TODO: check if larger than maximum size
            out_buffer[0] = (unsigned char) group_size;
            memcpy (out_buffer + 1, group_msg.data (), group_size);
            memcpy (out_buffer + 1 + group_size, body_msg.data (), body_size);
        }

        rc = group_msg.close ();
        errno_assert (rc == 0);

        body_msg.close ();
        errno_assert (rc == 0);

#ifdef ZMQ_HAVE_WINDOWS
        rc = sendto (fd, (const char *) out_buffer, (int) size, 0, out_address,
                     (int) out_addrlen);
        wsa_assert (rc != SOCKET_ERROR);
#elif defined ZMQ_HAVE_VXWORKS
        rc = sendto (fd, (caddr_t) out_buffer, size, 0,
                     (sockaddr *) out_address, (int) out_addrlen);
        errno_assert (rc != -1);
#else
        rc = sendto (fd, out_buffer, size, 0, out_address, out_addrlen);
        errno_assert (rc != -1);
#endif
    } else
        reset_pollout (handle);
}

const char *zmq::udp_engine_t::get_endpoint () const
{
    return "";
}

void zmq::udp_engine_t::restart_output ()
{
    //  If we don't support send we just drop all messages
    if (!send_enabled) {
        msg_t msg;
        while (session->pull_msg (&msg) == 0)
            msg.close ();
    } else {
        set_pollout (handle);
        out_event ();
    }
}

void zmq::udp_engine_t::in_event ()
{
    struct sockaddr_in in_address;
    socklen_t in_addrlen = sizeof (sockaddr_in);
#ifdef ZMQ_HAVE_WINDOWS
    int nbytes = recvfrom (fd, (char *) in_buffer, MAX_UDP_MSG, 0,
                           (sockaddr *) &in_address, &in_addrlen);
    const int last_error = WSAGetLastError ();
    if (nbytes == SOCKET_ERROR) {
        wsa_assert (last_error == WSAENETDOWN || last_error == WSAENETRESET
                    || last_error == WSAEWOULDBLOCK);
        return;
    }
#elif defined ZMQ_HAVE_VXWORKS
    int nbytes = recvfrom (fd, (char *) in_buffer, MAX_UDP_MSG, 0,
                           (sockaddr *) &in_address, (int *) &in_addrlen);
    if (nbytes == -1) {
        errno_assert (errno != EBADF && errno != EFAULT && errno != ENOMEM
                      && errno != ENOTSOCK);
        return;
    }
#else
    int nbytes = recvfrom (fd, in_buffer, MAX_UDP_MSG, 0,
                           (sockaddr *) &in_address, &in_addrlen);
    if (nbytes == -1) {
        errno_assert (errno != EBADF && errno != EFAULT && errno != ENOMEM
                      && errno != ENOTSOCK);
        return;
    }
#endif
    int rc;
    int body_size;
    int body_offset;
    msg_t msg;

    if (options.raw_socket) {
        sockaddr_to_msg (&msg, &in_address);

        body_size = nbytes;
        body_offset = 0;
    } else {
        char *group_buffer = (char *) in_buffer + 1;
        int group_size = in_buffer[0];

        rc = msg.init_size (group_size);
        errno_assert (rc == 0);
        msg.set_flags (msg_t::more);
        memcpy (msg.data (), group_buffer, group_size);

        //  This doesn't fit, just ingore
        if (nbytes - 1 < group_size)
            return;

        body_size = nbytes - 1 - group_size;
        body_offset = 1 + group_size;
    }
    // Push group description to session
    rc = session->push_msg (&msg);
    errno_assert (rc == 0 || (rc == -1 && errno == EAGAIN));

    //  Group description message doesn't fit in the pipe, drop
    if (rc != 0) {
        rc = msg.close ();
        errno_assert (rc == 0);

        reset_pollin (handle);
        return;
    }

    rc = msg.close ();
    errno_assert (rc == 0);
    rc = msg.init_size (body_size);
    errno_assert (rc == 0);
    memcpy (msg.data (), in_buffer + body_offset, body_size);

    // Push message body to session
    rc = session->push_msg (&msg);
    // Message body doesn't fit in the pipe, drop and reset session state
    if (rc != 0) {
        rc = msg.close ();
        errno_assert (rc == 0);

        session->reset ();
        reset_pollin (handle);
        return;
    }

    rc = msg.close ();
    errno_assert (rc == 0);
    session->flush ();
}

void zmq::udp_engine_t::restart_input ()
{
    if (!recv_enabled)
        return;

    set_pollin (handle);
    in_event ();
}
