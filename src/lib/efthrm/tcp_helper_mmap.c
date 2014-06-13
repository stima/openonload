/*
** Copyright 2005-2012  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/**************************************************************************\
** <L5_PRIVATE L5_SOURCE>
**   Copyright: (c) Level 5 Networks Limited.
**      Author: djr
**     Started: 2006/06/16
** Description: TCP helper resource
** </L5_PRIVATE>
\**************************************************************************/

/*! \cidoxg_driver_efab */
#include <onload/debug.h>
#include <onload/cplane.h>
#include <ci/internal/cplane_handle.h>
#include <onload/tcp_helper.h>
#include <ci/efch/mmap.h>



static int tcp_helper_rm_mmap_mem(tcp_helper_resource_t* trs,
                                  unsigned long* bytes,
                                  void* opaque, int* map_num,
                                  unsigned long* offset)
{
  int rc = 0;
  OO_DEBUG_SHM(ci_log("mmap mem %lx", *offset));
  OO_DEBUG_VM(ci_log("tcp_helper_rm_mmap_mem: %u "
                     "map_num=%d bytes=0x%lx offset=0x%lx",
                     trs->id, *map_num, *bytes, *offset));

  /* map contiguous shared state */
  rc = ci_contig_shmbuf_mmap(&trs->netif.state_buf, 0, bytes, opaque,
                             map_num, offset);
  if( rc < 0 )  goto out;

  OO_DEBUG_MEMSIZE(ci_log("%s: taken %d bytes for contig shmbuf leaving %lu",
                          __FUNCTION__,
                          (int) ci_contig_shmbuf_size(&trs->netif.state_buf),
                          *bytes));

#ifdef CI_HAVE_OS_NOPAGE
  rc = ci_shmbuf_mmap(&trs->netif.pages_buf, 0, bytes, opaque,
                           map_num, offset);
  if( rc < 0 )  goto out;
  OO_DEBUG_MEMSIZE(ci_log("after mapping page buf have %ld", *bytes));
#endif

  /* map the control plane shared data areas */
  rc = cicp_mmap(CICP_HANDLE(&trs->netif), bytes, opaque, map_num, offset);
  OO_DEBUG_MEMSIZE(ci_log("after mapping cplane sdata have %ld", *bytes));

 out:
  return rc;
}


static int tcp_helper_rm_mmap_io(tcp_helper_resource_t* trs,
                                 unsigned long* bytes,
                                 void* opaque, int* map_num,
                                 unsigned long* offset)
{
  int rc, intf_i;

  OO_DEBUG_SHM(ci_log("mmap io %lx", *offset));
  OO_DEBUG_VM(ci_log("tcp_helper_rm_mmap_io: %u "
                     "map_num=%d bytes=0x%lx offset=0x%lx",
                     trs->id, *map_num, *bytes, *offset));

  OO_STACK_FOR_EACH_INTF_I(&trs->netif, intf_i) {
    rc = efab_vi_resource_mmap(trs->nic[intf_i].vi_rs, bytes, opaque,
                               map_num, offset, 0);
    if( rc < 0 )
      return rc;
  }

  return 0;
}


static int tcp_helper_rm_mmap_buf(tcp_helper_resource_t* trs,
                                  unsigned long* bytes,
                                  void* opaque, int* map_num,
                                  unsigned long* offset)
{
  int intf_i, rc;
  ci_netif* ni;
  ci_netif_state* ns;

  ni = &trs->netif;
  ns = ni->state;
  ci_assert(ns);
  OO_DEBUG_SHM(ci_log("mmap buf %lx %lx", *offset, *bytes));
  OO_DEBUG_VM(ci_log("tcp_helper_rm_mmap_buf: %u "
                     "map_num=%d bytes=0x%lx offset=0x%lx",
                     trs->id, *map_num, *bytes, *offset));

  OO_STACK_FOR_EACH_INTF_I(ni, intf_i ) {
    rc = efab_vi_resource_mmap(trs->nic[intf_i].vi_rs, bytes, opaque,
                               map_num, offset, 1);
    if( rc < 0 )  return rc;
  }

  /* Reserve space for packet buffers */
#ifdef CI_HAVE_OS_NOPAGE
  {
    unsigned long n;
    unsigned i;

    for( i = 0; i < ns->pkt_sets_max; i++ )
      if (ni->pkt_bufs[i] != NULL) {
        rc = efab_iobufset_resource_mmap(ni->pkt_rs[i], bytes, opaque,
					 map_num, offset, 0);
        if( rc < 0 )  return rc;
      }
      else {
        n = ns->pkt_set_bytes;
        n = CI_MIN(n, *bytes);
        *bytes -= n;
      }
  }
#endif

  return 0;
}


