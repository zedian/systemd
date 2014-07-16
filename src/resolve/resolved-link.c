/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <net/if.h>

#include "sd-network.h"
#include "strv.h"
#include "resolved-link.h"

/* RFC 4795 Section 2.8. suggests a TTL of 30s by default */
#define LLMNR_DEFAULT_TTL (30)

static void link_address_add_rrs(LinkAddress *a);

int link_new(Manager *m, Link **ret, int ifindex) {
        _cleanup_(link_freep) Link *l = NULL;
        int r;

        assert(m);
        assert(ifindex > 0);

        r = hashmap_ensure_allocated(&m->links, NULL, NULL);
        if (r < 0)
                return r;

        l = new0(Link, 1);
        if (!l)
                return -ENOMEM;

        l->ifindex = ifindex;

        r = hashmap_put(m->links, INT_TO_PTR(ifindex), l);
        if (r < 0)
                return r;

        l->manager = m;

        if (ret)
                *ret = l;
        l = NULL;

        return 0;
}

Link *link_free(Link *l) {

        if (!l)
                return NULL;

        while (l->addresses)
                link_address_free(l->addresses);

        if (l->manager)
                hashmap_remove(l->manager->links, INT_TO_PTR(l->ifindex));

        dns_scope_free(l->unicast_scope);
        dns_scope_free(l->llmnr_ipv4_scope);
        dns_scope_free(l->llmnr_ipv6_scope);

        while (l->dns_servers)
                dns_server_free(l->dns_servers);

        free(l);
        return NULL;
}

static void link_allocate_scopes(Link *l) {
        int r;

        assert(l);

        if (l->dns_servers) {
                if (!l->unicast_scope) {
                        r = dns_scope_new(l->manager, &l->unicast_scope, l, DNS_PROTOCOL_DNS, AF_UNSPEC);
                        if (r < 0)
                                log_warning("Failed to allocate DNS scope: %s", strerror(-r));
                }
        } else
                l->unicast_scope = dns_scope_free(l->unicast_scope);

        if (link_relevant(l, AF_INET) && (l->flags & IFF_MULTICAST) && l->manager->use_llmnr) {
                if (!l->llmnr_ipv4_scope) {
                        r = dns_scope_new(l->manager, &l->llmnr_ipv4_scope, l, DNS_PROTOCOL_LLMNR, AF_INET);
                        if (r < 0)
                                log_warning("Failed to allocate LLMNR IPv4 scope: %s", strerror(-r));
                }
        } else
                l->llmnr_ipv4_scope = dns_scope_free(l->llmnr_ipv4_scope);

        if (link_relevant(l, AF_INET6) && (l->flags & IFF_MULTICAST) && l->manager->use_llmnr) {
                if (!l->llmnr_ipv6_scope) {
                        r = dns_scope_new(l->manager, &l->llmnr_ipv6_scope, l, DNS_PROTOCOL_LLMNR, AF_INET6);
                        if (r < 0)
                                log_warning("Failed to allocate LLMNR IPv6 scope: %s", strerror(-r));
                }
        } else
                l->llmnr_ipv6_scope = dns_scope_free(l->llmnr_ipv6_scope);
}

static void link_add_rrs(Link *l) {
        LinkAddress *a;

        LIST_FOREACH(addresses, a, l->addresses)
                link_address_add_rrs(a);
}

int link_update_rtnl(Link *l, sd_rtnl_message *m) {
        const char *n = NULL;
        int r;

        assert(l);
        assert(m);

        r = sd_rtnl_message_link_get_flags(m, &l->flags);
        if (r < 0)
                return r;

        sd_rtnl_message_read_u32(m, IFLA_MTU, &l->mtu);

        if (sd_rtnl_message_read_string(m, IFLA_IFNAME, &n) >= 0) {
                strncpy(l->name, n, sizeof(l->name));
                char_array_0(l->name);
        }

        link_allocate_scopes(l);
        link_add_rrs(l);

        return 0;
}