#ifndef CI_HAVE_OS_NOPAGE

static int tcp_helper_rm_mmap_page(tcp_helper_resource_t* trs,
                                   unsigned long* bytes,
                                   void* opaque, int* map_num,
                                   unsigned long* offset, unsigned index)
{
  OO_DEBUG_SHM(ci_log("%s: offset=%lx index=%u", __FUNCTION__, *offset, index));

  if( index < trs->netif.k_shmbufs_n )
    return ci_shmbuf_mmap(trs->netif.k_shmbufs[index], 0, bytes,
                          opaque, map_num, offset);

  DEBUGERR(ci_log("%s: bad index=%d n=%u", __FUNCTION__, index,
                  trs->netif.k_shmbufs_n));
  return -EINVAL;
}


static int tcp_helper_rm_mmap_pktbuf(tcp_helper_resource_t* trs,
                                     unsigned long* bytes, void* opaque,
                                     int* map_num, unsigned long* offset,
                                     unsigned index)
{
  OO_DEBUG_SHM(ci_log("%s: offset=%lx index=%u", __FUNCTION__, *offset, index));

  if( index < trs->netif.pkt_sets_n ) {
    return efab_iobufset_resource_mmap(trs->netif.pkt_rs[index], bytes, opaque,
                                       map_num, offset, index);
  }

  DEBUGERR(ci_log("%s: bad index=%d n=%u", __FUNCTION__, index,
                  trs->netif.pkt_sets_n));
  return -EINVAL;
}

#endif


int efab_tcp_helper_rm_mmap(tcp_helper_resource_t* trs, unsigned long* bytes,
                            void* opaque, int* map_num,
                            unsigned long* offset, int map_id)
{
  int map_type = map_id &~ CI_NETIF_MMAP_ID_ID_MASK;
  int rc;

  TCP_HELPER_RESOURCE_ASSERT_VALID(trs, 0);
  ci_assert(*bytes > 0);

  OO_DEBUG_VM(ci_log("tcp_helper_rm_mmap: %u "
                     "map_num=%d bytes=0x%lx offset=0x%lx map_id=%x",
                     trs->id, *map_num, *bytes, *offset, map_id));

  switch( map_type ) {
#ifndef CI_HAVE_OS_NOPAGE
  case CI_NETIF_MMAP_ID_PAGES:
    rc = tcp_helper_rm_mmap_page(trs, bytes, opaque, map_num, offset,
                                 map_id & CI_NETIF_MMAP_ID_ID_MASK);
    break;
  case CI_NETIF_MMAP_ID_PKTS:
    rc = tcp_helper_rm_mmap_pktbuf(trs, bytes, opaque, map_num, offset,
                                   map_id & CI_NETIF_MMAP_ID_ID_MASK);
    break;
#endif
  default:
    switch( map_id ) {
    case CI_NETIF_MMAP_ID_STATE:
      rc = tcp_helper_rm_mmap_mem(trs, bytes, opaque, map_num, offset);
      break;
    case CI_NETIF_MMAP_ID_IO:
      rc = tcp_helper_rm_mmap_io(trs, bytes, opaque, map_num, offset);
      break;
    case CI_NETIF_MMAP_ID_IOBUFS:
      rc = tcp_helper_rm_mmap_buf(trs, bytes, opaque, map_num, offset);
      break;
    default:
      rc = -EINVAL;
      break;
    }
    break;
  }

  if( rc == 0 )  return 0;

  OO_DEBUG_VM(ci_log("%s: failed map_id=%x rc=%d", __FUNCTION__, map_id, rc));
  return rc;
}

/*! \cidoxg_end */