static int link_update_dns_servers(Link *l) {
        _cleanup_strv_free_ char **nameservers = NULL;
        char **nameserver;
        DnsServer *s, *nx;
        int r;

        assert(l);

        LIST_FOREACH(servers, s, l->dns_servers)
                s->marked = true;

        r = sd_network_get_dns(l->ifindex, &nameservers);
        if (r < 0)
                goto clear;

        STRV_FOREACH(nameserver, nameservers) {
                union in_addr_union a;
                int family;

                r = in_addr_from_string_auto(*nameserver, &family, &a);
                if (r < 0)
                        goto clear;

                s = link_find_dns_server(l, family, &a);
                if (s)
                        s->marked = false;
                else {
                        r = dns_server_new(l->manager, NULL, l, family, &a);
                        if (r < 0)
                                goto clear;
                }
        }

        LIST_FOREACH_SAFE(servers, s, nx, l->dns_servers)
                if (s->marked)
                        dns_server_free(s);

        return 0;

clear:
        while (l->dns_servers)
                dns_server_free(l->dns_servers);

        return r;
}

int link_update_monitor(Link *l) {
        assert(l);

        link_update_dns_servers(l);
        link_allocate_scopes(l);
        link_add_rrs(l);

        return 0;
}

bool link_relevant(Link *l, int family) {
        _cleanup_free_ char *state = NULL;
        LinkAddress *a;

        assert(l);

        /* A link is relevant if it isn't a loopback device and has at
         * least one relevant IP address */

        if (l->flags & IFF_LOOPBACK)
                return false;

        sd_network_get_link_operational_state(l->ifindex, &state);
        if (state && !STR_IN_SET(state, "unknown", "degraded", "routable"))
                return false;

        LIST_FOREACH(addresses, a, l->addresses)
                if (a->family == family && link_address_relevant(a))
                        return true;

        return false;
}

LinkAddress *link_find_address(Link *l, int family, const union in_addr_union *in_addr) {
        LinkAddress *a;

        assert(l);

        LIST_FOREACH(addresses, a, l->addresses)
                if (a->family == family && in_addr_equal(family, &a->in_addr, in_addr))
                        return a;

        return NULL;
}

DnsServer* link_find_dns_server(Link *l, int family, const union in_addr_union *in_addr) {
        DnsServer *s;

        assert(l);

        LIST_FOREACH(servers, s, l->dns_servers)
                if (s->family == family && in_addr_equal(family, &s->address, in_addr))
                        return s;
        return NULL;
}

DnsServer *link_get_dns_server(Link *l) {
        assert(l);

        if (!l->current_dns_server)
                l->current_dns_server = l->dns_servers;

        return l->current_dns_server;
}

void link_next_dns_server(Link *l) {
        assert(l);

        /* Switch to the next DNS server */

        if (!l->current_dns_server) {
                l->current_dns_server = l->dns_servers;
                if (l->current_dns_server)
                        return;
        }

        if (!l->current_dns_server)
                return;

        if (l->current_dns_server->servers_next) {
                l->current_dns_server = l->current_dns_server->servers_next;
                return;
        }

        l->current_dns_server = l->dns_servers;
}

int link_address_new(Link *l, LinkAddress **ret, int family, const union in_addr_union *in_addr) {
        LinkAddress *a;

        assert(l);
        assert(in_addr);

        a = new0(LinkAddress, 1);
        if (!a)
                return -ENOMEM;

        a->family = family;
        a->in_addr = *in_addr;

        a->link = l;
        LIST_PREPEND(addresses, l->addresses, a);

        if (ret)
                *ret = a;

        return 0;
}

LinkAddress *link_address_free(LinkAddress *a) {
        if (!a)
                return NULL;

        if (a->link) {
                LIST_REMOVE(addresses, a->link->addresses, a);

                if (a->llmnr_address_rr) {

                        if (a->family == AF_INET && a->link->llmnr_ipv4_scope)
                                dns_zone_remove_rr(&a->link->llmnr_ipv4_scope->zone, a->llmnr_address_rr);
                        else if (a->family == AF_INET6 && a->link->llmnr_ipv6_scope)
                                dns_zone_remove_rr(&a->link->llmnr_ipv6_scope->zone, a->llmnr_address_rr);

                        dns_resource_record_unref(a->llmnr_address_rr);
                }

                if (a->llmnr_ptr_rr) {
                        if (a->family == AF_INET && a->link->llmnr_ipv4_scope)
                                dns_zone_remove_rr(&a->link->llmnr_ipv4_scope->zone, a->llmnr_ptr_rr);
                        else if (a->family == AF_INET6 && a->link->llmnr_ipv6_scope)
                                dns_zone_remove_rr(&a->link->llmnr_ipv6_scope->zone, a->llmnr_ptr_rr);

                        dns_resource_record_unref(a->llmnr_ptr_rr);
                }
        }

        free(a);
        return NULL;
}

static void link_address_add_rrs(LinkAddress *a) {
        int r;

        assert(a);

        if (a->family == AF_INET && a->link->llmnr_ipv4_scope) {

                if (!a->link->manager->host_ipv4_key) {
                        a->link->manager->host_ipv4_key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_A, a->link->manager->hostname);
                        if (!a->link->manager->host_ipv4_key) {
                                r = -ENOMEM;
                                goto fail;
                        }
                }

                if (!a->llmnr_address_rr) {
                        a->llmnr_address_rr = dns_resource_record_new(a->link->manager->host_ipv4_key);
                        if (!a->llmnr_address_rr) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        a->llmnr_address_rr->a.in_addr = a->in_addr.in;
                        a->llmnr_address_rr->ttl = LLMNR_DEFAULT_TTL;
                }

                if (!a->llmnr_ptr_rr) {
                        r = dns_resource_record_new_reverse(&a->llmnr_ptr_rr, a->family, &a->in_addr, a->link->manager->hostname);
                        if (r < 0)
                                goto fail;

                        a->llmnr_ptr_rr->ttl = LLMNR_DEFAULT_TTL;
                }

                if (link_address_relevant(a)) {
                        r = dns_zone_put(&a->link->llmnr_ipv4_scope->zone, a->llmnr_address_rr);
                        if (r < 0)
                                goto fail;

                        r = dns_zone_put(&a->link->llmnr_ipv4_scope->zone, a->llmnr_ptr_rr);
                        if (r < 0)
                                goto fail;
                } else {
                        dns_zone_remove_rr(&a->link->llmnr_ipv4_scope->zone, a->llmnr_address_rr);
                        dns_zone_remove_rr(&a->link->llmnr_ipv4_scope->zone, a->llmnr_ptr_rr);
                }
        }

        if (a->family == AF_INET6 && a->link->llmnr_ipv6_scope) {

                if (!a->link->manager->host_ipv6_key) {
                        a->link->manager->host_ipv6_key = dns_resource_key_new(DNS_CLASS_IN, DNS_TYPE_AAAA, a->link->manager->hostname);
                        if (!a->link->manager->host_ipv6_key) {
                                r = -ENOMEM;
                                goto fail;
                        }
                }

                if (!a->llmnr_address_rr) {
                        a->llmnr_address_rr = dns_resource_record_new(a->link->manager->host_ipv6_key);
                        if (!a->llmnr_address_rr) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        a->llmnr_address_rr->aaaa.in6_addr = a->in_addr.in6;
                        a->llmnr_address_rr->ttl = LLMNR_DEFAULT_TTL;
                }

                if (!a->llmnr_ptr_rr) {
                        r = dns_resource_record_new_reverse(&a->llmnr_ptr_rr, a->family, &a->in_addr, a->link->manager->hostname);
                        if (r < 0)
                                goto fail;

                        a->llmnr_ptr_rr->ttl = LLMNR_DEFAULT_TTL;
                }

                if (link_address_relevant(a)) {
                        r = dns_zone_put(&a->link->llmnr_ipv6_scope->zone, a->llmnr_address_rr);
                        if (r < 0)
                                goto fail;

                        r = dns_zone_put(&a->link->llmnr_ipv6_scope->zone, a->llmnr_ptr_rr);
                        if (r < 0)
                                goto fail;
                } else {
                        dns_zone_remove_rr(&a->link->llmnr_ipv6_scope->zone, a->llmnr_address_rr);
                        dns_zone_remove_rr(&a->link->llmnr_ipv6_scope->zone, a->llmnr_ptr_rr);
                }
        }

        return;

fail:
        log_debug("Failed to update address RRs: %s", strerror(-r));
}

int link_address_update_rtnl(LinkAddress *a, sd_rtnl_message *m) {
        int r;
        assert(a);
        assert(m);

        r = sd_rtnl_message_addr_get_flags(m, &a->flags);
        if (r < 0)
                return r;

        sd_rtnl_message_addr_get_scope(m, &a->scope);

        link_allocate_scopes(a->link);
        link_add_rrs(a->link);

        return 0;
}

bool link_address_relevant(LinkAddress *a) {
        assert(a);

        if (a->flags & IFA_F_DEPRECATED)
                return false;

        if (IN_SET(a->scope, RT_SCOPE_HOST, RT_SCOPE_NOWHERE))
                return false;

        return true;
}